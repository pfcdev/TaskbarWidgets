// ==WindhawkMod==
// @id              taskbar-stats
// @name            TaskbarStats
// @description     Experimental static Windows 11 taskbar stats module
// @version         0.1.0
// @author          TaskbarStats
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lgdi32 -Wl,--export-all-symbols
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# TaskbarStats

Milestone 1 prototype. This mod inserts a static `[CPU --%] [RAM --%]` module
into the real Windows 11 taskbar XAML tree. It is not a tray icon, overlay,
always-on-top window, desktop widget, Widgets Board widget, or normal taskbar
button.

The implementation uses private Windows 11 Shell/XAML Diagnostics behavior.
Unsupported builds should fail closed and log diagnostics.
*/
// ==/WindhawkModReadme==

#include <windows.h>
#include <ocidl.h>
#include <xamlom.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>

#include <cstdarg>
#include <cstdio>

namespace wf = winrt::Windows::Foundation;
namespace wuc = winrt::Windows::UI::Core;
namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
namespace wuxcp = winrt::Windows::UI::Xaml::Controls::Primitives;
namespace wuxi = winrt::Windows::UI::Xaml::Input;
namespace wuxm = winrt::Windows::UI::Xaml::Media;
namespace wuxmi = winrt::Windows::UI::Xaml::Media::Imaging;

std::atomic_bool g_tapInitialized = false;
std::atomic_bool g_delayedInitializationScheduled = false;
std::atomic_bool g_uninitializing = false;
thread_local bool g_initializedForThread = false;
bool g_inInjectTaskbarStatsTap = false;
HWND g_win32ProofWindow = nullptr;
HWND g_win32ProofParent = nullptr;
HMODULE g_hookModule = nullptr;

std::wstring GetLocalAppDataTaskbarStatsPath(PCWSTR leaf) {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\TaskbarStats";
    CreateDirectory(path.c_str(), nullptr);
    path += L"\\Logs";
    CreateDirectory(path.c_str(), nullptr);
    path += L"\\";
    path += leaf;
    return path;
}

void Wh_Log(PCWSTR format, ...) {
    std::wstring path = GetLocalAppDataTaskbarStatsPath(L"hook.log");
    if (path.empty()) {
        return;
    }

    WCHAR message[1024]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(message, ARRAYSIZE(message), _TRUNCATE, format, args);
    va_end(args);

    SYSTEMTIME time{};
    GetLocalTime(&time);
    WCHAR line[1400]{};
    _snwprintf_s(line, ARRAYSIZE(line), _TRUNCATE,
                 L"%04u-%02u-%02uT%02u:%02u:%02u.%03u [hook] %s\r\n",
                 time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                 time.wSecond, time.wMilliseconds, message);

    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"a, ccs=UTF-8") == 0 && file) {
        fputws(line, file);
        fclose(file);
    }
}

std::wstring GetTaskbarStatsPath(PCWSTR leaf = nullptr) {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\TaskbarStats";
    CreateDirectory(path.c_str(), nullptr);
    if (leaf && *leaf) {
        path += L"\\";
        path += leaf;
    }

    return path;
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

std::string JsonEscapeUtf8(const std::wstring& value) {
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

void WriteTaskbarStatsCommand(const std::wstring& command,
                              const std::wstring& accountId = L"") {
    std::wstring directory = GetTaskbarStatsPath(L"Commands");
    if (directory.empty()) {
        return;
    }

    CreateDirectory(directory.c_str(), nullptr);

    SYSTEMTIME time{};
    GetSystemTime(&time);
    WCHAR fileName[128]{};
    swprintf_s(fileName, L"\\%04u%02u%02u%02u%02u%02u%03u_%u.json",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds, GetCurrentProcessId());

    std::wstring path = directory + fileName;
    std::string json = "{\"command\":\"" + JsonEscapeUtf8(command) + "\"";
    if (!accountId.empty()) {
        json += ",\"accountId\":\"" + JsonEscapeUtf8(accountId) + "\"";
    }
    json += "}\n";

    HANDLE file = CreateFile(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Failed to create command file: %u", GetLastError());
        return;
    }

    DWORD written = 0;
    WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written,
              nullptr);
    CloseHandle(file);
}

HMODULE GetCurrentModuleHandle() {
    HMODULE module = nullptr;
    GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                          GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                      reinterpret_cast<LPCWSTR>(&GetCurrentModuleHandle),
                      &module);
    return module;
}

struct InsertedModule {
    InstanceHandle anchorHandle{};
    wuxc::Grid parent{nullptr};
    wux::UIElement root{nullptr};
    wux::DispatcherTimer timer{nullptr};
    uint32_t insertedColumn{};
    bool insertedGridColumn{};
};

thread_local std::vector<InsertedModule> g_insertedModules;

std::wstring GetAssetsFolder() {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\TaskbarStats\\Assets\\";
    return path;
}

std::wstring ToFileUri(std::wstring path) {
    std::replace(path.begin(), path.end(), L'\\', L'/');
    return L"file:///" + path;
}

