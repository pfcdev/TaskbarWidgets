#include <windows.h>
#include <gdiplus.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <complex>
#include <cwctype>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include "../common/json_string.h"
#include "../common/media_visualizer_shared.h"

namespace wf = winrt::Windows::Foundation;
namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
namespace gdi = Gdiplus;
namespace visualizer = taskbar_widgets::media_visualizer;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::FtmBase;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::ClassicCom;

std::atomic_bool g_visualizerCaptureReady{};
std::atomic_bool g_visualizerHasAudio{};
std::atomic_bool g_visualizerMediaPlaying{};
std::atomic<DWORD> g_visualizerTargetProcessId{};
std::atomic<std::uint32_t> g_visualizerSampleRate{};
std::atomic<std::uint64_t> g_visualizerFrameCount{};
std::atomic<float> g_visualizerPeak{};
std::atomic<float> g_visualizerSessionPeak{};

template <typename T>
struct ComReleaser {
    void operator()(T* value) const noexcept {
        if (value) value->Release();
    }
};

template <typename T>
using unique_com = std::unique_ptr<T, ComReleaser<T>>;

struct ScopedHandle {
    HANDLE value{};
    ~ScopedHandle() {
        if (value) CloseHandle(value);
    }
    explicit operator bool() const { return value != nullptr; }
};

class SharedVisualizerWriter {
public:
    SharedVisualizerWriter() {
        mapping_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(visualizer::SharedFrame)),
            visualizer::MappingName().c_str());
        if (!mapping_) return;
        const bool alreadyExisted = GetLastError() == ERROR_ALREADY_EXISTS;

        frame_ = static_cast<visualizer::SharedFrame*>(MapViewOfFile(
            mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(visualizer::SharedFrame)));
        if (!frame_) return;

        if (!alreadyExisted ||
            frame_->magic != visualizer::kMagic ||
            frame_->version != visualizer::kVersion) {
            ZeroMemory(frame_, sizeof(*frame_));
            frame_->magic = visualizer::kMagic;
            frame_->version = visualizer::kVersion;
        }
    }

    ~SharedVisualizerWriter() {
        Publish({}, 0, false);
        if (frame_) UnmapViewOfFile(frame_);
        if (mapping_) CloseHandle(mapping_);
    }

    explicit operator bool() const { return frame_ != nullptr; }

    void Publish(const std::array<float, visualizer::kBandCount>& bands,
                 std::uint32_t sampleRate,
                 bool hasAudio) {
        if (!frame_) return;

        InterlockedIncrement64(&frame_->sequence);
        MemoryBarrier();
        frame_->magic = visualizer::kMagic;
        frame_->version = visualizer::kVersion;
        frame_->tickMilliseconds = GetTickCount64();
        frame_->sampleRate = sampleRate;
        frame_->flags = hasAudio ? 1u : 0u;
        frame_->bands = bands;
        MemoryBarrier();
        InterlockedIncrement64(&frame_->sequence);
    }

private:
    HANDLE mapping_{};
    visualizer::SharedFrame* frame_{};
};

class ProcessLoopbackActivator final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                          FtmBase,
                          IActivateAudioInterfaceCompletionHandler> {
public:
    ProcessLoopbackActivator() {
        completed_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    }

    ~ProcessLoopbackActivator() {
        if (completed_) CloseHandle(completed_);
    }

    STDMETHODIMP ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        ComPtr<IUnknown> activated;
        HRESULT activationResult = E_FAIL;
        HRESULT status = operation
                             ? operation->GetActivateResult(
                                   &activationResult, activated.GetAddressOf())
                             : E_POINTER;
        if (SUCCEEDED(status)) status = activationResult;
        if (SUCCEEDED(status)) status = activated.As(&client_);
        result_ = status;
        SetEvent(completed_);
        return S_OK;
    }

    HANDLE CompletedEvent() const { return completed_; }
    HRESULT Result() const { return result_; }
    ComPtr<IAudioClient> Client() const { return client_; }

private:
    HANDLE completed_{};
    HRESULT result_{E_PENDING};
    ComPtr<IAudioClient> client_;
};

ComPtr<IAudioClient> ActivateProcessLoopback(DWORD processId) {
    AUDIOCLIENT_ACTIVATION_PARAMS parameters{};
    parameters.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    parameters.ProcessLoopbackParams.TargetProcessId = processId;
    parameters.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(parameters);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&parameters);

    auto handler = Microsoft::WRL::Make<ProcessLoopbackActivator>();
    if (!handler || !handler->CompletedEvent()) return {};

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT status = ActivateAudioInterfaceAsync(
        VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
        &activation, handler.Get(), operation.GetAddressOf());
    if (FAILED(status) ||
        WaitForSingleObject(handler->CompletedEvent(), 5000) != WAIT_OBJECT_0 ||
        FAILED(handler->Result())) {
        return {};
    }
    return handler->Client();
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return true;
    if (format->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
        format->cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return false;
    }
    const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
    return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
}

float DecodeAudioSample(const BYTE* sample,
                        const WAVEFORMATEX* format,
                        bool floatingPoint) {
    if (floatingPoint && format->wBitsPerSample == 32) {
        return *reinterpret_cast<const float*>(sample);
    }
    if (format->wBitsPerSample == 16) {
        return static_cast<float>(*reinterpret_cast<const std::int16_t*>(sample)) /
               32768.0f;
    }
    if (format->wBitsPerSample == 24) {
        std::int32_t value = sample[0] | (sample[1] << 8) | (sample[2] << 16);
        if (value & 0x00800000) value |= 0xFF000000;
        return static_cast<float>(value) / 8388608.0f;
    }
    if (format->wBitsPerSample == 32) {
        return static_cast<float>(*reinterpret_cast<const std::int32_t*>(sample)) /
               2147483648.0f;
    }
    return 0.0f;
}

void FastFourierTransform(std::vector<std::complex<float>>& values) {
    const std::size_t count = values.size();
    for (std::size_t i = 1, j = 0; i < count; ++i) {
        std::size_t bit = count >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(values[i], values[j]);
    }

    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t length = 2; length <= count; length <<= 1) {
        const float angle = -2.0f * pi / static_cast<float>(length);
        const std::complex<float> step(std::cos(angle), std::sin(angle));
        for (std::size_t offset = 0; offset < count; offset += length) {
            std::complex<float> factor(1.0f, 0.0f);
            for (std::size_t index = 0; index < length / 2; ++index) {
                const auto even = values[offset + index];
                const auto odd = values[offset + index + length / 2] * factor;
                values[offset + index] = even + odd;
                values[offset + index + length / 2] = even - odd;
                factor *= step;
            }
        }
    }
}

