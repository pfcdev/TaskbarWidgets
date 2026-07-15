#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

namespace wf = winrt::Windows::Foundation;
namespace wmc = winrt::Windows::Media::Control;
namespace wss = winrt::Windows::Storage::Streams;
namespace gdi = Gdiplus;

struct Options {
    std::wstring appDir;
    bool watch{};
    bool toggle{};
};

struct MediaStatus {
    bool loaded{true};
    bool active{};
    bool playing{};
    bool stale{};
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

std::wstring ProcessBaseNameFromWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
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
    std::string pattern = std::string("\"") + key + "\":";
    size_t position = json.find(pattern);
    if (position == std::string::npos) {
        return false;
    }

    position = json.find('"', position + pattern.size());
    if (position == std::string::npos) {
        return false;
    }

    std::string raw;
    for (size_t i = position + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (ch == '"') {
            value = Utf8ToWide(raw);
            return true;
        }
        if (ch == '\\' && i + 1 < json.size()) {
            char next = json[++i];
            switch (next) {
                case '"':
                case '\\':
                    raw.push_back(next);
                    break;
                case 'n':
                    raw.push_back('\n');
                    break;
                case 'r':
                    raw.push_back('\r');
                    break;
                case 't':
                    raw.push_back('\t');
                    break;
                default:
                    break;
            }
        } else {
            raw.push_back(ch);
        }
    }

    return false;
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
    status.coverPath = JoinPath(AssetsWidgetDirectory(appDir), L"media_cover.png");
    MediaStatus previous = ReadPreviousStatus(appDir);

    try {
        auto manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        auto selected = SelectBestMediaSession(manager, status);
        if (!selected.session) {
            PreservePreviousMedia(status, previous);
            return status;
        }

        status.active = true;
        status.playing = selected.playing;
        status.sourceApp = selected.sourceApp;

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
        status.error = ex.message().c_str();
        PreservePreviousMedia(status, previous);
    } catch (...) {
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

void TogglePlayback(const std::wstring& appDir) {
    try {
        auto manager = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        MediaStatus transientStatus;
        auto selected = SelectBestMediaSession(manager, transientStatus);
        if (selected.session) {
            selected.session.TryTogglePlayPauseAsync().get();
        }
    } catch (...) {
        // Status refresh below will expose query errors if any.
    }

    WriteStatus(appDir, QueryStatus(appDir));
}

int wmain(int argc, wchar_t** argv) {
    Options options = ParseOptions(argc, argv);
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    GdiplusSession gdiplus;

    if (options.toggle) {
        TogglePlayback(options.appDir);
        return 0;
    }

    if (!options.watch) {
        WriteStatus(options.appDir, QueryStatus(options.appDir));
        return 0;
    }

    while (true) {
        WriteStatus(options.appDir, QueryStatus(options.appDir));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