bool FileExists(const std::wstring& path) {
    DWORD attributes = GetFileAttributes(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

wux::UIElement MakeIcon(PCWSTR assetName, PCWSTR fallbackGlyph) {
    std::wstring assetPath = GetAssetsFolder();
    assetPath += assetName;

    if (FileExists(assetPath)) {
        wuxc::Image image;
        image.Width(16);
        image.Height(16);
        image.Stretch(wuxm::Stretch::Uniform);
        image.HorizontalAlignment(wux::HorizontalAlignment::Center);
        image.VerticalAlignment(wux::VerticalAlignment::Center);

        wuxmi::SvgImageSource source;
        source.UriSource(wf::Uri(ToFileUri(assetPath)));
        image.Source(source);
        return image;
    }

    wuxc::FontIcon icon;
    icon.Glyph(fallbackGlyph);
    icon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(14);
    icon.Width(16);
    icon.Height(16);
    icon.HorizontalAlignment(wux::HorizontalAlignment::Center);
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    icon.Foreground(wuxm::SolidColorBrush(winrt::Windows::UI::Color{
        0xFF, 0xF8, 0xFA, 0xFC}));
    return icon;
}

wuxc::TextBlock MakeText(PCWSTR text, double fontSize, BYTE alpha = 0xFF) {
    wuxc::TextBlock block;
    block.Text(text);
    block.FontSize(fontSize);
    block.HorizontalAlignment(wux::HorizontalAlignment::Center);
    block.TextAlignment(wux::TextAlignment::Center);
    block.LineHeight(fontSize + 1);
    block.Foreground(wuxm::SolidColorBrush(winrt::Windows::UI::Color{
        alpha, 0xF8, 0xFA, 0xFC}));
    return block;
}

void SetElementName(wux::FrameworkElement const& element, PCWSTR name) {
    element.Name(name);
}

wuxc::TextBlock MakeNamedText(PCWSTR name,
                              PCWSTR text,
                              double fontSize,
                              BYTE alpha = 0xFF) {
    auto block = MakeText(text, fontSize, alpha);
    SetElementName(block, name);
    return block;
}

wuxm::SolidColorBrush MakeBrush(BYTE a, BYTE r, BYTE g, BYTE b) {
    return wuxm::SolidColorBrush(winrt::Windows::UI::Color{a, r, g, b});
}

std::vector<std::wstring> GetAntigravityProjectTitles();
void SetExpandedMode(wux::UIElement const& root, bool expanded);
void ShowAccountMenu(wux::FrameworkElement const& root);

struct CodexAccountInfo {
    std::wstring id;
    std::wstring label;
    bool active{};
};

wuxc::FontIcon MakeNamedStateIcon(PCWSTR name) {
    wuxc::FontIcon icon;
    icon.Name(name);
    icon.Glyph(L"\xE73E");
    icon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(8);
    icon.Width(8);
    icon.Height(10);
    icon.Margin(wux::ThicknessHelper::FromLengths(2, 0, 1, 0));
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    icon.Foreground(MakeBrush(0xCC, 0x94, 0xA3, 0xB8));
    return icon;
}

wux::UIElement MakeSmallMetric(PCWSTR glyph,
                               PCWSTR valueName,
                               PCWSTR valueText) {
    wuxc::StackPanel metric;
    metric.Orientation(wuxc::Orientation::Horizontal);
    metric.VerticalAlignment(wux::VerticalAlignment::Center);
    metric.Margin(wux::ThicknessHelper::FromLengths(0, 0, 5, 0));

    wuxc::FontIcon icon;
    icon.Glyph(glyph);
    icon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(9);
    icon.Width(9);
    icon.Height(10);
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    icon.Foreground(MakeBrush(0xAA, 0xF8, 0xFA, 0xFC));

    auto value = MakeNamedText(valueName, valueText, 9, 0xD8);
    value.Margin(wux::ThicknessHelper::FromLengths(2, 0, 0, 0));
    value.VerticalAlignment(wux::VerticalAlignment::Center);

    metric.Children().Append(icon.as<wux::UIElement>());
    metric.Children().Append(value.as<wux::UIElement>());
    return metric;
}

wux::FrameworkElement MakeExpandedProjectRow(PCWSTR rowName,
                                             PCWSTR titleName,
                                             PCWSTR iconName,
                                             PCWSTR stateName) {
    wuxc::StackPanel row;
    row.Name(rowName);
    row.Orientation(wuxc::Orientation::Horizontal);
    row.HorizontalAlignment(wux::HorizontalAlignment::Center);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    row.Height(12);

    auto title = MakeNamedText(titleName, L"", 9, 0xF0);
    title.Width(126);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    title.VerticalAlignment(wux::VerticalAlignment::Center);

    auto icon = MakeNamedStateIcon(iconName);

    auto stateText = MakeNamedText(stateName, L"", 8, 0xCC);
    stateText.Width(30);
    stateText.HorizontalAlignment(wux::HorizontalAlignment::Left);
    stateText.TextAlignment(wux::TextAlignment::Left);
    stateText.VerticalAlignment(wux::VerticalAlignment::Center);
    stateText.Foreground(MakeBrush(0xCC, 0x94, 0xA3, 0xB8));

    row.Children().Append(title.as<wux::UIElement>());
    row.Children().Append(icon.as<wux::UIElement>());
    row.Children().Append(stateText.as<wux::UIElement>());
    return row;
}

wux::FrameworkElement MakeTaskbarStatsRoot() {
    wuxc::Grid root;
    root.Name(L"TaskbarStatsRoot");
    root.VerticalAlignment(wux::VerticalAlignment::Center);
    root.HorizontalAlignment(wux::HorizontalAlignment::Right);
    root.Height(36);
    root.Width(184);
    root.Margin(wux::ThicknessHelper::FromLengths(6, 0, 6, 0));

    wuxc::Grid compact;
    compact.Name(L"TaskbarStatsCompactPanel");
    compact.Height(36);
    compact.Width(184);

    wuxc::RowDefinition titleRow;
    titleRow.Height(wux::GridLengthHelper::FromPixels(14));
    wuxc::RowDefinition lineRow;
    lineRow.Height(wux::GridLengthHelper::FromPixels(1));
    wuxc::RowDefinition metricsRow;
    metricsRow.Height(wux::GridLengthHelper::FromPixels(17));
    compact.RowDefinitions().Append(titleRow);
    compact.RowDefinitions().Append(lineRow);
    compact.RowDefinitions().Append(metricsRow);

    wuxc::StackPanel titleLine;
    titleLine.Orientation(wuxc::Orientation::Horizontal);
    titleLine.HorizontalAlignment(wux::HorizontalAlignment::Center);
    titleLine.VerticalAlignment(wux::VerticalAlignment::Center);

    auto title = MakeNamedText(L"TaskbarStatsTitle", L"Antigravity", 10, 0xF0);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    title.Width(104);

    auto stateIcon = MakeNamedStateIcon(L"TaskbarStatsStateIcon");

    auto stateText = MakeNamedText(L"TaskbarStatsStateText", L"IDLE", 8, 0xCC);
    stateText.Width(26);
    stateText.HorizontalAlignment(wux::HorizontalAlignment::Left);
    stateText.TextAlignment(wux::TextAlignment::Left);
    stateText.VerticalAlignment(wux::VerticalAlignment::Center);
    stateText.Foreground(MakeBrush(0xCC, 0x94, 0xA3, 0xB8));

    titleLine.Children().Append(title.as<wux::UIElement>());
    titleLine.Children().Append(stateIcon.as<wux::UIElement>());
    titleLine.Children().Append(stateText.as<wux::UIElement>());

    wuxc::Border separator;
    separator.Height(1);
    separator.Width(126);
    separator.HorizontalAlignment(wux::HorizontalAlignment::Center);
    separator.Background(wuxm::SolidColorBrush(winrt::Windows::UI::Color{
        0x34, 0xF8, 0xFA, 0xFC}));

    wuxc::StackPanel metrics;
    metrics.Orientation(wuxc::Orientation::Horizontal);
    metrics.HorizontalAlignment(wux::HorizontalAlignment::Center);
    metrics.VerticalAlignment(wux::VerticalAlignment::Center);
    metrics.Children().Append(MakeSmallMetric(L"\xE950", L"TaskbarStatsLimit", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE823", L"TaskbarStatsReset", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE9D2", L"TaskbarStatsWeek", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE8D4", L"TaskbarStatsTokens", L"--"));

    wuxc::Grid::SetRow(titleLine, 0);
    wuxc::Grid::SetRow(separator, 1);
    wuxc::Grid::SetRow(metrics, 2);
    compact.Children().Append(titleLine.as<wux::UIElement>());
    compact.Children().Append(separator.as<wux::UIElement>());
    compact.Children().Append(metrics.as<wux::UIElement>());

    wuxc::Grid expanded;
    expanded.Name(L"TaskbarStatsExpandedPanel");
    expanded.Height(36);
    expanded.Width(184);
    expanded.Visibility(wux::Visibility::Collapsed);

    for (int i = 0; i < 3; ++i) {
        wuxc::RowDefinition row;
        row.Height(wux::GridLengthHelper::FromPixels(12));
        expanded.RowDefinitions().Append(row);
    }

    auto expandedRow0 = MakeExpandedProjectRow(
        L"TaskbarStatsExpandedRow0",
        L"TaskbarStatsExpandedTitle0",
        L"TaskbarStatsExpandedIcon0",
        L"TaskbarStatsExpandedState0");
    auto expandedRow1 = MakeExpandedProjectRow(
        L"TaskbarStatsExpandedRow1",
        L"TaskbarStatsExpandedTitle1",
        L"TaskbarStatsExpandedIcon1",
        L"TaskbarStatsExpandedState1");
    auto expandedRow2 = MakeExpandedProjectRow(
        L"TaskbarStatsExpandedRow2",
        L"TaskbarStatsExpandedTitle2",
        L"TaskbarStatsExpandedIcon2",
        L"TaskbarStatsExpandedState2");
    wuxc::Grid::SetRow(expandedRow0, 0);
    wuxc::Grid::SetRow(expandedRow1, 1);
    wuxc::Grid::SetRow(expandedRow2, 2);
    expanded.Children().Append(expandedRow0.as<wux::UIElement>());
    expanded.Children().Append(expandedRow1.as<wux::UIElement>());
    expanded.Children().Append(expandedRow2.as<wux::UIElement>());

    root.Children().Append(compact.as<wux::UIElement>());
    root.Children().Append(expanded.as<wux::UIElement>());
    root.PointerEntered([root](auto const&, auto const&) {
        SetExpandedMode(root.as<wux::UIElement>(), true);
    });
    root.PointerExited([root](auto const&, auto const&) {
        SetExpandedMode(root.as<wux::UIElement>(), false);
    });
    root.Tapped([root](auto const&, wuxi::TappedRoutedEventArgs const& args) {
        ShowAccountMenu(root);
        args.Handled(true);
    });

    return root;
}

std::wstring GetCodexStatusPath() {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\TaskbarStats\\codex-status.json";
    return path;
}

std::string ReadUtf8File(const std::wstring& path) {
    HANDLE file = CreateFile(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
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
    BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read,
                       nullptr);
    CloseHandle(file);

    if (!ok) {
        return {};
    }

    data.resize(read);
    return data;
}

bool ExtractJsonString(const std::string& json,
                       const char* key,
                       std::wstring& value) {
    std::string pattern = "\"";
    pattern += key;
    pattern += "\":";

    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return false;
    }

    size_t start = json.find_first_not_of(" \t\r\n", keyPos + pattern.size());
    if (start == std::string::npos || json[start] != '"') {
        return false;
    }
    ++start;

    size_t end = start;
    bool escaped = false;
    while (end < json.size()) {
        char ch = json[end];
        if (!escaped && ch == '"') {
            break;
        }

        escaped = !escaped && ch == '\\';
        if (ch != '\\') {
            escaped = false;
        }
        ++end;
    }

    if (end >= json.size()) {
        return false;
    }

    std::string raw = json.substr(start, end - start);
    int wideLength = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(),
                                         static_cast<int>(raw.size()), nullptr, 0);
    if (wideLength <= 0) {
        value.assign(raw.begin(), raw.end());
        return true;
    }

    value.resize(wideLength);
    MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), static_cast<int>(raw.size()),
                        value.data(), wideLength);
    return true;
}