std::array<float, visualizer::kBandCount> AnalyzeAudioWindow(
    const std::vector<float>& samples,
    std::uint32_t sampleRate) {
    const std::size_t count = samples.size();
    std::vector<std::complex<float>> spectrum(count);
    constexpr float pi = 3.14159265358979323846f;
    for (std::size_t index = 0; index < count; ++index) {
        // Match FluentFlyout/NAudio's HammingWindow rather than applying a
        // second visual-only approximation in Explorer.
        const float window = 0.54f - 0.46f *
            std::cos(2.0f * pi * static_cast<float>(index) /
                     static_cast<float>(count - 1));
        spectrum[index] = {samples[index] * window, 0.0f};
    }
    FastFourierTransform(spectrum);

    std::array<float, visualizer::kBandCount> result{};
    constexpr float lowFrequency = 40.0f;
    const float highFrequency = std::min(8000.0f, sampleRate * 0.48f);
    const float ratio = highFrequency / lowFrequency;
    for (std::size_t band = 0; band < result.size(); ++band) {
        const float startHz = lowFrequency *
            std::pow(ratio, static_cast<float>(band) / result.size());
        const float endHz = lowFrequency *
            std::pow(ratio, static_cast<float>(band + 1) / result.size());
        std::size_t first = std::max<std::size_t>(1,
            static_cast<std::size_t>(startHz * count / sampleRate));
        std::size_t last = std::min<std::size_t>(count / 2,
            std::max(first + 1,
                static_cast<std::size_t>(std::ceil(endHz * count / sampleRate))));

        float peak = 0.0f;
        constexpr float visualizerResponseScale = 0.60f;
        constexpr float fftDisplayScale =
            (1.0f / 8.0f) * visualizerResponseScale;
        for (std::size_t bin = first; bin < last; ++bin) {
            // Preserve the measured -18 dB calibration, then reduce response
            // by the requested 40%. This changes sensitivity rather than the
            // physical bar height, so strong peaks can still reach full scale.
            peak = std::max(
                peak, std::abs(spectrum[bin]) * fftDisplayScale);
        }
        const float progress = static_cast<float>(band) /
                               static_cast<float>(result.size());
        // FluentFlyout deliberately gives progressively stronger gain to
        // higher bands so the taskbar display does not collapse into two bass
        // bars. Publish dB encoded as 0..1; Explorer applies the user's
        // sensitivity once and owns the attack/release animation.
        peak *= 1.0f + progress * 75.0f;
        peak = std::max(peak, 0.001f);
        const float decibels = 20.0f * std::log10(peak);
        result[band] =
            (std::clamp(decibels, -100.0f, 0.0f) + 100.0f) / 100.0f;
    }
    return result;
}

bool CaptureAudioStream(SharedVisualizerWriter& writer, DWORD processId) {
    ComPtr<IAudioClient> client;
    WAVEFORMATEX processFormat{};
    WAVEFORMATEX* format = nullptr;
    std::unique_ptr<WAVEFORMATEX, decltype(&CoTaskMemFree)>
        mixFormat(nullptr, CoTaskMemFree);
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_LOOPBACK |
                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK;

    if (processId != 0) {
        client = ActivateProcessLoopback(processId);
        if (!client) return false;

        processFormat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        processFormat.nChannels = 2;
        processFormat.nSamplesPerSec = 48000;
        processFormat.wBitsPerSample = 32;
        processFormat.nBlockAlign =
            processFormat.nChannels * processFormat.wBitsPerSample / 8;
        processFormat.nAvgBytesPerSec =
            processFormat.nSamplesPerSec * processFormat.nBlockAlign;
        format = &processFormat;
        streamFlags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                       AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    } else {
        IMMDeviceEnumerator* enumeratorRaw = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                    CLSCTX_ALL,
                                    IID_PPV_ARGS(&enumeratorRaw)))) {
            return false;
        }
        unique_com<IMMDeviceEnumerator> enumerator(enumeratorRaw);

        IMMDevice* deviceRaw = nullptr;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia,
                                                       &deviceRaw))) {
            return false;
        }
        unique_com<IMMDevice> device(deviceRaw);

        IAudioClient* clientRaw = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
                                    nullptr,
                                    reinterpret_cast<void**>(&clientRaw)))) {
            return false;
        }
        client.Attach(clientRaw);

        WAVEFORMATEX* mixFormatRaw = nullptr;
        if (FAILED(client->GetMixFormat(&mixFormatRaw)) || !mixFormatRaw) {
            return false;
        }
        mixFormat.reset(mixFormatRaw);
        format = mixFormat.get();
    }

    ScopedHandle sampleReady{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    if (!sampleReady) return false;

    if (FAILED(client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags, 0, 0, format, nullptr))) {
        return false;
    }
    if (FAILED(client->SetEventHandle(sampleReady.value))) return false;

    IAudioCaptureClient* captureRaw = nullptr;
    if (FAILED(client->GetService(IID_PPV_ARGS(&captureRaw)))) return false;
    unique_com<IAudioCaptureClient> capture(captureRaw);
    if (FAILED(client->Start())) return false;
    g_visualizerCaptureReady.store(true, std::memory_order_release);
    g_visualizerSampleRate.store(format->nSamplesPerSec,
                                 std::memory_order_relaxed);

    constexpr std::size_t fftSize = 4096;
    const std::size_t hopSize =
        std::max<std::size_t>(1, format->nSamplesPerSec / 30);
    std::vector<float> mono;
    mono.reserve(fftSize * 2);
    std::size_t pendingSamples = 0;
    float inputPeak = 0.0f;
    std::array<float, visualizer::kBandCount> levels{};
    const bool floatingPoint = IsFloatFormat(format);
    const std::size_t bytesPerSample = format->wBitsPerSample / 8;
    if (bytesPerSample == 0 || format->nChannels == 0) {
        client->Stop();
        return false;
    }

    HRESULT status = S_OK;
    bool detectedAudio = false;
    int silentWindows = 0;
    while (SUCCEEDED(status) &&
           g_visualizerMediaPlaying.load(std::memory_order_acquire) &&
           (processId == 0 ||
            g_visualizerTargetProcessId.load(std::memory_order_acquire) ==
                processId)) {
        const DWORD waitResult = WaitForSingleObject(sampleReady.value, 100);
        if (waitResult == WAIT_TIMEOUT) continue;
        if (waitResult != WAIT_OBJECT_0) break;

        UINT32 packetFrames = 0;
        status = capture->GetNextPacketSize(&packetFrames);
        if (FAILED(status)) break;
        if (packetFrames == 0) {
            continue;
        }

        BYTE* data = nullptr;
        DWORD flags = 0;
        UINT32 frames = 0;
        status = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
        if (FAILED(status)) break;

        for (UINT32 frame = 0; frame < frames; ++frame) {
            float mixed = 0.0f;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && data) {
                const BYTE* frameData = data +
                    static_cast<std::size_t>(frame) * format->nBlockAlign;
                float strongest = 0.0f;
                for (WORD channel = 0; channel < format->nChannels; ++channel) {
                    const float sample = DecodeAudioSample(
                        frameData + channel * bytesPerSample,
                        format, floatingPoint);
                    if (std::abs(sample) > strongest) {
                        strongest = std::abs(sample);
                        mixed = sample;
                    }
                }
            }
            const float clamped = std::clamp(mixed, -1.0f, 1.0f);
            inputPeak = std::max(inputPeak, std::abs(clamped));
            mono.push_back(clamped);
        }
        pendingSamples += frames;
        capture->ReleaseBuffer(frames);

        if (mono.size() > fftSize * 2) {
            mono.erase(mono.begin(), mono.end() - fftSize);
        }
        if (mono.size() >= fftSize && pendingSamples >= hopSize) {
            std::vector<float> window(mono.end() - fftSize, mono.end());
            const bool audible = inputPeak > 0.00032f;
            detectedAudio = detectedAudio || audible;
            silentWindows = audible ? 0 : silentWindows + 1;
            if (audible) {
                levels = AnalyzeAudioWindow(window, format->nSamplesPerSec);
            } else {
                levels.fill(0.0f);
            }
            g_visualizerHasAudio.store(audible, std::memory_order_relaxed);
            g_visualizerPeak.store(inputPeak, std::memory_order_relaxed);
            g_visualizerFrameCount.fetch_add(1, std::memory_order_relaxed);
            writer.Publish(levels, format->nSamplesPerSec, audible);
            pendingSamples = 0;
            inputPeak = 0.0f;
            if (processId != 0 && !detectedAudio && silentWindows >= 30) {
                break;
            }
        }
    }

    client->Stop();
    g_visualizerCaptureReady.store(false, std::memory_order_release);
    g_visualizerHasAudio.store(false, std::memory_order_relaxed);
    g_visualizerPeak.store(0.0f, std::memory_order_relaxed);
    return detectedAudio;
}

void RunAudioVisualizer() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    SharedVisualizerWriter writer;
    if (!writer) return;

    std::array<float, visualizer::kBandCount> empty{};
    while (true) {
        const bool mediaPlaying =
            g_visualizerMediaPlaying.load(std::memory_order_acquire);
        g_visualizerCaptureReady.store(false, std::memory_order_release);
        g_visualizerHasAudio.store(false, std::memory_order_relaxed);
        g_visualizerPeak.store(0.0f, std::memory_order_relaxed);
        writer.Publish(empty, 0, false);
        if (mediaPlaying) {
            // FluentFlyout uses WASAPI endpoint loopback. Capture the render
            // endpoint directly as well, while the selected GSMTC session is
            // playing, so visualization starts immediately without waiting
            // for a process-loopback probe to time out.
            CaptureAudioStream(writer, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
}

struct Options {
    std::wstring appDir;
    bool watch{};
    bool toggle{};
    bool previous{};
    bool next{};
};

struct MediaStatus {
    bool loaded{true};
    bool active{};
    bool playing{};
    bool stale{};
    bool canToggle{};
    bool canPrevious{};
    bool canNext{};
    long long updatedAtUnix{};
    std::wstring title;
    std::wstring artist;
    std::wstring coverPath;
    std::wstring backgroundLeftColor;
    std::wstring backgroundRightColor;
    std::wstring accentColor;
    std::wstring textColor;
    std::wstring sourceApp;
    std::wstring metadataSource;
    std::wstring error;
    int sessionCount{};
    bool visualizerCaptureReady{};
    bool visualizerHasAudio{};
    unsigned int visualizerSampleRate{};
    unsigned long long visualizerFrameCount{};
    float visualizerPeak{};
    float visualizerSessionPeak{};
    DWORD visualizerTargetProcessId{};
    std::wstring visualizerTargetMode;
};

struct MediaSessionCandidate {
    wmc::GlobalSystemMediaTransportControlsSession session{nullptr};
    wmc::GlobalSystemMediaTransportControlsSessionMediaProperties props{nullptr};
    bool playing{};
    bool current{};
    bool hasMetadata{};
    std::wstring title;
    std::wstring artist;
    std::wstring sourceApp;
};

struct GdiplusSession {
    ULONG_PTR token{};

    GdiplusSession() {
        gdi::GdiplusStartupInput input{};
        if (gdi::GdiplusStartup(&token, &input, nullptr) != gdi::Ok) {
            token = 0;
        }
    }

    ~GdiplusSession() {
        if (token) {
            gdi::GdiplusShutdown(token);
        }
    }

    explicit operator bool() const {
        return token != 0;
    }
};

std::wstring ExeDirectory() {
    WCHAR path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    std::wstring value(path);
    size_t slash = value.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : value.substr(0, slash);
}

Options ParseOptions(int argc, wchar_t** argv) {
    Options options;
    options.appDir = ExeDirectory();

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (arg == L"--watch") {
            options.watch = true;
        } else if (arg == L"--toggle") {
            options.toggle = true;
        } else if (arg == L"--previous") {
            options.previous = true;
        } else if (arg == L"--next") {
            options.next = true;
        } else if (arg == L"--app-dir" && i + 1 < argc) {
            options.appDir = argv[++i];
        }
    }

    return options;
}

std::wstring JoinPath(const std::wstring& base, const std::wstring& leaf) {
    if (base.empty()) {
        return leaf;
    }

    wchar_t last = base.back();
    if (last == L'\\' || last == L'/') {
        return base + leaf;
    }

    return base + L"\\" + leaf;
}

std::wstring AssetsWidgetDirectory(const std::wstring& appDir) {
    std::wstring assets = JoinPath(appDir, L"Assets");
    CreateDirectoryW(assets.c_str(), nullptr);
    std::wstring widgets = JoinPath(assets, L"widgets");
    CreateDirectoryW(widgets.c_str(), nullptr);
    return widgets;
}

bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

unsigned long long HashMediaKey(const std::wstring& key) {
    unsigned long long hash = 14695981039346656037ULL;
    for (wchar_t ch : key) {
        hash ^= static_cast<unsigned long long>(ch);
        hash *= 1099511628211ULL;
    }

    return hash;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    int length = WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                                     static_cast<int>(value.size()), nullptr, 0,
                                     nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(),
                        static_cast<int>(value.size()), result.data(), length,
                        nullptr, nullptr);
    return result;
}

std::string JsonEscape(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string escaped;
    escaped.reserve(utf8.size());
    for (char ch : utf8) {
        switch (ch) {
            case '\\':
                escaped += "\\\\";
                break;
            case '"':
                escaped += "\\\"";
                break;
            case '\r':
                escaped += "\\r";
                break;
            case '\n':
                escaped += "\\n";
                break;
            case '\t':
                escaped += "\\t";
                break;
            default:
                escaped += ch;
                break;
        }
    }
    return escaped;
}

long long CurrentUnixTime() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);
    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    return static_cast<long long>((value.QuadPart - 116444736000000000ULL) /
                                  10000000ULL);
}