std::vector<CodexAccountInfo> ReadCodexAccounts() {
    std::vector<CodexAccountInfo> accounts;
    std::wstring settingsPath = GetTaskbarStatsPath(L"settings.json");
    std::string json = ReadUtf8File(settingsPath);
    std::wstring activeAccountId;
    ExtractJsonString(json, "activeAccountId", activeAccountId);

    size_t accountsKey = json.find("\"accounts\"");
    size_t arrayStart = accountsKey == std::string::npos
                            ? std::string::npos
                            : json.find('[', accountsKey);
    size_t arrayEnd = arrayStart == std::string::npos
                          ? std::string::npos
                          : json.find(']', arrayStart);
    if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
        size_t cursor = arrayStart;
        while (cursor < arrayEnd && accounts.size() < 5) {
            size_t objectStart = json.find('{', cursor);
            if (objectStart == std::string::npos || objectStart >= arrayEnd) {
                break;
            }

            size_t objectEnd = json.find('}', objectStart);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd) {
                break;
            }

            std::string object = json.substr(objectStart, objectEnd - objectStart + 1);
            CodexAccountInfo account;
            ExtractJsonString(object, "id", account.id);
            ExtractJsonString(object, "label", account.label);
            if (!account.id.empty()) {
                if (account.label.empty()) {
                    account.label = account.id;
                }
                account.active =
                    _wcsicmp(account.id.c_str(), activeAccountId.c_str()) == 0;
                accounts.push_back(account);
            }

            cursor = objectEnd + 1;
        }
    }

    if (accounts.empty()) {
        accounts.push_back({L"default", L"Default", true});
    } else if (std::none_of(accounts.begin(), accounts.end(),
                            [](const CodexAccountInfo& account) {
                                return account.active;
                            })) {
        accounts[0].active = true;
    }

    return accounts;
}

void ShowAccountMenu(wux::FrameworkElement const& root) {
    try {
        auto flyout = wuxc::MenuFlyout();
        auto accounts = ReadCodexAccounts();

        for (const auto& account : accounts) {
            auto item = wuxc::MenuFlyoutItem();
            std::wstring label = account.active ? L"* " : L"";
            label += account.label;
            item.Text(label);

            std::wstring accountId = account.id;
            item.Click([accountId](auto const&, auto const&) {
                WriteTaskbarStatsCommand(L"switchAccount", accountId);
            });
            flyout.Items().Append(item);
        }

        auto separator = wuxc::MenuFlyoutSeparator();
        flyout.Items().Append(separator);

        auto addItem = wuxc::MenuFlyoutItem();
        addItem.Text(L"Add Codex account");
        addItem.Click([](auto const&, auto const&) {
            WriteTaskbarStatsCommand(L"addAccount");
        });
        flyout.Items().Append(addItem);

        auto restartIdeItem = wuxc::MenuFlyoutItem();
        restartIdeItem.Text(L"Restart IDE with active account");
        restartIdeItem.Click([](auto const&, auto const&) {
            WriteTaskbarStatsCommand(L"restartIde");
        });
        flyout.Items().Append(restartIdeItem);

        wuxcp::FlyoutBase::SetAttachedFlyout(root, flyout);
        wuxcp::FlyoutBase::ShowAttachedFlyout(root);
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"ShowAccountMenu failed: 0x%08X %s", ex.code(),
               ex.message().c_str());
    } catch (...) {
        Wh_Log(L"ShowAccountMenu failed with unknown exception");
    }
}

bool ExtractJsonDouble(const std::string& json, const char* key, double& value) {
    std::string pattern = "\"";
    pattern += key;
    pattern += "\":";

    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return false;
    }

    size_t start = json.find_first_not_of(" \t\r\n", keyPos + pattern.size());
    if (start == std::string::npos || json.compare(start, 4, "null") == 0) {
        return false;
    }

    char* end = nullptr;
    value = strtod(json.c_str() + start, &end);
    return end && end != json.c_str() + start;
}

bool ExtractJsonInt64(const std::string& json, const char* key, long long& value) {
    double number = 0;
    if (!ExtractJsonDouble(json, key, number)) {
        return false;
    }

    value = static_cast<long long>(number);
    return true;
}

struct CodexStatusSnapshot {
    bool loaded{};
    std::wstring status = L"WAIT";
    std::wstring planType = L"--";
    long long updatedAtUnix = 0;
    double primaryUsedPercent = -1;
    double secondaryUsedPercent = -1;
    long long primaryWindowMins = 0;
    long long primaryResetsAtUnix = 0;
    long long secondaryResetsAtUnix = 0;
    long long tokens30d = 0;
    long long threadCount30d = 0;
    std::wstring taskState = L"IDLE";
    std::wstring taskLabel = L"IDLE";
    std::wstring taskTitle;
    long long taskUpdatedAtUnix = 0;
};

CodexStatusSnapshot ReadCodexStatusSnapshot() {
    CodexStatusSnapshot snapshot;
    std::string json = ReadUtf8File(GetCodexStatusPath());
    if (json.empty()) {
        return snapshot;
    }

    snapshot.loaded = true;
    ExtractJsonInt64(json, "updatedAtUnix", snapshot.updatedAtUnix);
    ExtractJsonString(json, "status", snapshot.status);
    ExtractJsonString(json, "planType", snapshot.planType);
    ExtractJsonDouble(json, "primaryUsedPercent", snapshot.primaryUsedPercent);
    ExtractJsonDouble(json, "secondaryUsedPercent",
                      snapshot.secondaryUsedPercent);
    ExtractJsonInt64(json, "primaryWindowMins",
                     snapshot.primaryWindowMins);
    ExtractJsonInt64(json, "primaryResetsAtUnix",
                     snapshot.primaryResetsAtUnix);
    ExtractJsonInt64(json, "secondaryResetsAtUnix",
                     snapshot.secondaryResetsAtUnix);
    ExtractJsonInt64(json, "tokens30d", snapshot.tokens30d);
    ExtractJsonInt64(json, "threadCount30d", snapshot.threadCount30d);
    ExtractJsonString(json, "taskState", snapshot.taskState);
    ExtractJsonString(json, "taskLabel", snapshot.taskLabel);
    ExtractJsonString(json, "taskTitle", snapshot.taskTitle);
    ExtractJsonInt64(json, "taskUpdatedAtUnix", snapshot.taskUpdatedAtUnix);
    return snapshot;
}

long long CurrentUnixTime() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);

    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;

    constexpr unsigned long long windowsToUnix100ns = 116444736000000000ULL;
    return static_cast<long long>((value.QuadPart - windowsToUnix100ns) /
                                  10000000ULL);
}

std::wstring FormatPercent(double value) {
    if (value < 0) {
        return L"--";
    }

    WCHAR buffer[16]{};
    swprintf_s(buffer, L"%.0f%%", value);
    return buffer;
}

std::wstring FormatRemainingPercent(double usedPercent) {
    if (usedPercent < 0) {
        return L"--";
    }

    double remaining = 100.0 - usedPercent;
    if (remaining < 0) {
        remaining = 0;
    } else if (remaining > 100) {
        remaining = 100;
    }

    return FormatPercent(remaining);
}

std::wstring FormatTokenCount(long long value) {
    WCHAR buffer[32]{};
    if (value >= 1000000000LL) {
        swprintf_s(buffer, L"%.1fB", value / 1000000000.0);
    } else if (value >= 1000000LL) {
        swprintf_s(buffer, L"%.0fM", value / 1000000.0);
    } else if (value >= 1000LL) {
        swprintf_s(buffer, L"%.0fK", value / 1000.0);
    } else {
        swprintf_s(buffer, L"%lld", value);
    }

    return buffer;
}