bool WriteRawFile(const std::wstring& path, const void* data, size_t size) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    bool ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) &&
              written == size;
    CloseHandle(file);
    return ok;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    return WriteRawFile(path, bytes.data(), bytes.size());
}

std::wstring TrimWhitespace(const std::wstring& value) {
    size_t first = 0;
    while (first < value.size() && std::iswspace(value[first])) {
        ++first;
    }

    size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1])) {
        --last;
    }

    return value.substr(first, last - first);
}

std::wstring ToLower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool ContainsInsensitive(const std::wstring& haystack,
                         const std::wstring& needle) {
    if (needle.empty()) {
        return true;
    }

    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

std::wstring FileNameWithoutExtension(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    std::wstring name =
        slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        name = name.substr(0, dot);
    }
    return name;
}

std::wstring ProcessBaseNameFromPid(DWORD pid) {
    if (!pid) {
        return {};
    }

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return {};
    }

    WCHAR path[MAX_PATH]{};
    DWORD size = ARRAYSIZE(path);
    std::wstring baseName;
    if (QueryFullProcessImageNameW(process, 0, path, &size) && size > 0) {
        baseName = FileNameWithoutExtension(std::wstring(path, size));
    }
    CloseHandle(process);
    return baseName;
}

std::wstring ProcessBaseNameFromWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return ProcessBaseNameFromPid(pid);
}

std::wstring WindowTitle(HWND hwnd) {
    int length = GetWindowTextLengthW(hwnd);
    if (length <= 0 || length > 512) {
        return {};
    }

    std::wstring title(static_cast<size_t>(length + 1), L'\0');
    int copied = GetWindowTextW(hwnd, title.data(), length + 1);
    if (copied <= 0) {
        return {};
    }

    title.resize(static_cast<size_t>(copied));
    return TrimWhitespace(title);
}

bool IsNoisyWindowTitle(const std::wstring& title) {
    std::wstring lowered = ToLower(title);
    return lowered.empty() ||
           lowered == L"program manager" ||
           lowered == L"settings" ||
           lowered.find(L"taskbarstats") != std::wstring::npos ||
           lowered.find(L"windows input experience") != std::wstring::npos;
}

bool SourceMatchesProcess(const std::wstring& sourceLower,
                          const std::wstring& processLower) {
    if (sourceLower.empty() || processLower.empty()) {
        return false;
    }

    if (sourceLower.find(processLower) != std::wstring::npos ||
        processLower.find(sourceLower) != std::wstring::npos) {
        return true;
    }

    struct Alias {
        PCWSTR source;
        PCWSTR process;
    };

    constexpr Alias aliases[] = {
        {L"spotify", L"spotify"},
        {L"chrome", L"chrome"},
        {L"googlechrome", L"chrome"},
        {L"msedge", L"msedge"},
        {L"edge", L"msedge"},
        {L"opera", L"opera"},
        {L"firefox", L"firefox"},
        {L"brave", L"brave"},
        {L"vivaldi", L"vivaldi"},
        {L"yandex", L"browser"},
    };

    for (const auto& alias : aliases) {
        if (sourceLower.find(alias.source) != std::wstring::npos &&
            processLower.find(alias.process) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

DWORD FindMediaAudioSessionProcessId(const std::wstring& sourceApp,
                                     float* selectedPeak = nullptr) {
    const std::wstring sourceLower = ToLower(sourceApp);
    if (sourceLower.empty()) return 0;

    IMMDeviceEnumerator* enumeratorRaw = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                CLSCTX_ALL, IID_PPV_ARGS(&enumeratorRaw)))) {
        return 0;
    }
    unique_com<IMMDeviceEnumerator> enumerator(enumeratorRaw);

    IMMDeviceCollection* devicesRaw = nullptr;
    if (FAILED(enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, &devicesRaw)) || !devicesRaw) {
        return 0;
    }
    unique_com<IMMDeviceCollection> devices(devicesRaw);

    DWORD bestProcessId = 0;
    float bestPeak = -1.0f;
    int bestStateScore = -1;
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex) {
        IMMDevice* deviceRaw = nullptr;
        if (FAILED(devices->Item(deviceIndex, &deviceRaw)) || !deviceRaw) {
            continue;
        }
        unique_com<IMMDevice> device(deviceRaw);

        IAudioSessionManager2* managerRaw = nullptr;
        if (FAILED(device->Activate(
                __uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                reinterpret_cast<void**>(&managerRaw))) || !managerRaw) {
            continue;
        }
        unique_com<IAudioSessionManager2> manager(managerRaw);

        IAudioSessionEnumerator* sessionsRaw = nullptr;
        if (FAILED(manager->GetSessionEnumerator(&sessionsRaw)) ||
            !sessionsRaw) {
            continue;
        }
        unique_com<IAudioSessionEnumerator> sessions(sessionsRaw);

        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int index = 0; index < sessionCount; ++index) {
            IAudioSessionControl* controlRaw = nullptr;
            if (FAILED(sessions->GetSession(index, &controlRaw)) ||
                !controlRaw) {
                continue;
            }
            unique_com<IAudioSessionControl> control(controlRaw);

            IAudioSessionControl2* control2Raw = nullptr;
            if (FAILED(control->QueryInterface(IID_PPV_ARGS(&control2Raw))) ||
                !control2Raw) {
                continue;
            }
            unique_com<IAudioSessionControl2> control2(control2Raw);

            DWORD processId = 0;
            if (FAILED(control2->GetProcessId(&processId)) || processId == 0) {
                continue;
            }
            const std::wstring processLower =
                ToLower(ProcessBaseNameFromPid(processId));
            if (!SourceMatchesProcess(sourceLower, processLower)) {
                continue;
            }

            AudioSessionState state = AudioSessionStateInactive;
            control->GetState(&state);
            const int stateScore = state == AudioSessionStateActive ? 2 :
                                   state == AudioSessionStateInactive ? 1 : 0;

            float peak = 0.0f;
            IAudioMeterInformation* meterRaw = nullptr;
            if (SUCCEEDED(control->QueryInterface(IID_PPV_ARGS(&meterRaw))) &&
                meterRaw) {
                unique_com<IAudioMeterInformation> meter(meterRaw);
                meter->GetPeakValue(&peak);
            }

            if (stateScore > bestStateScore ||
                (stateScore == bestStateScore && peak > bestPeak)) {
                bestStateScore = stateScore;
                bestPeak = peak;
                bestProcessId = processId;
            }
        }
    }
    if (selectedPeak) {
        *selectedPeak = std::max(bestPeak, 0.0f);
    }
    return bestProcessId;
}