std::wstring FormatReset(long long unixTime) {
    if (unixTime <= 0) {
        return L"--";
    }

    long long seconds = unixTime - CurrentUnixTime();
    if (seconds <= 0) {
        return L"NOW";
    }

    long long minutes = (seconds + 59) / 60;
    WCHAR buffer[32]{};
    if (minutes < 60) {
        swprintf_s(buffer, L"%lldM", minutes);
    } else if (minutes < 60 * 24) {
        swprintf_s(buffer, L"%lldH", (minutes + 59) / 60);
    } else {
        swprintf_s(buffer, L"%lldD", (minutes + 60 * 24 - 1) / (60 * 24));
    }

    return buffer;
}

std::wstring FormatDurationFromMinutes(long long minutes) {
    if (minutes <= 0) {
        return L"--";
    }

    WCHAR buffer[32]{};
    if (minutes < 60) {
        swprintf_s(buffer, L"%lldM", minutes);
    } else if (minutes < 60 * 24) {
        swprintf_s(buffer, L"%lldH", minutes / 60);
    } else {
        swprintf_s(buffer, L"%lldD", minutes / (60 * 24));
    }

    return buffer;
}

std::wstring FormatAge(long long updatedAtUnix) {
    if (updatedAtUnix <= 0) {
        return L"--";
    }

    long long seconds = CurrentUnixTime() - updatedAtUnix;
    if (seconds < 0) {
        seconds = 0;
    }

    if (seconds < 60) {
        return L"0M";
    }

    return FormatDurationFromMinutes((seconds + 59) / 60);
}

std::wstring Trim(const std::wstring& value) {
    size_t start = 0;
    while (start < value.size() && iswspace(value[start])) {
        ++start;
    }

    size_t end = value.size();
    while (end > start && iswspace(value[end - 1])) {
        --end;
    }

    return value.substr(start, end - start);
}

std::wstring ExtractAntigravityProjectTitle(const std::wstring& title) {
    size_t marker = title.find(L" - Antigravity");
    if (marker == std::wstring::npos) {
        marker = title.find(L"Antigravity");
    }

    if (marker == std::wstring::npos) {
        return {};
    }

    std::wstring project = Trim(title.substr(0, marker));
    if (!project.empty()) {
        return project;
    }

    return Trim(title);
}

std::vector<std::wstring> GetAntigravityProjectTitles() {
    std::vector<std::wstring> titles;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            if (!IsWindowVisible(window)) {
                return TRUE;
            }

            WCHAR title[512]{};
            int length = GetWindowText(window, title, ARRAYSIZE(title));
            if (length <= 0) {
                return TRUE;
            }

            std::wstring project = ExtractAntigravityProjectTitle(title);
            if (!project.empty() &&
                std::find(reinterpret_cast<std::vector<std::wstring>*>(parameter)->begin(),
                          reinterpret_cast<std::vector<std::wstring>*>(parameter)->end(),
                          project) ==
                    reinterpret_cast<std::vector<std::wstring>*>(parameter)->end()) {
                reinterpret_cast<std::vector<std::wstring>*>(parameter)
                    ->push_back(project);
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&titles));

    std::sort(titles.begin(), titles.end());
    return titles;
}

wuxc::TextBlock FindNamedTextBlock(wux::DependencyObject const& root,
                                   PCWSTR name) {
    auto element = root.try_as<wux::FrameworkElement>();
    if (element && element.Name() == name) {
        return element.try_as<wuxc::TextBlock>();
    }

    auto panel = root.try_as<wuxc::Panel>();
    if (panel) {
        for (auto const& child : panel.Children()) {
            auto found = FindNamedTextBlock(child, name);
            if (found) {
                return found;
            }
        }
    }

    auto border = root.try_as<wuxc::Border>();
    if (border && border.Child()) {
        return FindNamedTextBlock(border.Child(), name);
    }

    return nullptr;
}

wuxc::FontIcon FindNamedFontIcon(wux::DependencyObject const& root,
                                 PCWSTR name) {
    auto element = root.try_as<wux::FrameworkElement>();
    if (element && element.Name() == name) {
        return element.try_as<wuxc::FontIcon>();
    }

    auto panel = root.try_as<wuxc::Panel>();
    if (panel) {
        for (auto const& child : panel.Children()) {
            auto found = FindNamedFontIcon(child, name);
            if (found) {
                return found;
            }
        }
    }

    auto border = root.try_as<wuxc::Border>();
    if (border && border.Child()) {
        return FindNamedFontIcon(border.Child(), name);
    }

    return nullptr;
}

wux::FrameworkElement FindNamedFrameworkElement(
    wux::DependencyObject const& root,
    PCWSTR name) {
    auto element = root.try_as<wux::FrameworkElement>();
    if (element && element.Name() == name) {
        return element;
    }

    auto panel = root.try_as<wuxc::Panel>();
    if (panel) {
        for (auto const& child : panel.Children()) {
            auto found = FindNamedFrameworkElement(child, name);
            if (found) {
                return found;
            }
        }
    }

    auto border = root.try_as<wuxc::Border>();
    if (border && border.Child()) {
        return FindNamedFrameworkElement(border.Child(), name);
    }

    return nullptr;
}

void SetNamedText(wux::UIElement const& root,
                  PCWSTR name,
                  const std::wstring& text) {
    auto block = FindNamedTextBlock(root, name);
    if (block && block.Text() != text) {
        block.Text(text);
    }
}

void SetNamedVisibility(wux::UIElement const& root,
                        PCWSTR name,
                        wux::Visibility visibility) {
    auto element = FindNamedFrameworkElement(root, name);
    if (element && element.Visibility() != visibility) {
        element.Visibility(visibility);
    }
}

struct TaskVisual {
    PCWSTR label;
    PCWSTR glyph;
    winrt::Windows::UI::Color color;
};

TaskVisual GetTaskVisual(const std::wstring& state,
                         const std::wstring& label) {
    if (_wcsicmp(state.c_str(), L"RUN") == 0) {
        return {label.empty() ? L"RUN" : label.c_str(), L"\xE768",
                {0xFF, 0x22, 0xC5, 0x5E}};
    }

    if (_wcsicmp(state.c_str(), L"TOOL") == 0) {
        return {label.empty() ? L"TOOL" : label.c_str(), L"\xE90F",
                {0xFF, 0xF5, 0x9E, 0x0B}};
    }

    if (_wcsicmp(state.c_str(), L"STOP") == 0) {
        return {label.empty() ? L"STOP" : label.c_str(), L"\xE711",
                {0xFF, 0xEF, 0x44, 0x44}};
    }

    if (_wcsicmp(label.c_str(), L"DONE") == 0) {
        return {L"DONE", L"\xE8FB", {0xFF, 0x38, 0xBD, 0xF8}};
    }

    return {label.empty() ? L"IDLE" : label.c_str(), L"\xE73E",
            {0xFF, 0x94, 0xA3, 0xB8}};
}

void SetTaskStateVisualNamed(wux::UIElement const& root,
                             PCWSTR iconName,
                             PCWSTR textName,
                             const std::wstring& state,
                             const std::wstring& label) {
    TaskVisual visual = GetTaskVisual(state, label);
    auto brush = wuxm::SolidColorBrush(visual.color);

    auto icon = FindNamedFontIcon(root, iconName);
    if (icon) {
        icon.Glyph(visual.glyph);
        icon.Foreground(brush);
    }

    auto text = FindNamedTextBlock(root, textName);
    if (text) {
        text.Text(visual.label);
        text.Foreground(brush);
    }
}

void SetTaskStateVisual(wux::UIElement const& root,
                        const std::wstring& state,
                        const std::wstring& label) {
    SetTaskStateVisualNamed(root, L"TaskbarStatsStateIcon",
                            L"TaskbarStatsStateText", state, label);
}

void UpdateExpandedTaskRows(wux::UIElement const& root,
                            const std::vector<std::wstring>& titles,
                            const std::wstring& state,
                            const std::wstring& label) {
    constexpr PCWSTR rowNames[] = {
        L"TaskbarStatsExpandedRow0",
        L"TaskbarStatsExpandedRow1",
        L"TaskbarStatsExpandedRow2"};
    constexpr PCWSTR titleNames[] = {
        L"TaskbarStatsExpandedTitle0",
        L"TaskbarStatsExpandedTitle1",
        L"TaskbarStatsExpandedTitle2"};
    constexpr PCWSTR iconNames[] = {
        L"TaskbarStatsExpandedIcon0",
        L"TaskbarStatsExpandedIcon1",
        L"TaskbarStatsExpandedIcon2"};
    constexpr PCWSTR stateNames[] = {
        L"TaskbarStatsExpandedState0",
        L"TaskbarStatsExpandedState1",
        L"TaskbarStatsExpandedState2"};

    for (uint32_t i = 0; i < ARRAYSIZE(rowNames); ++i) {
        if (i < titles.size()) {
            SetNamedVisibility(root, rowNames[i], wux::Visibility::Visible);
            SetNamedText(root, titleNames[i], titles[i]);
            SetTaskStateVisualNamed(root, iconNames[i], stateNames[i], state,
                                    label);
        } else {
            SetNamedVisibility(root, rowNames[i], wux::Visibility::Collapsed);
            SetNamedText(root, titleNames[i], L"");
            SetNamedText(root, stateNames[i], L"");
        }
    }
}

void SetExpandedMode(wux::UIElement const& root, bool expanded) {
    if (expanded && GetAntigravityProjectTitles().size() <= 1) {
        expanded = false;
    }

    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement) {
        rootElement.Width(184);
    }

    SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                       expanded ? wux::Visibility::Collapsed
                                : wux::Visibility::Visible);
    SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                       expanded ? wux::Visibility::Visible
                                : wux::Visibility::Collapsed);
}

void UpdateTaskbarStatsRoot(wux::UIElement const& root) {
    CodexStatusSnapshot snapshot = ReadCodexStatusSnapshot();
    if (!snapshot.loaded) {
        SetNamedText(root, L"TaskbarStatsTitle", L"Antigravity");
        SetTaskStateVisual(root, L"IDLE", L"IDLE");
        UpdateExpandedTaskRows(root, {}, L"IDLE", L"IDLE");
        SetNamedText(root, L"TaskbarStatsLimit", L"--");
        SetNamedText(root, L"TaskbarStatsReset", L"--");
        SetNamedText(root, L"TaskbarStatsWeek", L"--");
        SetNamedText(root, L"TaskbarStatsTokens", L"--");
        return;
    }

    std::vector<std::wstring> antigravityTitles = GetAntigravityProjectTitles();
    if (!antigravityTitles.empty()) {
        uint32_t titleIndex = 0;
        if (antigravityTitles.size() > 1) {
            titleIndex = static_cast<uint32_t>(
                (CurrentUnixTime() / 5) % antigravityTitles.size());
        }

        SetNamedText(root, L"TaskbarStatsTitle",
                     antigravityTitles[titleIndex]);
    } else {
        SetNamedText(root, L"TaskbarStatsTitle", L"Antigravity");
    }

    SetTaskStateVisual(root, snapshot.taskState, snapshot.taskLabel);
    UpdateExpandedTaskRows(root, antigravityTitles, snapshot.taskState,
                           snapshot.taskLabel);

    SetNamedText(root, L"TaskbarStatsLimit",
                 FormatRemainingPercent(snapshot.primaryUsedPercent));
    SetNamedText(root, L"TaskbarStatsReset",
                 FormatReset(snapshot.primaryResetsAtUnix));
    SetNamedText(root, L"TaskbarStatsWeek",
                 FormatRemainingPercent(snapshot.secondaryUsedPercent));
    SetNamedText(root, L"TaskbarStatsTokens",
                 FormatTokenCount(snapshot.tokens30d));
}

wux::DispatcherTimer StartTaskbarStatsTimer(wux::UIElement const& root) {
    wux::DispatcherTimer timer;
    timer.Interval(std::chrono::seconds(1));
    timer.Tick([root](auto const&, auto const&) {
        try {
            UpdateTaskbarStatsRoot(root);
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"TaskbarStats update failed: 0x%08X %s", ex.code(),
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"TaskbarStats update failed with unknown exception");
        }
    });
    UpdateTaskbarStatsRoot(root);
    timer.Start();
    return timer;
}

bool HasTaskbarStatsChild(wuxc::Grid const& parent) {
    for (auto const& child : parent.Children()) {
        auto element = child.try_as<wux::FrameworkElement>();
        if (element && element.Name() == L"TaskbarStatsRoot") {
            return true;
        }
    }

    return false;
}

void RemoveInsertedModule(InsertedModule& module) {
    try {
        if (module.timer) {
            module.timer.Stop();
            module.timer = nullptr;
        }

        if (module.parent && module.root) {
            auto children = module.parent.Children();
            uint32_t childIndex = 0;
            if (children.IndexOf(module.root, childIndex)) {
                children.RemoveAt(childIndex);
            }
        }

        if (module.parent && module.insertedGridColumn) {
            auto children = module.parent.Children();
            for (auto const& child : children) {
                auto frameworkElement = child.try_as<wux::FrameworkElement>();
                if (!frameworkElement) {
                    continue;
                }

                int column = wuxc::Grid::GetColumn(frameworkElement);
                if (column > static_cast<int>(module.insertedColumn)) {
                    wuxc::Grid::SetColumn(frameworkElement, column - 1);
                }
            }

            auto columns = module.parent.ColumnDefinitions();
            if (module.insertedColumn < columns.Size()) {
                columns.RemoveAt(module.insertedColumn);
            }
        }
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"TaskbarStats cleanup failed: 0x%08X %s", ex.code(),
               ex.message().c_str());
    }
}

void CleanupAllInsertedModules() {
    for (auto& module : g_insertedModules) {
        RemoveInsertedModule(module);
    }

    g_insertedModules.clear();
}

void CleanupInsertedModuleForAnchor(InstanceHandle handle) {
    auto it = std::remove_if(
        g_insertedModules.begin(), g_insertedModules.end(),
        [handle](InsertedModule& module) {
            if (module.anchorHandle != handle) {
                return false;
            }

            RemoveInsertedModule(module);
            return true;
        });

    g_insertedModules.erase(it, g_insertedModules.end());
}

bool TryInsertNextToSystemTray(InstanceHandle handle,
                               wux::FrameworkElement const& element,
                               PCWSTR typeName) {
    if (!typeName || wcscmp(typeName, L"SystemTray.SystemTrayFrame") != 0) {
        return false;
    }

    auto parent = element.Parent().try_as<wuxc::Grid>();
    if (!parent) {
        Wh_Log(L"SystemTray.SystemTrayFrame parent is not a Grid");
        return false;
    }

    if (HasTaskbarStatsChild(parent)) {
        Wh_Log(L"TaskbarStatsRoot already exists for this taskbar parent");
        return true;
    }

    auto trayElement = element.as<wux::UIElement>();
    auto children = parent.Children();
    uint32_t trayChildIndex = 0;
    if (!children.IndexOf(trayElement, trayChildIndex)) {
        Wh_Log(L"SystemTray.SystemTrayFrame was not found in parent children");
        return false;
    }

    int trayColumn = wuxc::Grid::GetColumn(element);
    if (trayColumn < 0) {
        trayColumn = 0;
    }

    auto columns = parent.ColumnDefinitions();
    uint32_t columnCount = columns.Size();
    uint32_t targetColumn = 0;
    bool insertedGridColumn = false;

    if (columnCount == 0) {
        Wh_Log(L"Taskbar grid has no explicit columns; using right-aligned "
               L"tray margin layout");
        targetColumn = 0;
    } else if (trayColumn > static_cast<int>(columnCount)) {
        Wh_Log(L"Taskbar grid tray column %d exceeds column count %u; "
               L"inserting at end",
               trayColumn, columnCount);
        targetColumn = columnCount;
    } else {
        targetColumn = static_cast<uint32_t>(trayColumn);
    }

    if (columnCount > 0) {
        wuxc::ColumnDefinition column;
        column.Width(wux::GridLengthHelper::Auto());
        columns.InsertAt(targetColumn, column);
        insertedGridColumn = true;

        for (auto const& child : children) {
            auto frameworkElement = child.try_as<wux::FrameworkElement>();
            if (!frameworkElement) {
                continue;
            }

            int columnIndex = wuxc::Grid::GetColumn(frameworkElement);
            if (columnIndex >= static_cast<int>(targetColumn)) {
                wuxc::Grid::SetColumn(frameworkElement, columnIndex + 1);
            }
        }
    }

    auto root = MakeTaskbarStatsRoot();
    if (columnCount == 0) {
        double trayWidth = element.ActualWidth();
        if (trayWidth < 48) {
            trayWidth = 190;
        }

        root.Margin(wux::ThicknessHelper::FromLengths(0, 0, trayWidth + 12, 0));
        Wh_Log(L"Right-aligning TaskbarStatsRoot with tray width %.1f",
               trayWidth);
    }

    // Prefer a real auto-width taskbar grid slot before the system tray when
    // explicit columns exist. Otherwise use right alignment with tray margin,
    // because adding a first column to the no-column parent centers the module.
    wuxc::Grid::SetColumn(root, targetColumn);
    wuxc::Grid::SetRow(root, wuxc::Grid::GetRow(element));
    wuxc::Canvas::SetZIndex(root, 10000);

    children.InsertAt(trayChildIndex, root.as<wux::UIElement>());
    auto timer = StartTaskbarStatsTimer(root.as<wux::UIElement>());

    g_insertedModules.push_back(InsertedModule{
        .anchorHandle = handle,
        .parent = parent,
        .root = root.as<wux::UIElement>(),
        .timer = timer,
        .insertedColumn = targetColumn,
        .insertedGridColumn = insertedGridColumn,
    });

    Wh_Log(L"Inserted TaskbarStatsRoot anchored to SystemTray.SystemTrayFrame");
    return true;
}