DWORD FindSourceProcessTreeRoot(const std::wstring& sourceApp) {
    const std::wstring sourceLower = ToLower(sourceApp);
    if (sourceLower.empty()) return 0;

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    struct ProcessEntry {
        DWORD processId{};
        DWORD parentProcessId{};
    };
    std::vector<ProcessEntry> matches;
    std::unordered_map<DWORD, DWORD> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const std::wstring processLower =
                ToLower(FileNameWithoutExtension(entry.szExeFile));
            parents[entry.th32ProcessID] = entry.th32ParentProcessID;
            if (SourceMatchesProcess(sourceLower, processLower)) {
                matches.push_back(
                    {entry.th32ProcessID, entry.th32ParentProcessID});
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    if (matches.empty()) return 0;

    std::unordered_set<DWORD> matchingIds;
    for (const auto& process : matches) matchingIds.insert(process.processId);

    DWORD bestRoot = 0;
    int bestDescendantCount = -1;
    for (const auto& candidate : matches) {
        if (matchingIds.contains(candidate.parentProcessId)) continue;

        int descendantCount = 0;
        for (const auto& process : matches) {
            DWORD current = process.processId;
            for (int depth = 0; depth < 32 && current != 0; ++depth) {
                if (current == candidate.processId) {
                    ++descendantCount;
                    break;
                }
                auto parent = parents.find(current);
                if (parent == parents.end() || parent->second == current) break;
                current = parent->second;
            }
        }
        if (descendantCount > bestDescendantCount) {
            bestDescendantCount = descendantCount;
            bestRoot = candidate.processId;
        }
    }
    return bestRoot != 0 ? bestRoot : matches.front().processId;
}

std::wstring StripKnownWindowSuffix(std::wstring title) {
    std::wstring lowered = ToLower(title);
    const std::vector<std::wstring> suffixes = {
        L" - youtube - google chrome",
        L" - youtube - microsoft edge",
        L" - youtube - opera",
        L" - youtube - brave",
        L" - youtube - firefox",
        L" - google chrome",
        L" - microsoft edge",
        L" - opera",
        L" - brave",
        L" - firefox",
        L" - spotify",
    };

    for (const auto& suffix : suffixes) {
        if (lowered.size() > suffix.size() &&
            lowered.rfind(suffix) == lowered.size() - suffix.size()) {
            title.resize(title.size() - suffix.size());
            return TrimWhitespace(title);
        }
    }

    return TrimWhitespace(title);
}

struct WindowTitleSearch {
    std::wstring sourceLower;
    std::wstring bestTitle;
    int bestScore{-1};
    bool allowGenericMediaWindows{};
};

bool IsKnownMediaProcess(const std::wstring& processLower) {
    constexpr PCWSTR processes[] = {
        L"spotify",
        L"chrome",
        L"msedge",
        L"opera",
        L"firefox",
        L"brave",
        L"vivaldi",
        L"browser",
    };

    for (PCWSTR process : processes) {
        if (processLower.find(process) != std::wstring::npos) {
            return true;
        }
    }

    return false;
}

bool LooksLikeMediaWindow(const std::wstring& titleLower,
                          const std::wstring& processLower) {
    if (processLower.find(L"spotify") != std::wstring::npos) {
        return true;
    }

    return IsKnownMediaProcess(processLower) &&
           (titleLower.find(L"youtube") != std::wstring::npos ||
            titleLower.find(L"music") != std::wstring::npos ||
            titleLower.find(L"spotify") != std::wstring::npos);
}

BOOL CALLBACK EnumWindowTitleFallback(HWND hwnd, LPARAM lparam) {
    auto* search = reinterpret_cast<WindowTitleSearch*>(lparam);
    if (!IsWindowVisible(hwnd) || IsIconic(hwnd)) {
        return TRUE;
    }

    std::wstring title = WindowTitle(hwnd);
    if (IsNoisyWindowTitle(title)) {
        return TRUE;
    }

    std::wstring processLower = ToLower(ProcessBaseNameFromWindow(hwnd));
    std::wstring titleLower = ToLower(title);
    bool sourceMatched = SourceMatchesProcess(search->sourceLower, processLower);
    bool genericMatched = search->allowGenericMediaWindows &&
                          LooksLikeMediaWindow(titleLower, processLower);
    if (!sourceMatched && !genericMatched) {
        return TRUE;
    }

    int score = sourceMatched ? 100 : 70;
    if (titleLower.find(L"youtube") != std::wstring::npos) {
        score += 20;
    }
    if (processLower.find(L"spotify") != std::wstring::npos) {
        score += 20;
    }
    if (GetForegroundWindow() == hwnd) {
        score += 5;
    }

    if (score > search->bestScore) {
        search->bestScore = score;
        search->bestTitle = StripKnownWindowSuffix(title);
    }

    return TRUE;
}

std::wstring FindWindowTitleFallback(const std::wstring& sourceApp) {
    WindowTitleSearch search;
    search.sourceLower = ToLower(sourceApp);
    search.allowGenericMediaWindows = search.sourceLower.empty();

    EnumWindows(EnumWindowTitleFallback, reinterpret_cast<LPARAM>(&search));
    return TrimWhitespace(search.bestTitle);
}

struct RgbSample {
    double r{};
    double g{};
    double b{};
    int count{};
};

BYTE ClampByte(double value) {
    return static_cast<BYTE>(std::clamp(std::round(value), 0.0, 255.0));
}

std::wstring HexColor(BYTE r, BYTE g, BYTE b) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring value = L"#000000";
    BYTE channels[] = {r, g, b};
    for (int i = 0; i < 3; ++i) {
        value[1 + i * 2] = digits[(channels[i] >> 4) & 0xF];
        value[2 + i * 2] = digits[channels[i] & 0xF];
    }
    return value;
}

std::wstring BackgroundHexFromSample(const RgbSample& sample) {
    if (sample.count <= 0) {
        return L"#0F172A";
    }

    double r = sample.r / sample.count;
    double g = sample.g / sample.count;
    double b = sample.b / sample.count;
    double brightness = (r * 0.299 + g * 0.587 + b * 0.114);
    double darken = brightness > 170.0 ? 0.26 : 0.36;
    double floor = 10.0;
    return HexColor(ClampByte(r * darken + floor),
                    ClampByte(g * darken + floor),
                    ClampByte(b * darken + floor));
}

std::wstring AccentHexFromSample(const RgbSample& sample) {
    if (sample.count <= 0) {
        return L"#22D3EE";
    }

    double r = sample.r / sample.count;
    double g = sample.g / sample.count;
    double b = sample.b / sample.count;
    double maxChannel = std::max({r, g, b});
    double boost = maxChannel < 1.0 ? 1.0 : std::min(1.75, 210.0 / maxChannel);
    return HexColor(ClampByte(r * boost),
                    ClampByte(g * boost),
                    ClampByte(b * boost));
}

RgbSample SampleBitmapArea(gdi::Bitmap& bitmap,
                           UINT x0,
                           UINT y0,
                           UINT x1,
                           UINT y1) {
    RgbSample sample;
    UINT width = bitmap.GetWidth();
    UINT height = bitmap.GetHeight();
    if (width == 0 || height == 0) {
        return sample;
    }

    x1 = std::min(x1, width);
    y1 = std::min(y1, height);
    UINT stepX = std::max<UINT>(1, (x1 - x0) / 24);
    UINT stepY = std::max<UINT>(1, (y1 - y0) / 48);
    for (UINT y = y0; y < y1; y += stepY) {
        for (UINT x = x0; x < x1; x += stepX) {
            gdi::Color color;
            if (bitmap.GetPixel(x, y, &color) != gdi::Ok || color.GetA() < 150) {
                continue;
            }

            int r = color.GetR();
            int g = color.GetG();
            int b = color.GetB();
            int maxChannel = std::max({r, g, b});
            int minChannel = std::min({r, g, b});
            int brightness = (r * 30 + g * 59 + b * 11) / 100;
            if (brightness < 12 || brightness > 245 || (maxChannel - minChannel < 8)) {
                continue;
            }

            sample.r += r;
            sample.g += g;
            sample.b += b;
            ++sample.count;
        }
    }

    return sample;
}

void MergeSample(RgbSample& target, const RgbSample& source) {
    target.r += source.r;
    target.g += source.g;
    target.b += source.b;
    target.count += source.count;
}

bool AnalyzeThumbnailColors(const std::wstring& path, MediaStatus& status) {
    if (path.empty() || !FileExists(path)) {
        return false;
    }

    gdi::Bitmap bitmap(path.c_str());
    if (bitmap.GetLastStatus() != gdi::Ok ||
        bitmap.GetWidth() < 4 || bitmap.GetHeight() < 4) {
        return false;
    }

    UINT width = bitmap.GetWidth();
    UINT height = bitmap.GetHeight();
    UINT edgeX = std::max<UINT>(2, width / 5);
    UINT edgeY = std::max<UINT>(2, height / 6);

    RgbSample left = SampleBitmapArea(bitmap, 0, 0, edgeX, height);
    RgbSample right = SampleBitmapArea(bitmap, width - edgeX, 0, width, height);
    RgbSample top = SampleBitmapArea(bitmap, 0, 0, width, edgeY);
    RgbSample bottom = SampleBitmapArea(bitmap, 0, height - edgeY, width, height);

    RgbSample accent = left;
    MergeSample(accent, right);
    MergeSample(accent, top);
    MergeSample(accent, bottom);

    if (left.count <= 0) {
        left = top.count > 0 ? top : accent;
    }
    if (right.count <= 0) {
        right = bottom.count > 0 ? bottom : accent;
    }

    status.backgroundLeftColor = BackgroundHexFromSample(left);
    status.backgroundRightColor = BackgroundHexFromSample(right);
    status.accentColor = AccentHexFromSample(accent);
    status.textColor = L"#F8FAFC";
    return accent.count > 0;
}

std::string ReadTextFile(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return {};
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > 1024 * 1024) {
        CloseHandle(file);
        return {};
    }

    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok) {
        return {};
    }

    data.resize(read);
    return data;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return {};
    }

    int length = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                     static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), length);
    return result;
}

bool ExtractJsonString(const std::string& json, const char* key, std::wstring& value) {
    std::string decoded;
    if (!taskbar_widgets::json::ExtractStringUtf8(json, key, decoded)) {
        return false;
    }
    value = Utf8ToWide(decoded);
    return true;
}

bool ExtractJsonBool(const std::string& json, const char* key, bool& value) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t position = json.find(pattern);
    if (position == std::string::npos) {
        return false;
    }

    position += pattern.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }

    if (json.compare(position, 4, "true") == 0) {
        value = true;
        return true;
    }
    if (json.compare(position, 5, "false") == 0) {
        value = false;
        return true;
    }

    return false;
}

bool ExtractJsonInt64(const std::string& json, const char* key, long long& value) {
    std::string pattern = std::string("\"") + key + "\":";
    size_t position = json.find(pattern);
    if (position == std::string::npos) {
        return false;
    }

    position += pattern.size();
    while (position < json.size() &&
           (json[position] == ' ' || json[position] == '\t' ||
            json[position] == '\r' || json[position] == '\n')) {
        ++position;
    }

    char* end = nullptr;
    long long parsed = std::strtoll(json.c_str() + position, &end, 10);
    if (end == json.c_str() + position) {
        return false;
    }

    value = parsed;
    return true;
}

MediaStatus ReadPreviousStatus(const std::wstring& appDir) {
    MediaStatus status;
    std::string json = ReadTextFile(
        JoinPath(JoinPath(appDir, L"State"), L"media-player.json"));
    if (json.empty()) {
        return status;
    }

    ExtractJsonBool(json, "active", status.active);
    ExtractJsonBool(json, "playing", status.playing);
    ExtractJsonBool(json, "canToggle", status.canToggle);
    ExtractJsonBool(json, "canPrevious", status.canPrevious);
    ExtractJsonBool(json, "canNext", status.canNext);
    ExtractJsonString(json, "title", status.title);
    ExtractJsonString(json, "artist", status.artist);
    ExtractJsonString(json, "coverPath", status.coverPath);
    ExtractJsonString(json, "backgroundLeftColor", status.backgroundLeftColor);
    ExtractJsonString(json, "backgroundRightColor", status.backgroundRightColor);
    ExtractJsonString(json, "accentColor", status.accentColor);
    ExtractJsonString(json, "textColor", status.textColor);
    ExtractJsonString(json, "sourceApp", status.sourceApp);
    ExtractJsonString(json, "metadataSource", status.metadataSource);
    ExtractJsonInt64(json, "updatedAtUnix", status.updatedAtUnix);
    return status;
}

void PreservePreviousMedia(MediaStatus& status, const MediaStatus& previous) {
    long long age = CurrentUnixTime() - previous.updatedAtUnix;
    bool previousIsFresh = previous.updatedAtUnix > 0 && age >= 0 && age <= 600;
    bool previousHasMedia = !previous.title.empty() || !previous.artist.empty();
    if (!previousIsFresh || !previousHasMedia) {
        return;
    }

    status.active = previous.active || !status.error.empty();
    status.playing = previous.playing;
    status.canToggle = previous.canToggle;
    status.canPrevious = previous.canPrevious;
    status.canNext = previous.canNext;
    status.stale = true;
    status.title = previous.title;
    status.artist = previous.artist;
    if (!previous.coverPath.empty() && FileExists(previous.coverPath)) {
        status.coverPath = previous.coverPath;
    }
    status.backgroundLeftColor = previous.backgroundLeftColor;
    status.backgroundRightColor = previous.backgroundRightColor;
    status.accentColor = previous.accentColor;
    status.textColor = previous.textColor;
    status.sourceApp = previous.sourceApp;
    status.metadataSource = previous.metadataSource;
}