void ScheduleInsertNextToSystemTray(InstanceHandle handle,
                                    wux::FrameworkElement const& element,
                                    PCWSTR typeName) {
    if (!typeName || wcscmp(typeName, L"SystemTray.SystemTrayFrame") != 0) {
        return;
    }

    try {
        std::wstring stableTypeName = typeName;
        auto dispatcher = element.Dispatcher();
        if (!dispatcher) {
            Wh_Log(L"SystemTray.SystemTrayFrame has no dispatcher");
            return;
        }

        // Adding children while XAML Diagnostics reports a tree mutation can
        // destabilize Explorer on some builds. Queue the real insertion so the
        // taskbar tree finishes its current mutation first.
        auto action = dispatcher.RunAsync(
            wuc::CoreDispatcherPriority::Low,
            wuc::DispatchedHandler([handle, element, stableTypeName]() {
                TryInsertNextToSystemTray(handle, element,
                                          stableTypeName.c_str());
            }));
        (void)action;
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"Failed to schedule TaskbarStats insertion: 0x%08X %s",
               ex.code(), ex.message().c_str());
    }
}

class VisualTreeWatcher
    : public winrt::implements<VisualTreeWatcher,
                               IVisualTreeServiceCallback2,
                               winrt::non_agile> {
public:
    explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> site)
        : m_xamlDiagnostics(site.as<IXamlDiagnostics>()) {
        Wh_Log(L"TaskbarStats VisualTreeWatcher constructing");

        HANDLE thread = CreateThread(
            nullptr, 0,
            [](LPVOID parameter) -> DWORD {
                auto watcher = reinterpret_cast<VisualTreeWatcher*>(parameter);
                HRESULT hr =
                    watcher->m_xamlDiagnostics.as<IVisualTreeService3>()
                        ->AdviseVisualTreeChange(watcher);
                watcher->Release();
                if (FAILED(hr)) {
                    Wh_Log(L"AdviseVisualTreeChange failed: 0x%08X", hr);
                }
                return 0;
            },
            this, 0, nullptr);

        if (thread) {
            AddRef();
            CloseHandle(thread);
        }
    }

    void UnadviseVisualTreeChange() {
        HRESULT hr = m_xamlDiagnostics.as<IVisualTreeService3>()
                         ->UnadviseVisualTreeChange(this);
        if (FAILED(hr)) {
            Wh_Log(L"UnadviseVisualTreeChange failed: 0x%08X", hr);
        }
    }

private:
    wf::IInspectable FromHandle(InstanceHandle handle) {
        wf::IInspectable object;
        winrt::check_hresult(m_xamlDiagnostics->GetIInspectableFromHandle(
            handle,
            reinterpret_cast<::IInspectable**>(winrt::put_abi(object))));
        return object;
    }

    HRESULT STDMETHODCALLTYPE OnVisualTreeChange(
        ParentChildRelation,
        VisualElement element,
        VisualMutationType mutationType) noexcept override {
        try {
            if (!g_initializedForThread) {
                return S_OK;
            }

            if (mutationType == Add) {
                auto inspectable = FromHandle(element.Handle);
                auto frameworkElement =
                    inspectable.try_as<wux::FrameworkElement>();
                if (frameworkElement) {
                    ScheduleInsertNextToSystemTray(
                        element.Handle, frameworkElement, element.Type);
                }
            } else if (mutationType == Remove) {
                CleanupInsertedModuleForAnchor(element.Handle);
            }
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"OnVisualTreeChange failed: 0x%08X %s", ex.code(),
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"OnVisualTreeChange failed with unknown exception");
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnElementStateChanged(
        InstanceHandle,
        VisualElementState,
        LPCWSTR) noexcept override {
        return S_OK;
    }

    winrt::com_ptr<IXamlDiagnostics> m_xamlDiagnostics;
};

winrt::com_ptr<VisualTreeWatcher> g_visualTreeWatcher;

// {3A94E3F0-8D88-4725-9FE0-8C9F45529701}
static constexpr CLSID CLSID_TaskbarStatsTap = {
    0x3a94e3f0,
    0x8d88,
    0x4725,
    {0x9f, 0xe0, 0x8c, 0x9f, 0x45, 0x52, 0x97, 0x01}};

class TaskbarStatsTap
    : public winrt::implements<TaskbarStatsTap, IObjectWithSite,
                               winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        try {
            Wh_Log(L"TaskbarStatsTap::SetSite site=%p", site);

            if (g_visualTreeWatcher) {
                g_visualTreeWatcher->UnadviseVisualTreeChange();
                g_visualTreeWatcher = nullptr;
            }

            m_site.copy_from(site);
            if (m_site) {
                // Some Windhawk examples balance the module refcount added by
                // InitializeXamlDiagnosticsEx with FreeLibrary here. On this
                // Windows 11 build, the minimal TAP path crashed right after
                // InitializeXamlDiagnosticsEx succeeded. Keep the extra module
                // reference for now; stability is more important than allowing
                // immediate unload in Milestone 1.
                g_visualTreeWatcher = winrt::make_self<VisualTreeWatcher>(m_site);
            }

            return S_OK;
        } catch (...) {
            return winrt::to_hresult();
        }
    }

    HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override {
        if (!site) {
            return E_POINTER;
        }

        *site = nullptr;
        if (!m_site) {
            return E_FAIL;
        }

        return m_site.as(riid, site);
    }

private:
    winrt::com_ptr<IUnknown> m_site;
};

template <typename T>
class SimpleFactory : public winrt::implements<SimpleFactory<T>, IClassFactory> {
public:
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,
                                             REFIID riid,
                                             void** object) noexcept override {
        if (outer) {
            return CLASS_E_NOAGGREGATION;
        }

        return winrt::make<T>().as(riid, object);
    }

    HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override {
        return S_OK;
    }
};