bool SaveThumbnail(wmc::GlobalSystemMediaTransportControlsSessionMediaProperties const& props,
                   const std::wstring& appDir,
                   const std::wstring& mediaKey,
                   std::wstring& coverPath) {
    try {
        std::wstring key = mediaKey.empty() ? L"active" : mediaKey;
        coverPath = JoinPath(
            AssetsWidgetDirectory(appDir),
            L"media_live_cover_" + std::to_wstring(HashMediaKey(key)) + L".png");
        if (FileExists(coverPath)) {
            return true;
        }

        auto reference = props.Thumbnail();
        if (!reference) {
            return false;
        }

        auto stream = reference.OpenReadAsync().get();
        uint64_t size64 = stream.Size();
        if (size64 == 0 || size64 > 10ULL * 1024ULL * 1024ULL) {
            return false;
        }

        uint32_t size = static_cast<uint32_t>(size64);
        wss::Buffer buffer(size);
        auto loaded = stream.ReadAsync(buffer, size, wss::InputStreamOptions::None).get();
        uint32_t length = loaded.Length();
        if (length == 0) {
            return false;
        }

        std::vector<uint8_t> bytes(length);
        wss::DataReader reader = wss::DataReader::FromBuffer(loaded);
        reader.ReadBytes(winrt::array_view<uint8_t>(
            bytes.data(), bytes.data() + bytes.size()));

        return WriteBytes(coverPath, bytes);
    } catch (...) {
        return false;
    }
}

MediaSessionCandidate ReadCandidate(
    wmc::GlobalSystemMediaTransportControlsSession const& session,
    bool current) {
    MediaSessionCandidate candidate;
    candidate.session = session;
    candidate.current = current;
    if (!session) {
        return candidate;
    }

    try {
        candidate.sourceApp = session.SourceAppUserModelId().c_str();
    } catch (...) {
        candidate.sourceApp.clear();
    }

    try {
        auto playback = session.GetPlaybackInfo();
        candidate.playing =
            playback.PlaybackStatus() ==
            wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
    } catch (...) {
        candidate.playing = false;
    }

    try {
        candidate.props = session.TryGetMediaPropertiesAsync().get();
        candidate.title = candidate.props.Title().c_str();
        candidate.artist = candidate.props.Artist().c_str();
        candidate.hasMetadata =
            !candidate.title.empty() || !candidate.artist.empty();
    } catch (...) {
        candidate.props = nullptr;
        candidate.hasMetadata = false;
    }

    return candidate;
}

int CandidateScore(const MediaSessionCandidate& candidate) {
    if (!candidate.session) {
        return -1;
    }

    int score = 0;
    if (candidate.hasMetadata) {
        score += 100;
    }
    if (candidate.playing) {
        score += 40;
    }
    if (candidate.current) {
        score += 10;
    }
    if (!candidate.sourceApp.empty()) {
        score += 2;
    }

    return score;
}

MediaSessionCandidate SelectBestMediaSession(
    wmc::GlobalSystemMediaTransportControlsSessionManager const& manager,
    MediaStatus& status) {
    std::vector<MediaSessionCandidate> candidates;

    auto current = manager.GetCurrentSession();
    if (current) {
        candidates.push_back(ReadCandidate(current, true));
    }

    try {
        auto sessions = manager.GetSessions();
        status.sessionCount = static_cast<int>(sessions.Size());
        for (auto const& session : sessions) {
            bool isCurrent = current && session == current;
            if (isCurrent) {
                continue;
            }
            candidates.push_back(ReadCandidate(session, false));
        }
    } catch (...) {
        status.sessionCount = current ? 1 : 0;
    }

    MediaSessionCandidate best;
    int bestScore = -1;
    for (const auto& candidate : candidates) {
        int score = CandidateScore(candidate);
        if (score > bestScore) {
            best = candidate;
            bestScore = score;
        }
    }

    return best;
}

MediaStatus QueryStatus(const std::wstring& appDir) {
    MediaStatus status;
    status.visualizerCaptureReady =
        g_visualizerCaptureReady.load(std::memory_order_acquire);
    status.visualizerHasAudio =
        g_visualizerHasAudio.load(std::memory_order_relaxed);
    status.visualizerSampleRate =
        g_visualizerSampleRate.load(std::memory_order_relaxed);
    status.visualizerFrameCount =
        g_visualizerFrameCount.load(std::memory_order_relaxed);
    status.visualizerPeak =
        g_visualizerPeak.load(std::memory_order_relaxed);
    status.visualizerSessionPeak =
        g_visualizerSessionPeak.load(std::memory_order_relaxed);
    status.coverPath = JoinPath(AssetsWidgetDirectory(appDir), L"media_cover.png");
    MediaStatus previous = ReadPreviousStatus(appDir);

    try {
        auto manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto selected = SelectBestMediaSession(manager, status);
        if (!selected.session) {
            g_visualizerMediaPlaying.store(false, std::memory_order_release);
            g_visualizerTargetProcessId.store(0, std::memory_order_release);
            g_visualizerSessionPeak.store(0.0f, std::memory_order_release);
            PreservePreviousMedia(status, previous);
            return status;
        }

        status.active = true;
        status.playing = selected.playing;
        status.sourceApp = selected.sourceApp;
        float audioSessionPeak = 0.0f;
        const DWORD audioSessionProcessId =
            FindMediaAudioSessionProcessId(status.sourceApp, &audioSessionPeak);
        status.visualizerSessionPeak = audioSessionPeak;
        g_visualizerSessionPeak.store(audioSessionPeak,
                                      std::memory_order_release);
        // The Core Audio session PID is the process that is actually rendering
        // the selected media. Prefer it over the application's launcher/root
        // process; Chromium-based players often render audio in a utility
        // process and process-tree capture from the browser root can be silent.
        status.visualizerTargetProcessId = audioSessionProcessId;
        if (status.visualizerTargetProcessId != 0) {
            status.visualizerTargetMode = L"audioSession";
        } else {
            status.visualizerTargetProcessId =
                FindSourceProcessTreeRoot(status.sourceApp);
            status.visualizerTargetMode =
                status.visualizerTargetProcessId != 0 ? L"processTree" : L"none";
        }
        g_visualizerTargetProcessId.store(status.visualizerTargetProcessId,
                                          std::memory_order_release);
        g_visualizerMediaPlaying.store(status.playing,
                                       std::memory_order_release);

        try {
            auto controls = selected.session.GetPlaybackInfo().Controls();
            status.canToggle = controls.IsPlayPauseToggleEnabled() ||
                               controls.IsPlayEnabled() ||
                               controls.IsPauseEnabled();
            status.canPrevious = controls.IsPreviousEnabled();
            status.canNext = controls.IsNextEnabled();
        } catch (...) {
            // Some players omit capability metadata. Keep toggle available
            // because the session itself still commonly accepts the command.
            status.canToggle = true;
        }

        if (!selected.title.empty()) {
            status.title = selected.title;
        }
        if (!selected.artist.empty()) {
            status.artist = selected.artist;
        }
        if (!status.title.empty() || !status.artist.empty()) {
            status.metadataSource = L"gsmtc";
        }

        if (status.title.empty() && status.artist.empty()) {
            std::wstring fallbackTitle = FindWindowTitleFallback(status.sourceApp);
            if (!fallbackTitle.empty()) {
                status.title = fallbackTitle;
                status.metadataSource = L"windowTitle";
            }
        }

        std::wstring liveCover;
        if (selected.props &&
            SaveThumbnail(selected.props, appDir, status.title + L"|" + status.artist,
                          liveCover)) {
            status.coverPath = liveCover;
            AnalyzeThumbnailColors(status.coverPath, status);
        }
    } catch (winrt::hresult_error const& ex) {
        g_visualizerMediaPlaying.store(false, std::memory_order_release);
        g_visualizerTargetProcessId.store(0, std::memory_order_release);
        g_visualizerSessionPeak.store(0.0f, std::memory_order_release);
        status.error = ex.message().c_str();
        PreservePreviousMedia(status, previous);
    } catch (...) {
        g_visualizerMediaPlaying.store(false, std::memory_order_release);
        g_visualizerTargetProcessId.store(0, std::memory_order_release);
        g_visualizerSessionPeak.store(0.0f, std::memory_order_release);
        status.error = L"Media query failed";
        PreservePreviousMedia(status, previous);
    }

    return status;
}