extern "C" __declspec(dllexport) HRESULT __stdcall DllGetClassObject(
    REFCLSID clsid,
    REFIID riid,
    void** object) {
    Wh_Log(L"DllGetClassObject called");

    if (!IsEqualCLSID(clsid, CLSID_TaskbarStatsTap)) {
        Wh_Log(L"DllGetClassObject unknown CLSID");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    return winrt::make<SimpleFactory<TaskbarStatsTap>>().as(riid, object);
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX =
    decltype(&InitializeXamlDiagnosticsEx);

HRESULT InjectTaskbarStatsTap() noexcept {
    HMODULE module = GetCurrentModuleHandle();
    if (!module) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    WCHAR location[MAX_PATH]{};
    DWORD length = GetModuleFileName(module, location, ARRAYSIZE(location));
    if (length == 0 || length >= ARRAYSIZE(location)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HMODULE xaml = LoadLibraryEx(L"Windows.UI.Xaml.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!xaml) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    auto initializeXamlDiagnostics =
        reinterpret_cast<PFN_INITIALIZE_XAML_DIAGNOSTICS_EX>(
            GetProcAddress(xaml, "InitializeXamlDiagnosticsEx"));
    if (!initializeXamlDiagnostics) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    g_inInjectTaskbarStatsTap = true;

    HRESULT hr = E_FAIL;
    for (int i = 0; i < 10000; ++i) {
        WCHAR connectionName[64]{};
        wsprintf(connectionName, L"VisualDiagConnection%d", i + 1);
        hr = initializeXamlDiagnostics(connectionName, GetCurrentProcessId(), L"",
                                       location, CLSID_TaskbarStatsTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    g_inInjectTaskbarStatsTap = false;
    return hr;
}

void InitializeForCurrentThread() {
    if (g_initializedForThread) {
        return;
    }

    g_initializedForThread = true;
    Wh_Log(L"Initialized TaskbarStats for XAML thread %u",
           GetCurrentThreadId());
}

void UninitializeForCurrentThread() {
    CleanupAllInsertedModules();
    g_initializedForThread = false;
}

void InitializeTapOnce() {
    if (g_uninitializing) {
        return;
    }

    if (g_tapInitialized.exchange(true)) {
        return;
    }

    Wh_Log(L"InjectTaskbarStatsTap starting");
    HRESULT hr = InjectTaskbarStatsTap();
    if (FAILED(hr)) {
        Wh_Log(L"InjectTaskbarStatsTap failed: 0x%08X", hr);
        g_tapInitialized = false;
        return;
    }

    Wh_Log(L"InjectTaskbarStatsTap completed: 0x%08X", hr);
}

using RunFromWindowThreadProc = void(WINAPI*)(PVOID parameter);

bool RunFromWindowThread(HWND window,
                         RunFromWindowThreadProc proc,
                         PVOID procParam) {
    static const UINT registeredMessage =
        RegisterWindowMessage(L"TaskbarStats_RunFromWindowThread");

    struct RunParam {
        RunFromWindowThreadProc proc;
        PVOID procParam;
    };

    DWORD threadId = GetWindowThreadProcessId(window, nullptr);
    if (!threadId) {
        return false;
    }

    if (threadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int code, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (code == HC_ACTION) {
                auto cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
                if (cwp->message == registeredMessage) {
                    auto param = reinterpret_cast<RunParam*>(cwp->lParam);
                    param->proc(param->procParam);
                }
            }

            return CallNextHookEx(nullptr, code, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook) {
        return false;
    }

    RunParam param{proc, procParam};
    SendMessage(window, registeredMessage, 0, reinterpret_cast<LPARAM>(&param));
    UnhookWindowsHookEx(hook);
    return true;
}

HWND FindCurrentProcessTaskbarWindow() {
    HWND taskbar = nullptr;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            DWORD processId = 0;
            WCHAR className[64]{};
            if (GetWindowThreadProcessId(window, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassName(window, className, ARRAYSIZE(className)) &&
                _wcsicmp(className, L"Shell_TrayWnd") == 0) {
                *reinterpret_cast<HWND*>(parameter) = window;
                return FALSE;
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&taskbar));
    return taskbar;
}

HWND GetTaskbarUiWindow() {
    HWND taskbar = FindCurrentProcessTaskbarWindow();
    if (!taskbar) {
        return nullptr;
    }

    return FindWindowEx(taskbar, nullptr,
                        L"Windows.UI.Composition.DesktopWindowContentBridge",
                        nullptr);
}

LRESULT CALLBACK Win32ProofWindowProc(HWND window,
                                      UINT message,
                                      WPARAM wParam,
                                      LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);

            RECT rect;
            GetClientRect(window, &rect);

            HBRUSH background = CreateSolidBrush(RGB(90, 32, 32));
            FillRect(dc, &rect, background);
            DeleteObject(background);

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(248, 250, 252));

            HFONT font = CreateFont(
                -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            HGDIOBJ oldFont = SelectObject(dc, font);

            RECT cpuIcon{8, 9, 18, 19};
            HBRUSH cpuBrush = CreateSolidBrush(RGB(56, 189, 248));
            FillRect(dc, &cpuIcon, cpuBrush);
            DeleteObject(cpuBrush);

            RECT ramIcon{82, 9, 92, 19};
            HBRUSH ramBrush = CreateSolidBrush(RGB(52, 211, 153));
            FillRect(dc, &ramIcon, ramBrush);
            DeleteObject(ramBrush);

            RECT textRect{24, 0, 156, rect.bottom};
            DrawText(dc, L"CPU --%    RAM --%", -1, &textRect,
                     DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);

            SelectObject(dc, oldFont);
            DeleteObject(font);

            EndPaint(window, &ps);
            return 0;
        }
    }

    return DefWindowProc(window, message, wParam, lParam);
}

void PositionWin32ProofWindow() {
    if (!g_win32ProofWindow) {
        return;
    }

    HWND parent = g_win32ProofParent ? g_win32ProofParent : GetParent(g_win32ProofWindow);
    if (!parent) {
        return;
    }

    RECT rect;
    if (!GetClientRect(parent, &rect)) {
        return;
    }

    constexpr int width = 220;
    constexpr int height = 32;
    int x = std::max<int>(8, static_cast<int>(rect.right) - 650);
    int y = std::max<int>(0, (static_cast<int>(rect.bottom) - height) / 2);

    SetWindowPos(g_win32ProofWindow, HWND_TOP, x, y, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
    InvalidateRect(g_win32ProofWindow, nullptr, TRUE);
    UpdateWindow(g_win32ProofWindow);
}

void CreateWin32ProofModule() {
    if (g_win32ProofWindow) {
        PositionWin32ProofWindow();
        return;
    }

    HWND taskbar = FindCurrentProcessTaskbarWindow();
    HWND taskbarUi = GetTaskbarUiWindow();
    HWND parent = taskbarUi ? taskbarUi : taskbar;
    if (!parent) {
        Wh_Log(L"Win32 proof module skipped: no taskbar parent found");
        return;
    }

    g_win32ProofParent = parent;

    WCHAR parentClass[128]{};
    GetClassName(parent, parentClass, ARRAYSIZE(parentClass));
    RECT parentRect{};
    GetClientRect(parent, &parentRect);
    Wh_Log(L"Creating Win32 proof module parent=%s size=%dx%d",
           parentClass, parentRect.right - parentRect.left,
           parentRect.bottom - parentRect.top);

    constexpr PCWSTR className = L"TaskbarStatsWin32Proof";

    static bool classRegistered = false;
    if (!classRegistered) {
        WNDCLASS windowClass{};
        windowClass.lpfnWndProc = Win32ProofWindowProc;
        windowClass.hInstance = GetCurrentModuleHandle();
        windowClass.lpszClassName = className;
        windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);

        if (!RegisterClass(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            Wh_Log(L"RegisterClass failed for Win32 proof module: %u",
                   GetLastError());
            return;
        }

        classRegistered = true;
    }

    g_win32ProofWindow = CreateWindowEx(
        WS_EX_NOACTIVATE, className, L"TaskbarStats",
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0, 0, 220, 32, parent, nullptr, GetCurrentModuleHandle(), nullptr);
    if (!g_win32ProofWindow) {
        Wh_Log(L"CreateWindowEx failed for Win32 proof module: %u",
               GetLastError());
        return;
    }

    PositionWin32ProofWindow();
    Wh_Log(L"Created Win32 child proof module inside Shell_TrayWnd");
}

std::vector<HWND> GetXamlHostWindows() {
    std::vector<HWND> windows;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL {
            DWORD processId = 0;
            WCHAR className[64]{};
            if (GetWindowThreadProcessId(window, &processId) &&
                processId == GetCurrentProcessId() &&
                GetClassName(window, className, ARRAYSIZE(className)) &&
                (_wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0 ||
                 _wcsicmp(className, L"Shell_InputSwitchTopLevelWindow") == 0)) {
                reinterpret_cast<std::vector<HWND>*>(parameter)->push_back(window);
            }

            return TRUE;
        },
        reinterpret_cast<LPARAM>(&windows));
    return windows;
}

bool InitializeExistingTaskbarThreads() {
    bool foundAny = false;

    if (HWND taskbarUi = GetTaskbarUiWindow()) {
        foundAny = true;
        Wh_Log(L"Found taskbar DesktopWindowContentBridge");
        RunFromWindowThread(taskbarUi,
                            [](PVOID) { InitializeForCurrentThread(); },
                            nullptr);
    }

    for (HWND xamlHost : GetXamlHostWindows()) {
        foundAny = true;
        Wh_Log(L"Found XAML host window: %08X",
               static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(xamlHost)));
        RunFromWindowThread(xamlHost,
                            [](PVOID) { InitializeForCurrentThread(); },
                            nullptr);
    }

    if (foundAny) {
        return true;
    } else {
        Wh_Log(L"No taskbar XAML host was found yet");
        return false;
    }
}

void ScheduleDelayedInitialization(DWORD delayMs) {
    if (g_uninitializing) {
        return;
    }

    if (g_delayedInitializationScheduled.exchange(true)) {
        return;
    }

    HANDLE thread = CreateThread(
        nullptr, 0,
        [](LPVOID parameter) -> DWORD {
            DWORD delayMs = static_cast<DWORD>(
                reinterpret_cast<ULONG_PTR>(parameter));
            Sleep(delayMs);

            g_delayedInitializationScheduled = false;
            if (g_uninitializing) {
                return 0;
            }

            Wh_Log(L"Delayed initialization running");
            if (InitializeExistingTaskbarThreads()) {
                InitializeTapOnce();
            } else {
                ScheduleDelayedInitialization(2000);
            }

            return 0;
        },
        reinterpret_cast<LPVOID>(static_cast<ULONG_PTR>(delayMs)), 0, nullptr);

    if (thread) {
        CloseHandle(thread);
    } else {
        g_delayedInitializationScheduled = false;
        Wh_Log(L"Failed to create delayed initialization thread");
    }
}

void OnWindowCreated(HWND window,
                     HWND parent,
                     LPCWSTR className,
                     PCSTR functionName) {
    WCHAR createdClassName[64]{};
    if (parent &&
        GetClassName(window, createdClassName, ARRAYSIZE(createdClassName)) &&
        _wcsicmp(createdClassName,
                 L"Windows.UI.Composition.DesktopWindowContentBridge") == 0) {
        WCHAR parentClassName[64]{};
        if (GetClassName(parent, parentClassName, ARRAYSIZE(parentClassName)) &&
            _wcsicmp(parentClassName, L"Shell_TrayWnd") == 0) {
            Wh_Log(L"Taskbar XAML bridge created via %S", functionName);
            InitializeForCurrentThread();
            ScheduleDelayedInitialization(3000);
            return;
        }
    }

    BOOL textualClassName =
        ((reinterpret_cast<ULONG_PTR>(className) & ~static_cast<ULONG_PTR>(0xffff)) !=
         0);
    if (textualClassName &&
        (_wcsicmp(className, L"XamlExplorerHostIslandWindow") == 0 ||
         _wcsicmp(className, L"Shell_InputSwitchTopLevelWindow") == 0)) {
        Wh_Log(L"Taskbar XAML host created via %S", functionName);
        InitializeForCurrentThread();
        ScheduleDelayedInitialization(3000);
    }
}

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original = nullptr;

HWND WINAPI CreateWindowExW_Hook(DWORD exStyle,
                                 LPCWSTR className,
                                 LPCWSTR windowName,
                                 DWORD style,
                                 int x,
                                 int y,
                                 int width,
                                 int height,
                                 HWND parent,
                                 HMENU menu,
                                 HINSTANCE instance,
                                 PVOID parameter) {
    HWND window = CreateWindowExW_Original(exStyle, className, windowName, style,
                                           x, y, width, height, parent, menu,
                                           instance, parameter);
    if (window) {
        OnWindowCreated(window, parent, className, __FUNCTION__);
    }

    return window;
}

using CreateWindowInBand_t = HWND(WINAPI*)(DWORD exStyle,
                                           LPCWSTR className,
                                           LPCWSTR windowName,
                                           DWORD style,
                                           int x,
                                           int y,
                                           int width,
                                           int height,
                                           HWND parent,
                                           HMENU menu,
                                           HINSTANCE instance,
                                           PVOID parameter,
                                           DWORD band);
CreateWindowInBand_t CreateWindowInBand_Original = nullptr;

HWND WINAPI CreateWindowInBand_Hook(DWORD exStyle,
                                    LPCWSTR className,
                                    LPCWSTR windowName,
                                    DWORD style,
                                    int x,
                                    int y,
                                    int width,
                                    int height,
                                    HWND parent,
                                    HMENU menu,
                                    HINSTANCE instance,
                                    PVOID parameter,
                                    DWORD band) {
    HWND window = CreateWindowInBand_Original(
        exStyle, className, windowName, style, x, y, width, height, parent, menu,
        instance, parameter, band);
    if (window) {
        OnWindowCreated(window, parent, className, __FUNCTION__);
    }

    return window;
}

using CreateWindowInBandEx_t = HWND(WINAPI*)(DWORD exStyle,
                                             LPCWSTR className,
                                             LPCWSTR windowName,
                                             DWORD style,
                                             int x,
                                             int y,
                                             int width,
                                             int height,
                                             HWND parent,
                                             HMENU menu,
                                             HINSTANCE instance,
                                             PVOID parameter,
                                             DWORD band,
                                             DWORD typeFlags);
CreateWindowInBandEx_t CreateWindowInBandEx_Original = nullptr;

HWND WINAPI CreateWindowInBandEx_Hook(DWORD exStyle,
                                      LPCWSTR className,
                                      LPCWSTR windowName,
                                      DWORD style,
                                      int x,
                                      int y,
                                      int width,
                                      int height,
                                      HWND parent,
                                      HMENU menu,
                                      HINSTANCE instance,
                                      PVOID parameter,
                                      DWORD band,
                                      DWORD typeFlags) {
    HWND window = CreateWindowInBandEx_Original(
        exStyle, className, windowName, style, x, y, width, height, parent, menu,
        instance, parameter, band, typeFlags);
    if (window) {
        OnWindowCreated(window, parent, className, __FUNCTION__);
    }

    return window;
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR fileName,
                                   HANDLE file,
                                   DWORD flags) {
    HMODULE module = LoadLibraryExW_Original(fileName, file, flags);
    if (module && fileName) {
        PCWSTR baseName = wcsrchr(fileName, L'\\');
        baseName = baseName ? baseName + 1 : fileName;
        if (_wcsicmp(baseName, L"Windows.UI.Xaml.dll") == 0) {
            Wh_Log(L"Windows.UI.Xaml.dll loaded");
            ScheduleDelayedInitialization(3000);
        }
    }

    return module;
}

BOOL Wh_ModInit() {
    Wh_Log(L"TaskbarStats product hook init");
    g_uninitializing = false;
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"TaskbarStats after init");
    ScheduleDelayedInitialization(3000);
}

void Wh_ModUninit() {
    if (g_uninitializing.exchange(true)) {
        return;
    }

    Wh_Log(L"TaskbarStats uninit");

    if (g_win32ProofWindow) {
        DestroyWindow(g_win32ProofWindow);
        g_win32ProofWindow = nullptr;
    }

    if (g_visualTreeWatcher) {
        g_visualTreeWatcher->UnadviseVisualTreeChange();
        g_visualTreeWatcher = nullptr;
    }

    if (HWND taskbarUi = GetTaskbarUiWindow()) {
        RunFromWindowThread(taskbarUi,
                            [](PVOID) { UninitializeForCurrentThread(); },
                            nullptr);
    }

    for (HWND xamlHost : GetXamlHostWindows()) {
        RunFromWindowThread(xamlHost,
                            [](PVOID) { UninitializeForCurrentThread(); },
                            nullptr);
    }

    g_tapInitialized = false;
}

std::wstring GetShutdownEventName() {
    WCHAR name[128]{};
    swprintf_s(name, L"Local\\TaskbarStatsHookShutdown_%u", GetCurrentProcessId());
    return name;
}

DWORD WINAPI TaskbarStatsShutdownThread(LPVOID) {
    std::wstring eventName = GetShutdownEventName();
    HANDLE event = CreateEvent(nullptr, TRUE, FALSE, eventName.c_str());
    if (!event) {
        Wh_Log(L"CreateEvent failed for shutdown event: %u", GetLastError());
        return 0;
    }

    Wh_Log(L"Shutdown event ready: %s", eventName.c_str());
    WaitForSingleObject(event, INFINITE);
    CloseHandle(event);

    Wh_Log(L"Shutdown event signaled");
    Wh_ModUninit();
    if (g_hookModule) {
        FreeLibraryAndExitThread(g_hookModule, 0);
    }

    return 0;
}

DWORD WINAPI TaskbarStatsBootstrapThread(LPVOID) {
    Wh_ModInit();
    Wh_ModAfterInit();

    HANDLE shutdownThread = CreateThread(nullptr, 0, TaskbarStatsShutdownThread,
                                         nullptr, 0, nullptr);
    if (shutdownThread) {
        CloseHandle(shutdownThread);
    }

    while (!g_uninitializing) {
        InitializeExistingTaskbarThreads();
        InitializeTapOnce();
        Sleep(5000);
    }

    return 0;
}

extern "C" __declspec(dllexport) DWORD __stdcall TaskbarStatsHookVersion() {
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hookModule = reinterpret_cast<HMODULE>(instance);
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, TaskbarStatsBootstrapThread,
                                     nullptr, 0, nullptr);
        if (thread) {
            CloseHandle(thread);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!g_uninitializing) {
            Wh_ModUninit();
        }
    }

    return TRUE;
}