void WriteStatus(const std::wstring& appDir, const MediaStatus& status) {
    std::wstring stateDirectory = JoinPath(appDir, L"State");
    CreateDirectoryW(stateDirectory.c_str(), nullptr);
    std::wstring path = JoinPath(stateDirectory, L"media-player.json");
    std::wstring tempPath = path + L".tmp";

    long long now = CurrentUnixTime();
    std::string json = "{\n";
    json += "  \"schemaVersion\": 1,\n";
    json += "  \"widgetId\": \"media-player\",\n";
    json += "  \"sequence\": " + std::to_string(now) + ",\n";
    json += "  \"updatedAtUnix\": " + std::to_string(now) + ",\n";
    json += std::string("  \"status\": \"") + (status.error.empty() ? "ok" : "error") + "\",\n";
    if (!status.error.empty()) {
        json += "  \"error\": \"" + JsonEscape(status.error) + "\",\n";
    }
    json += "  \"data\": {\n";
    json += "    \"loaded\": true,\n";
    json += std::string("    \"active\": ") + (status.active ? "true" : "false") + ",\n";
    json += std::string("    \"playing\": ") + (status.playing ? "true" : "false") + ",\n";
    json += std::string("    \"canToggle\": ") + (status.canToggle ? "true" : "false") + ",\n";
    json += std::string("    \"canPrevious\": ") + (status.canPrevious ? "true" : "false") + ",\n";
    json += std::string("    \"canNext\": ") + (status.canNext ? "true" : "false") + ",\n";
    json += std::string("    \"stale\": ") + (status.stale ? "true" : "false") + ",\n";
    json += "    \"title\": \"" + JsonEscape(status.title) + "\",\n";
    json += "    \"artist\": \"" + JsonEscape(status.artist) + "\",\n";
    json += "    \"coverPath\": \"" + JsonEscape(status.coverPath) + "\",\n";
    json += "    \"backgroundLeftColor\": \"" + JsonEscape(status.backgroundLeftColor) + "\",\n";
    json += "    \"backgroundRightColor\": \"" + JsonEscape(status.backgroundRightColor) + "\",\n";
    json += "    \"accentColor\": \"" + JsonEscape(status.accentColor) + "\",\n";
    json += "    \"textColor\": \"" + JsonEscape(status.textColor) + "\",\n";
    json += "    \"sourceApp\": \"" + JsonEscape(status.sourceApp) + "\",\n";
    json += "    \"metadataSource\": \"" + JsonEscape(status.metadataSource) + "\",\n";
    json += "    \"sessionCount\": " + std::to_string(status.sessionCount) + ",\n";
    json += std::string("    \"visualizerCaptureReady\": ") +
            (status.visualizerCaptureReady ? "true" : "false") + ",\n";
    json += std::string("    \"visualizerHasAudio\": ") +
            (status.visualizerHasAudio ? "true" : "false") + ",\n";
    json += "    \"visualizerSampleRate\": " +
            std::to_string(status.visualizerSampleRate) + ",\n";
    json += "    \"visualizerFrameCount\": " +
            std::to_string(status.visualizerFrameCount) + ",\n";
    json += "    \"visualizerPeak\": " +
            std::to_string(status.visualizerPeak) + ",\n";
    json += "    \"visualizerSessionPeak\": " +
            std::to_string(status.visualizerSessionPeak) + ",\n";
    json += "    \"visualizerTargetProcessId\": " +
            std::to_string(status.visualizerTargetProcessId) + ",\n";
    json += "    \"visualizerTargetMode\": \"" +
            JsonEscape(status.visualizerTargetMode) + "\",\n";
    json += "    \"updatedAtUnix\": " + std::to_string(now);
    if (!status.error.empty()) {
        json += ",\n    \"error\": \"" + JsonEscape(status.error) + "\"";
    }
    json += "\n  }\n}\n";

    if (!WriteRawFile(tempPath, json.data(), json.size())) {
        return;
    }

    MoveFileExW(tempPath.c_str(), path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

enum class MediaCommand {
    Toggle,
    Previous,
    Next
};

bool SendMediaCommand(MediaCommand command) {
    try {
        auto manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        MediaStatus transientStatus;
        auto selected = SelectBestMediaSession(manager, transientStatus);
        if (selected.session) {
            switch (command) {
                case MediaCommand::Previous:
                    return selected.session.TrySkipPreviousAsync().get();
                case MediaCommand::Next:
                    return selected.session.TrySkipNextAsync().get();
                default:
                    return selected.session.TryTogglePlayPauseAsync().get();
            }
        }
    } catch (...) {
        // The caller will use the system media-key fallback.
    }
    return false;
}

int wmain(int argc, wchar_t** argv) {
    Options options = ParseOptions(argc, argv);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    GdiplusSession gdiplus;

    if (options.toggle) {
        return SendMediaCommand(MediaCommand::Toggle) ? 0 : 2;
    }
    if (options.previous) {
        return SendMediaCommand(MediaCommand::Previous) ? 0 : 2;
    }
    if (options.next) {
        return SendMediaCommand(MediaCommand::Next) ? 0 : 2;
    }

    if (!options.watch) {
        WriteStatus(options.appDir, QueryStatus(options.appDir));
        return 0;
    }

    std::thread visualizerThread(RunAudioVisualizer);
    visualizerThread.detach();
    while (true) {
        WriteStatus(options.appDir, QueryStatus(options.appDir));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
