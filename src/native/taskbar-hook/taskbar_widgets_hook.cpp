// Taskbar Widgets Explorer hook. This DLL is injected only by TaskbarWidgets.exe.
// It uses private Windows 11 XAML surfaces and must always fail closed.

// Windows SDK declares these COM server exports as imported functions. Rename
// only those declarations while headers are parsed so this DLL can provide its
// own exported implementations below.
#define DllGetClassObject TaskbarWidgetsSdkDllGetClassObject
#define DllCanUnloadNow TaskbarWidgetsSdkDllCanUnloadNow
#include <windows.h>
#include <windowsx.h>
#include <gdiplus.h>
#include <ocidl.h>
#include <xamlom.h>
#undef DllGetClassObject
#undef DllCanUnloadNow

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include "generated/widget_catalog.g.h"
#include "layout_math.h"
#include "../common/json_string.h"
#include "widget_renderer.h"

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
namespace wuxs = winrt::Windows::UI::Xaml::Shapes;
namespace gdi = Gdiplus;

std::atomic_bool g_tapInitialized = false;
std::atomic_bool g_delayedInitializationScheduled = false;
std::atomic_bool g_uninitializing = false;
std::atomic_uint g_commandSequence = 0;
thread_local bool g_initializedForThread = false;
bool g_inInjectTaskbarWidgetsTap = false;
constexpr PCWSTR kTaskbarWidgetsLayoutMarkerName =
    L"TaskbarWidgetsLayoutV20260714PercentMove";
constexpr double kTaskbarWidgetsExplicitColumnRightGap = 10.0;
constexpr double kTaskbarWidgetsOverlayTrayGap = 28.0;
constexpr double kTaskbarWidgetsOverlayMinRightReserve = 220.0;
constexpr double kTaskbarWidgetsMaxWidgetWidth = 240.0;
HWND g_win32ProofWindow = nullptr;
HWND g_win32ProofParent = nullptr;
HMODULE g_hookModule = nullptr;
ULONG_PTR g_gdiplusToken = 0;

HMODULE GetCurrentModuleHandle();
std::wstring GetTaskbarWidgetsRootPath();
wuxm::Brush MakeMediaGradientBrush(const winrt::Windows::UI::Color& left,
                                   const winrt::Windows::UI::Color& right);

std::wstring ParentDirectory(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return {};
    }

    return path.substr(0, slash);
}

std::wstring FileNameFromPath(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring GetFallbackTaskbarWidgetsRootPath() {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\Programs\\TaskbarWidgets\\Data";
    return path;
}

std::wstring GetTaskbarWidgetsInstallPath() {
    std::wstring dataPath = GetTaskbarWidgetsRootPath();
    if (_wcsicmp(FileNameFromPath(dataPath).c_str(), L"Data") == 0) {
        return ParentDirectory(dataPath);
    }
    return dataPath;
}

std::wstring GetTaskbarWidgetsRootPath() {
    HMODULE module = g_hookModule ? g_hookModule : GetCurrentModuleHandle();
    if (module) {
        WCHAR modulePath[MAX_PATH]{};
        DWORD length = GetModuleFileName(module, modulePath, ARRAYSIZE(modulePath));
        if (length > 0 && length < ARRAYSIZE(modulePath)) {
            std::wstring hookDirectory = ParentDirectory(modulePath);
            std::wstring runtimeHashDirectory = ParentDirectory(hookDirectory);
            std::wstring runtimeDirectory = ParentDirectory(runtimeHashDirectory);
            if (!runtimeDirectory.empty() &&
                _wcsicmp(FileNameFromPath(runtimeHashDirectory).c_str(),
                         L"Runtime") == 0) {
                return runtimeDirectory;
            }

            if (!hookDirectory.empty()) {
                return hookDirectory;
            }
        }
    }

    return GetFallbackTaskbarWidgetsRootPath();
}

std::wstring GetTaskbarWidgetsLogPath(PCWSTR leaf) {
    std::wstring path = GetTaskbarWidgetsRootPath();
    if (path.empty()) {
        return {};
    }

    CreateDirectory(path.c_str(), nullptr);
    path += L"\\Logs";
    CreateDirectory(path.c_str(), nullptr);
    path += L"\\";
    path += leaf;
    return path;
}

void Wh_Log(PCWSTR format, ...) {
    std::wstring path = GetTaskbarWidgetsLogPath(L"hook.log");
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

std::wstring GetTaskbarWidgetsPath(PCWSTR leaf = nullptr) {
    std::wstring path = GetTaskbarWidgetsRootPath();
    if (path.empty()) {
        return {};
    }

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

void WriteTaskbarWidgetsCommand(const std::wstring& command,
                              const std::wstring& accountId = L"",
                              const std::wstring& widgetId = L"",
                              std::optional<int> positionPct = std::nullopt,
                              std::optional<int> offsetPx = std::nullopt) {
    std::wstring directory = GetTaskbarWidgetsPath(L"Commands");
    if (directory.empty()) {
        return;
    }

    CreateDirectory(directory.c_str(), nullptr);

    SYSTEMTIME time{};
    GetSystemTime(&time);
    WCHAR fileName[128]{};
    swprintf_s(fileName, L"\\%04u%02u%02u%02u%02u%02u%03u_%u_%u.json",
               time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
               time.wSecond, time.wMilliseconds, GetCurrentProcessId(),
               ++g_commandSequence);

    std::wstring path = directory + fileName;
    std::wstring temporaryPath = path + L".tmp";
    long long createdAt = static_cast<long long>(std::time(nullptr));
    std::string json = "{\"schemaVersion\":1,\"commandId\":\"" +
                       WideToUtf8(std::to_wstring(createdAt)) + "-" +
                       WideToUtf8(std::to_wstring(GetCurrentProcessId())) +
                       "\",\"action\":\"" + JsonEscapeUtf8(command) +
                       "\",\"createdAtUnix\":" + std::to_string(createdAt);
    if (!accountId.empty()) {
        json += ",\"accountId\":\"" + JsonEscapeUtf8(accountId) + "\"";
    }
    if (!widgetId.empty()) {
        json += ",\"widgetId\":\"" + JsonEscapeUtf8(widgetId) + "\"";
    }
    if (positionPct.has_value()) {
        json += ",\"positionPct\":" + std::to_string(*positionPct);
    }
    if (offsetPx.has_value()) {
        json += ",\"offsetPx\":" + std::to_string(*offsetPx);
    }
    json += "}\n";

    HANDLE file = CreateFile(temporaryPath.c_str(), GENERIC_WRITE, 0,
                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wh_Log(L"Failed to create command file: %u", GetLastError());
        return;
    }

    DWORD written = 0;
    WriteFile(file, json.data(), static_cast<DWORD>(json.size()), &written,
              nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
    if (!MoveFileEx(temporaryPath.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        Wh_Log(L"Failed to publish command file: %u", GetLastError());
        DeleteFile(temporaryPath.c_str());
    }
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
    wux::FrameworkElement trayElement{nullptr};
    wux::UIElement root{nullptr};
    wux::DispatcherTimer timer{nullptr};
    uint32_t insertedColumn{};
    bool insertedGridColumn{};
};

thread_local std::vector<InsertedModule> g_insertedModules;

std::wstring GetAssetsFolder() {
    std::wstring path = GetTaskbarWidgetsInstallPath();
    if (path.empty()) {
        return {};
    }

    path += L"\\Assets\\";
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

unsigned long long GetFileWriteVersion(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesEx(path.c_str(), GetFileExInfoStandard, &data)) {
        return 0;
    }

    ULARGE_INTEGER value{};
    value.LowPart = data.ftLastWriteTime.dwLowDateTime;
    value.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return value.QuadPart;
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
std::wstring ReadActiveWidgetDesign();
wux::UIElement MakePngImage(PCWSTR assetName, double width, double height);
long long CurrentUnixTime();
long long CurrentUnixMillis();
bool ExtractJsonInt64(const std::string& json, const char* key, long long& value);
void SetExpandedMode(wux::UIElement const& root, bool expanded);
void ShowCodexHoverPopup();
void HideCodexHoverPopup();
double WidgetDesignWidth(const std::wstring& designId);
void ShowAccountMenu(wux::FrameworkElement const& root);
void ShowWeatherMenu(wux::FrameworkElement const& root);
HWND FindCurrentProcessTaskbarWindow();
void UpdateTaskbarWidgetsRoot(wux::UIElement const& root);
void ShowWidgetLibraryWindow();
void RefreshInsertedTaskbarWidgetsRoots();
void ApplyTaskbarWidgetsAnchorMargin(wux::FrameworkElement const& root,
                                   wuxc::Grid const& parent,
                                   wux::FrameworkElement const& trayElement);

struct CodexAccountInfo {
    std::wstring id;
    std::wstring label;
    std::wstring email;
    std::wstring rateLimitText;
    bool active{};
};

struct AccountMenuHitItem {
    RECT rect{};
    std::wstring command;
    std::wstring accountId;
};

struct WidgetLibraryHitItem {
    RECT rect{};
    std::wstring command;
    std::wstring designId;
};

struct WidgetInstanceRuntime {
    std::wstring id;
    std::wstring designId;
    bool enabled = true;
    long long moveX = 0;
    long long positionPct = -1;
    long long order = 0;
};

struct WidgetDragState {
    bool pressed{};
    bool dragging{};
    bool suppressTap{};
    double startPointerX{};
    double startLeft{};
};

struct WidgetDragOverride {
    double left{};
    int anchorPercent{};
    int offsetPx{};
    bool awaitingConfig{};
    std::chrono::steady_clock::time_point expiresAt{};
};

thread_local std::unordered_map<uintptr_t, WidgetDragOverride> g_widgetDragOverrides;

uintptr_t WidgetElementKey(wux::UIElement const& element) noexcept {
    return reinterpret_cast<uintptr_t>(winrt::get_abi(element));
}

HWND g_accountMenuWindow = nullptr;
HWND g_codexHoverWindow = nullptr;
std::vector<std::wstring> g_codexHoverTitles;
std::vector<CodexAccountInfo> g_accountMenuAccounts;
std::vector<AccountMenuHitItem> g_accountMenuHitItems;
int g_accountMenuHoveredIndex = -1;
HWND g_widgetLibraryWindow = nullptr;
std::vector<WidgetLibraryHitItem> g_widgetLibraryHitItems;
int g_widgetLibraryHoveredIndex = -1;
HWND g_weatherMenuWindow = nullptr;
int g_weatherMenuHoveredIndex = -1;

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
    row.Height(14);

    auto title = MakeNamedText(titleName, L"", 9, 0xF0);
    title.Width(166);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    title.VerticalAlignment(wux::VerticalAlignment::Center);

    auto icon = MakeNamedStateIcon(iconName);

    auto stateText = MakeNamedText(stateName, L"", 8, 0xCC);
    stateText.Width(32);
    stateText.HorizontalAlignment(wux::HorizontalAlignment::Left);
    stateText.TextAlignment(wux::TextAlignment::Left);
    stateText.VerticalAlignment(wux::VerticalAlignment::Center);
    stateText.Foreground(MakeBrush(0xCC, 0x94, 0xA3, 0xB8));

    row.Children().Append(title.as<wux::UIElement>());
    row.Children().Append(icon.as<wux::UIElement>());
    row.Children().Append(stateText.as<wux::UIElement>());
    return row;
}

wux::FrameworkElement MakeWeatherPanel() {
    wuxc::Border weather;
    weather.Name(L"TaskbarWidgetsWeatherPanel");
    weather.Height(36);
    weather.Width(228);
    weather.Visibility(wux::Visibility::Collapsed);
    weather.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(8));
    weather.Background(MakeBrush(0xF5, 0x11, 0x11, 0x11));
    weather.BorderBrush(MakeBrush(0xFF, 0x2D, 0x2D, 0x2D));
    weather.BorderThickness(wux::ThicknessHelper::FromUniformLength(1));
    weather.Padding(wux::ThicknessHelper::FromLengths(10, 1, 6, 1));

    wuxc::Grid layout;
    layout.Width(212);
    layout.Height(34);

    wuxc::ColumnDefinition textColumn;
    textColumn.Width(wux::GridLengthHelper::FromPixels(114));
    wuxc::ColumnDefinition tempColumn;
    tempColumn.Width(wux::GridLengthHelper::FromPixels(46));
    wuxc::ColumnDefinition iconColumn;
    iconColumn.Width(wux::GridLengthHelper::FromPixels(52));
    layout.ColumnDefinitions().Append(textColumn);
    layout.ColumnDefinitions().Append(tempColumn);
    layout.ColumnDefinitions().Append(iconColumn);

    wuxc::Grid textBlock;
    textBlock.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::RowDefinition cityRow;
    cityRow.Height(wux::GridLengthHelper::FromPixels(17));
    wuxc::RowDefinition timeRow;
    timeRow.Height(wux::GridLengthHelper::FromPixels(13));
    textBlock.RowDefinitions().Append(cityRow);
    textBlock.RowDefinitions().Append(timeRow);

    auto city = MakeNamedText(L"TaskbarWidgetsWeatherCity", L"Izmir", 11, 0xFF);
    city.Width(112);
    city.HorizontalAlignment(wux::HorizontalAlignment::Left);
    city.TextAlignment(wux::TextAlignment::Left);
    city.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    city.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

    auto condition =
        MakeNamedText(L"TaskbarWidgetsWeatherCondition", L"--:-- • --/--", 9, 0xB8);
    condition.Width(112);
    condition.HorizontalAlignment(wux::HorizontalAlignment::Left);
    condition.TextAlignment(wux::TextAlignment::Left);
    condition.TextTrimming(wux::TextTrimming::CharacterEllipsis);

    wuxc::Grid::SetRow(city, 0);
    wuxc::Grid::SetRow(condition, 1);
    textBlock.Children().Append(city.as<wux::UIElement>());
    textBlock.Children().Append(condition.as<wux::UIElement>());
    wuxc::Grid::SetColumn(textBlock, 0);

    auto temp = MakeNamedText(L"TaskbarWidgetsWeatherTemp", L"--\x00B0", 21, 0xFF);
    temp.Width(44);
    temp.VerticalAlignment(wux::VerticalAlignment::Center);
    temp.HorizontalAlignment(wux::HorizontalAlignment::Right);
    temp.TextAlignment(wux::TextAlignment::Right);
    wuxc::Grid::SetColumn(temp, 1);

    auto icon = MakePngImage(L"weather\\rain.png", 46, 34);
    SetElementName(icon.as<wux::FrameworkElement>(), L"TaskbarWidgetsWeatherIcon");
    wuxc::Grid::SetColumn(icon.as<wux::FrameworkElement>(), 2);

    layout.Children().Append(textBlock.as<wux::UIElement>());
    layout.Children().Append(temp.as<wux::UIElement>());
    layout.Children().Append(icon);
    weather.Child(layout);
    return weather;
}

wux::FrameworkElement MakeDiscordPanel() {
    wuxc::Border discord;
    discord.Name(L"TaskbarWidgetsDiscordPanel");
    discord.Height(36);
    discord.Width(184);
    discord.Visibility(wux::Visibility::Collapsed);
    discord.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(8));
    discord.Background(MakeBrush(0xF5, 0x11, 0x11, 0x11));
    discord.BorderBrush(MakeBrush(0xFF, 0x2D, 0x2D, 0x2D));
    discord.BorderThickness(wux::ThicknessHelper::FromUniformLength(1));
    discord.Padding(wux::ThicknessHelper::FromLengths(8, 3, 8, 3));

    wuxc::StackPanel row;
    row.Orientation(wuxc::Orientation::Horizontal);
    row.VerticalAlignment(wux::VerticalAlignment::Center);
    row.HorizontalAlignment(wux::HorizontalAlignment::Center);

    for (int i = 0; i < 5; ++i) {
        std::wstring frameName = L"TaskbarWidgetsDiscordAvatarFrame" + std::to_wstring(i);
        std::wstring avatarName = L"TaskbarWidgetsDiscordAvatarEllipse" + std::to_wstring(i);

        wuxc::Border frame;
        frame.Name(frameName);
        frame.Width(32);
        frame.Height(32);
        frame.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(16));
        frame.BorderThickness(wux::ThicknessHelper::FromUniformLength(2));
        frame.BorderBrush(MakeBrush(0x00, 0x00, 0x00, 0x00));
        frame.Background(MakeBrush(0x00, 0x00, 0x00, 0x00));
        frame.Margin(wux::ThicknessHelper::FromLengths(2, 0, 2, 0));
        frame.Visibility(wux::Visibility::Collapsed);
        frame.Padding(wux::ThicknessHelper::FromUniformLength(1));

        wuxs::Ellipse avatar;
        avatar.Name(avatarName);
        avatar.Width(24);
        avatar.Height(24);
        avatar.HorizontalAlignment(wux::HorizontalAlignment::Center);
        avatar.VerticalAlignment(wux::VerticalAlignment::Center);
        avatar.Opacity(0.38);
        avatar.Fill(MakeBrush(0xFF, 0x1F, 0x24, 0x2D));
        frame.Child(avatar);
        row.Children().Append(frame.as<wux::UIElement>());
    }

    discord.Child(row);
    return discord;
}

wux::FrameworkElement MakeMediaPanel() {
    wuxc::Border panel;
    panel.Name(L"TaskbarWidgetsMediaPanel");
    panel.Width(220);
    panel.Height(44);
    panel.Visibility(wux::Visibility::Collapsed);
    panel.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    panel.Background(MakeBrush(0xFF, 0xFA, 0xFA, 0xF8));
    panel.Padding(wux::ThicknessHelper::FromUniformLength(0));

    wuxc::Grid layout;
    layout.Width(220);
    layout.Height(44);

    wuxc::Grid content;
    content.Width(197);
    content.Height(30);
    content.HorizontalAlignment(wux::HorizontalAlignment::Left);
    content.VerticalAlignment(wux::VerticalAlignment::Center);
    content.Margin(wux::ThicknessHelper::FromLengths(12, 0, 11, 0));

    wuxc::ColumnDefinition coverColumn;
    coverColumn.Width(wux::GridLengthHelper::FromPixels(49));
    wuxc::ColumnDefinition textColumn;
    textColumn.Width(wux::GridLengthHelper::FromPixels(126));
    wuxc::ColumnDefinition buttonColumn;
    buttonColumn.Width(wux::GridLengthHelper::FromPixels(22));
    content.ColumnDefinitions().Append(coverColumn);
    content.ColumnDefinitions().Append(textColumn);
    content.ColumnDefinitions().Append(buttonColumn);

    wuxc::Border cover;
    cover.Name(L"TaskbarWidgetsMediaCover");
    cover.Width(49);
    cover.Height(30);
    cover.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(4));
    cover.HorizontalAlignment(wux::HorizontalAlignment::Left);
    cover.VerticalAlignment(wux::VerticalAlignment::Center);
    cover.Background(MakeBrush(0xFF, 0x17, 0x1B, 0x24));
    std::wstring coverPath = GetAssetsFolder() + L"widgets\\media_cover.png";
    if (FileExists(coverPath)) {
        wuxmi::BitmapImage source;
        source.UriSource(wf::Uri(ToFileUri(coverPath)));
        wuxm::ImageBrush brush;
        brush.ImageSource(source);
        brush.Stretch(wuxm::Stretch::UniformToFill);
        cover.Background(brush);
    }
    wuxc::Grid::SetColumn(cover, 0);

    wuxc::Grid textGrid;
    textGrid.Width(112);
    textGrid.Height(30);
    textGrid.Margin(wux::ThicknessHelper::FromLengths(12, 0, 0, 0));
    textGrid.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::RowDefinition titleRow;
    titleRow.Height(wux::GridLengthHelper::FromPixels(16));
    wuxc::RowDefinition artistRow;
    artistRow.Height(wux::GridLengthHelper::FromPixels(14));
    textGrid.RowDefinitions().Append(titleRow);
    textGrid.RowDefinitions().Append(artistRow);
    wuxc::Grid::SetColumn(textGrid, 1);

    wuxc::Grid titleViewport;
    titleViewport.Width(112);
    titleViewport.Height(16);
    wuxm::RectangleGeometry titleClip;
    titleClip.Rect(wf::Rect{0, 0, 112, 16});
    titleViewport.Clip(titleClip);
    wuxc::Grid::SetRow(titleViewport, 0);

    wuxc::StackPanel titleMarquee;
    titleMarquee.Name(L"TaskbarWidgetsMediaTitleMarquee");
    titleMarquee.Orientation(wuxc::Orientation::Horizontal);
    titleMarquee.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleMarquee.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxm::TranslateTransform titleTransform;
    titleMarquee.RenderTransform(titleTransform);

    auto title = MakeNamedText(L"TaskbarWidgetsMediaTitle", L"No media", 11, 0xFF);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.Foreground(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.Width(112);

    auto titleClone = MakeNamedText(L"TaskbarWidgetsMediaTitleClone", L"", 11, 0xFF);
    titleClone.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleClone.TextAlignment(wux::TextAlignment::Left);
    titleClone.Foreground(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    titleClone.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    titleClone.Width(0);

    titleMarquee.Children().Append(title.as<wux::UIElement>());
    titleMarquee.Children().Append(titleClone.as<wux::UIElement>());
    titleViewport.Children().Append(titleMarquee.as<wux::UIElement>());

    auto artist = MakeNamedText(L"TaskbarWidgetsMediaArtist", L"Open a player", 9, 0xF0);
    artist.HorizontalAlignment(wux::HorizontalAlignment::Left);
    artist.TextAlignment(wux::TextAlignment::Left);
    artist.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    artist.Foreground(MakeBrush(0xF0, 0x00, 0x00, 0x00));
    artist.Width(112);
    wuxc::Grid::SetRow(artist, 1);

    textGrid.Children().Append(titleViewport.as<wux::UIElement>());
    textGrid.Children().Append(artist.as<wux::UIElement>());

    wuxc::Border playButton;
    playButton.Name(L"TaskbarWidgetsMediaPlayButton");
    playButton.Width(18);
    playButton.Height(18);
    playButton.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    playButton.HorizontalAlignment(wux::HorizontalAlignment::Right);
    playButton.VerticalAlignment(wux::VerticalAlignment::Center);
    playButton.Background(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    wuxc::Grid::SetColumn(playButton, 2);

    wuxc::FontIcon playIcon;
    playIcon.Name(L"TaskbarWidgetsMediaPlayIcon");
    playIcon.Glyph(L"\xE768");
    playIcon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    playIcon.FontSize(9);
    playIcon.Width(18);
    playIcon.Height(18);
    playIcon.HorizontalAlignment(wux::HorizontalAlignment::Center);
    playIcon.VerticalAlignment(wux::VerticalAlignment::Center);
    playIcon.Foreground(MakeBrush(0xFF, 0xFF, 0xFF, 0xFF));
    playButton.Child(playIcon);

    content.Children().Append(cover.as<wux::UIElement>());
    content.Children().Append(textGrid.as<wux::UIElement>());
    content.Children().Append(playButton.as<wux::UIElement>());
    layout.Children().Append(content.as<wux::UIElement>());

    panel.Child(layout);
    return panel;
}

wux::FrameworkElement MakeSteamDownloadPanel() {
    wuxc::Border panel;
    panel.Name(L"TaskbarWidgetsSteamPanel");
    panel.Width(220);
    panel.Height(44);
    panel.Visibility(wux::Visibility::Collapsed);
    panel.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    panel.Background(MakeBrush(0xFF, 0x0B, 0x12, 0x20));
    panel.Padding(wux::ThicknessHelper::FromUniformLength(0));

    wuxc::Grid layout;
    layout.Width(220);
    layout.Height(44);

    wuxc::Border backdrop;
    backdrop.Name(L"TaskbarWidgetsSteamBackdrop");
    backdrop.Width(220);
    backdrop.Height(44);
    backdrop.Opacity(0.42);
    backdrop.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    backdrop.Background(MakeBrush(0xFF, 0x0B, 0x12, 0x20));

    wuxc::Border shade;
    shade.Width(220);
    shade.Height(44);
    shade.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    shade.Background(MakeMediaGradientBrush(
        winrt::Windows::UI::Color{0xEC, 0x08, 0x13, 0x23},
        winrt::Windows::UI::Color{0xD8, 0x1B, 0x28, 0x38}));

    wuxc::Grid content;
    content.Width(197);
    content.Height(32);
    content.HorizontalAlignment(wux::HorizontalAlignment::Left);
    content.VerticalAlignment(wux::VerticalAlignment::Center);
    content.Margin(wux::ThicknessHelper::FromLengths(12, 0, 11, 0));

    wuxc::ColumnDefinition coverColumn;
    coverColumn.Width(wux::GridLengthHelper::FromPixels(49));
    wuxc::ColumnDefinition textColumn;
    textColumn.Width(wux::GridLengthHelper::FromPixels(104));
    wuxc::ColumnDefinition metricColumn;
    metricColumn.Width(wux::GridLengthHelper::FromPixels(44));
    content.ColumnDefinitions().Append(coverColumn);
    content.ColumnDefinitions().Append(textColumn);
    content.ColumnDefinitions().Append(metricColumn);

    wuxc::Border cover;
    cover.Name(L"TaskbarWidgetsSteamCover");
    cover.Width(49);
    cover.Height(30);
    cover.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(4));
    cover.HorizontalAlignment(wux::HorizontalAlignment::Left);
    cover.VerticalAlignment(wux::VerticalAlignment::Center);
    cover.Background(MakeMediaGradientBrush(
        winrt::Windows::UI::Color{0xFF, 0x1B, 0x28, 0x38},
        winrt::Windows::UI::Color{0xFF, 0x2A, 0x47, 0x5E}));
    wuxc::Grid::SetColumn(cover, 0);

    wuxc::Grid textGrid;
    textGrid.Width(93);
    textGrid.Height(30);
    textGrid.Margin(wux::ThicknessHelper::FromLengths(12, 0, 0, 0));
    textGrid.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::RowDefinition titleRow;
    titleRow.Height(wux::GridLengthHelper::FromPixels(16));
    wuxc::RowDefinition detailRow;
    detailRow.Height(wux::GridLengthHelper::FromPixels(14));
    textGrid.RowDefinitions().Append(titleRow);
    textGrid.RowDefinitions().Append(detailRow);
    wuxc::Grid::SetColumn(textGrid, 1);

    wuxc::Grid titleViewport;
    titleViewport.Width(93);
    titleViewport.Height(16);
    wuxm::RectangleGeometry titleClip;
    titleClip.Rect(wf::Rect{0, 0, 93, 16});
    titleViewport.Clip(titleClip);
    wuxc::Grid::SetRow(titleViewport, 0);

    wuxc::StackPanel titleMarquee;
    titleMarquee.Name(L"TaskbarWidgetsSteamTitleMarquee");
    titleMarquee.Orientation(wuxc::Orientation::Horizontal);
    titleMarquee.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleMarquee.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxm::TranslateTransform titleTransform;
    titleMarquee.RenderTransform(titleTransform);

    auto title = MakeNamedText(L"TaskbarWidgetsSteamTitle", L"Steam", 11, 0xFF);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.Width(93);

    auto titleClone = MakeNamedText(L"TaskbarWidgetsSteamTitleClone", L"", 11, 0xFF);
    titleClone.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleClone.TextAlignment(wux::TextAlignment::Left);
    titleClone.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    titleClone.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    titleClone.Width(0);

    titleMarquee.Children().Append(title.as<wux::UIElement>());
    titleMarquee.Children().Append(titleClone.as<wux::UIElement>());
    titleViewport.Children().Append(titleMarquee.as<wux::UIElement>());

    auto detail = MakeNamedText(L"TaskbarWidgetsSteamDetail", L"Indirme yok", 9, 0xF0);
    detail.HorizontalAlignment(wux::HorizontalAlignment::Left);
    detail.TextAlignment(wux::TextAlignment::Left);
    detail.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    detail.Foreground(MakeBrush(0xF0, 0xCB, 0xD5, 0xE1));
    detail.Width(93);
    wuxc::Grid::SetRow(detail, 1);

    textGrid.Children().Append(titleViewport.as<wux::UIElement>());
    textGrid.Children().Append(detail.as<wux::UIElement>());

    wuxc::Grid metricGrid;
    metricGrid.Width(44);
    metricGrid.Height(30);
    metricGrid.VerticalAlignment(wux::VerticalAlignment::Center);
    metricGrid.HorizontalAlignment(wux::HorizontalAlignment::Right);
    wuxc::RowDefinition metricTextRow;
    metricTextRow.Height(wux::GridLengthHelper::FromPixels(17));
    wuxc::RowDefinition metricBarRow;
    metricBarRow.Height(wux::GridLengthHelper::FromPixels(13));
    metricGrid.RowDefinitions().Append(metricTextRow);
    metricGrid.RowDefinitions().Append(metricBarRow);
    wuxc::Grid::SetColumn(metricGrid, 2);

    auto metric = MakeNamedText(L"TaskbarWidgetsSteamMetric", L"--", 11, 0xFF);
    metric.HorizontalAlignment(wux::HorizontalAlignment::Right);
    metric.TextAlignment(wux::TextAlignment::Right);
    metric.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    metric.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    metric.Width(44);
    wuxc::Grid::SetRow(metric, 0);

    wuxc::Grid progressTrack;
    progressTrack.Name(L"TaskbarWidgetsSteamProgressTrack");
    progressTrack.Width(38);
    progressTrack.Height(3);
    progressTrack.Margin(wux::ThicknessHelper::FromLengths(0, 5, 0, 0));
    progressTrack.HorizontalAlignment(wux::HorizontalAlignment::Right);
    progressTrack.VerticalAlignment(wux::VerticalAlignment::Top);
    progressTrack.Background(MakeBrush(0x48, 0xCB, 0xD5, 0xE1));
    wuxc::Grid::SetRow(progressTrack, 1);

    wuxc::Border progressFill;
    progressFill.Name(L"TaskbarWidgetsSteamProgressFill");
    progressFill.Width(0);
    progressFill.Height(3);
    progressFill.HorizontalAlignment(wux::HorizontalAlignment::Left);
    progressFill.Background(MakeBrush(0xFF, 0x66, 0xC0, 0xF4));
    progressTrack.Children().Append(progressFill.as<wux::UIElement>());

    metricGrid.Children().Append(metric.as<wux::UIElement>());
    metricGrid.Children().Append(progressTrack.as<wux::UIElement>());

    content.Children().Append(cover.as<wux::UIElement>());
    content.Children().Append(textGrid.as<wux::UIElement>());
    content.Children().Append(metricGrid.as<wux::UIElement>());
    layout.Children().Append(backdrop.as<wux::UIElement>());
    layout.Children().Append(shade.as<wux::UIElement>());
    layout.Children().Append(content.as<wux::UIElement>());

    panel.Child(layout);
    return panel;
}

wux::FrameworkElement MakeSystemMetricPanel() {
    wuxc::Border panel;
    panel.Name(L"TaskbarWidgetsSystemPanel");
    panel.Width(32);
    panel.Height(24);
    panel.Visibility(wux::Visibility::Collapsed);
    panel.Background(MakeBrush(0x01, 0x00, 0x00, 0x00));

    // XMeters renders directly on the taskbar: no card, title, backdrop or
    // rounded container. The contents are rebuilt only when the sampled value
    // changes, keeping Explorer's 33 ms refresh path lightweight.
    wuxc::StackPanel meter;
    meter.Name(L"TaskbarWidgetsSystemMeter");
    meter.Width(32);
    meter.Height(24);
    meter.Orientation(wuxc::Orientation::Horizontal);
    meter.Spacing(3);
    meter.HorizontalAlignment(wux::HorizontalAlignment::Center);
    meter.VerticalAlignment(wux::VerticalAlignment::Center);
    panel.Child(meter);
    return panel;
}

void ShowWidgetContextMenu() {
    constexpr UINT_PTR kSettingsCommand = 1001;
    constexpr UINT_PTR kQuitCommand = 1002;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit TaskbarWidgets");

    POINT point{};
    GetCursorPos(&point);
    HWND owner = GetForegroundWindow();
    if (!owner) {
        owner = GetDesktopWindow();
    }

    SetForegroundWindow(owner);
    UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        point.x,
        point.y,
        0,
        owner,
        nullptr);
    DestroyMenu(menu);

    if (command == kSettingsCommand) {
        WriteTaskbarWidgetsCommand(L"openSettings");
    } else if (command == kQuitCommand) {
        WriteTaskbarWidgetsCommand(L"quit");
    }
}

std::wstring GetWidgetDesignFromRoot(wux::UIElement const& root) {
    auto frameworkElement = root.try_as<wux::FrameworkElement>();
    if (!frameworkElement) {
        return ReadActiveWidgetDesign();
    }

    auto tag = frameworkElement.Tag();
    if (!tag) {
        return ReadActiveWidgetDesign();
    }

    auto designId = winrt::unbox_value_or<winrt::hstring>(tag, L"");
    if (designId.empty()) {
        return ReadActiveWidgetDesign();
    }

    return std::wstring(designId.c_str());
}

wux::FrameworkElement MakeTaskbarWidgetsWidgetRoot(const WidgetInstanceRuntime& instance) {
    wuxc::Grid root;
    root.Name(L"TaskbarWidgetsWidget");
    root.Tag(winrt::box_value(winrt::hstring(instance.designId)));
    root.VerticalAlignment(wux::VerticalAlignment::Center);
    root.HorizontalAlignment(wux::HorizontalAlignment::Right);
    root.Height(36);
    root.Width(184);
    root.Margin(wux::ThicknessHelper::FromLengths(6, 0, 6, 0));
    // WinUI can optimize a fully transparent brush out of hit testing. A
    // practically invisible alpha keeps the entire declared widget rectangle
    // interactive instead of limiting hover to painted text and progress bars.
    root.IsHitTestVisible(true);
    root.Background(MakeBrush(0x01, 0x00, 0x00, 0x00));

    wuxc::Grid compact;
    compact.Name(L"TaskbarWidgetsCompactPanel");
    compact.Height(36);
    compact.Width(184);

    wuxc::RowDefinition titleRow;
    titleRow.Height(wux::GridLengthHelper::FromPixels(14));
    wuxc::RowDefinition lineRow;
    lineRow.Height(wux::GridLengthHelper::FromPixels(2));
    wuxc::RowDefinition metricsRow;
    metricsRow.Height(wux::GridLengthHelper::FromPixels(17));
    compact.RowDefinitions().Append(titleRow);
    compact.RowDefinitions().Append(lineRow);
    compact.RowDefinitions().Append(metricsRow);

    wuxc::StackPanel titleLine;
    titleLine.Orientation(wuxc::Orientation::Horizontal);
    titleLine.HorizontalAlignment(wux::HorizontalAlignment::Center);
    titleLine.VerticalAlignment(wux::VerticalAlignment::Center);

    auto title = MakeNamedText(L"TaskbarWidgetsTitle", L"Antigravity", 10, 0xF0);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    title.Width(104);

    auto stateIcon = MakeNamedStateIcon(L"TaskbarWidgetsStateIcon");

    auto stateText = MakeNamedText(L"TaskbarWidgetsStateText", L"IDLE", 8, 0xCC);
    stateText.Width(26);
    stateText.HorizontalAlignment(wux::HorizontalAlignment::Left);
    stateText.TextAlignment(wux::TextAlignment::Left);
    stateText.VerticalAlignment(wux::VerticalAlignment::Center);
    stateText.Foreground(MakeBrush(0xCC, 0x94, 0xA3, 0xB8));

    titleLine.Children().Append(title.as<wux::UIElement>());
    titleLine.Children().Append(stateIcon.as<wux::UIElement>());
    titleLine.Children().Append(stateText.as<wux::UIElement>());

    wuxc::Grid limitBar;
    limitBar.Name(L"TaskbarWidgetsLimitBarTrack");
    limitBar.Height(2);
    limitBar.Width(126);
    limitBar.HorizontalAlignment(wux::HorizontalAlignment::Center);
    limitBar.Background(MakeBrush(0x34, 0x94, 0xA3, 0xB8));

    wuxc::Border limitBarFill;
    limitBarFill.Name(L"TaskbarWidgetsLimitBarFill");
    limitBarFill.Height(2);
    limitBarFill.Width(0);
    limitBarFill.HorizontalAlignment(wux::HorizontalAlignment::Left);
    limitBarFill.Background(MakeBrush(0xFF, 0x94, 0xA3, 0xB8));
    limitBar.Children().Append(limitBarFill.as<wux::UIElement>());

    wuxc::StackPanel metrics;
    metrics.Orientation(wuxc::Orientation::Horizontal);
    metrics.HorizontalAlignment(wux::HorizontalAlignment::Center);
    metrics.VerticalAlignment(wux::VerticalAlignment::Center);
    metrics.Children().Append(MakeSmallMetric(L"\xE950", L"TaskbarWidgetsLimit", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE823", L"TaskbarWidgetsReset", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE9D2", L"TaskbarWidgetsWeek", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE8D4", L"TaskbarWidgetsTokens", L"--"));

    wuxc::Grid::SetRow(titleLine, 0);
    wuxc::Grid::SetRow(limitBar, 1);
    wuxc::Grid::SetRow(metrics, 2);
    compact.Children().Append(titleLine.as<wux::UIElement>());
    compact.Children().Append(limitBar.as<wux::UIElement>());
    compact.Children().Append(metrics.as<wux::UIElement>());

    wuxc::Grid expanded;
    expanded.Name(L"TaskbarWidgetsExpandedPanel");
    expanded.Height(72);
    expanded.Width(230);
    expanded.Visibility(wux::Visibility::Collapsed);

    for (int i = 0; i < 5; ++i) {
        wuxc::RowDefinition row;
        row.Height(wux::GridLengthHelper::FromPixels(14));
        expanded.RowDefinitions().Append(row);
    }

    auto expandedRow0 = MakeExpandedProjectRow(
        L"TaskbarWidgetsExpandedRow0",
        L"TaskbarWidgetsExpandedTitle0",
        L"TaskbarWidgetsExpandedIcon0",
        L"TaskbarWidgetsExpandedState0");
    auto expandedRow1 = MakeExpandedProjectRow(
        L"TaskbarWidgetsExpandedRow1",
        L"TaskbarWidgetsExpandedTitle1",
        L"TaskbarWidgetsExpandedIcon1",
        L"TaskbarWidgetsExpandedState1");
    auto expandedRow2 = MakeExpandedProjectRow(
        L"TaskbarWidgetsExpandedRow2",
        L"TaskbarWidgetsExpandedTitle2",
        L"TaskbarWidgetsExpandedIcon2",
        L"TaskbarWidgetsExpandedState2");
    auto expandedRow3 = MakeExpandedProjectRow(
        L"TaskbarWidgetsExpandedRow3",
        L"TaskbarWidgetsExpandedTitle3",
        L"TaskbarWidgetsExpandedIcon3",
        L"TaskbarWidgetsExpandedState3");
    auto expandedRow4 = MakeExpandedProjectRow(
        L"TaskbarWidgetsExpandedRow4",
        L"TaskbarWidgetsExpandedTitle4",
        L"TaskbarWidgetsExpandedIcon4",
        L"TaskbarWidgetsExpandedState4");
    wuxc::Grid::SetRow(expandedRow0, 0);
    wuxc::Grid::SetRow(expandedRow1, 1);
    wuxc::Grid::SetRow(expandedRow2, 2);
    wuxc::Grid::SetRow(expandedRow3, 3);
    wuxc::Grid::SetRow(expandedRow4, 4);
    expanded.Children().Append(expandedRow0.as<wux::UIElement>());
    expanded.Children().Append(expandedRow1.as<wux::UIElement>());
    expanded.Children().Append(expandedRow2.as<wux::UIElement>());
    expanded.Children().Append(expandedRow3.as<wux::UIElement>());
    expanded.Children().Append(expandedRow4.as<wux::UIElement>());

    root.Children().Append(compact.as<wux::UIElement>());
    root.Children().Append(expanded.as<wux::UIElement>());
    root.Children().Append(MakeWeatherPanel().as<wux::UIElement>());
    root.Children().Append(MakeDiscordPanel().as<wux::UIElement>());
    root.Children().Append(MakeMediaPanel().as<wux::UIElement>());
    root.Children().Append(MakeSteamDownloadPanel().as<wux::UIElement>());
    root.Children().Append(MakeSystemMetricPanel().as<wux::UIElement>());

    wuxc::Border layoutMarker;
    layoutMarker.Name(kTaskbarWidgetsLayoutMarkerName);
    layoutMarker.Visibility(wux::Visibility::Collapsed);
    root.Children().Append(layoutMarker.as<wux::UIElement>());

    auto dragState = std::make_shared<WidgetDragState>();
    root.PointerEntered([root](auto const&, auto const&) {
        auto element = root.as<wux::UIElement>();
        if (GetWidgetDesignFromRoot(element) == L"codex-status") {
            SetExpandedMode(element, true);
        }
    });
    root.PointerExited([root](auto const&, auto const&) {
        auto element = root.as<wux::UIElement>();
        if (GetWidgetDesignFromRoot(element) == L"codex-status") {
            SetExpandedMode(element, false);
            HideCodexHoverPopup();
        }
    });
    root.PointerPressed([root, dragState](auto const&, wuxi::PointerRoutedEventArgs const& args) {
        auto host = root.Parent().try_as<wux::UIElement>();
        if (!host) {
            return;
        }
        auto point = args.GetCurrentPoint(host);
        if (!point.Properties().IsLeftButtonPressed()) {
            return;
        }

        dragState->pressed = true;
        dragState->dragging = false;
        dragState->suppressTap = false;
        dragState->startPointerX = point.Position().X;
        dragState->startLeft = wuxc::Canvas::GetLeft(root);
        if (!std::isfinite(dragState->startLeft)) {
            dragState->startLeft = 0.0;
        }
        root.CapturePointer(args.Pointer());
    });
    root.PointerMoved([root, dragState](auto const&, wuxi::PointerRoutedEventArgs const& args) {
        if (!dragState->pressed) {
            return;
        }
        auto host = root.Parent().try_as<wux::FrameworkElement>();
        if (!host) {
            return;
        }
        auto point = args.GetCurrentPoint(host.as<wux::UIElement>());
        if (!point.Properties().IsLeftButtonPressed()) {
            return;
        }

        double deltaX = point.Position().X - dragState->startPointerX;
        if (!dragState->dragging && std::abs(deltaX) < 4.0) {
            return;
        }
        dragState->dragging = true;
        double width = root.ActualWidth() > 0.0 ? root.ActualWidth() : root.Width();
        double maximumLeft = std::max(0.0, host.ActualWidth() - width);
        double left = std::clamp(dragState->startLeft + deltaX, 0.0, maximumLeft);
        wuxc::Canvas::SetLeft(root, left);
        g_widgetDragOverrides[WidgetElementKey(root.as<wux::UIElement>())] = {
            left, 0, 0, false, std::chrono::steady_clock::time_point::max()};
        args.Handled(true);
    });
    root.PointerReleased([root, dragState](auto const&, wuxi::PointerRoutedEventArgs const& args) {
        if (!dragState->pressed) {
            return;
        }
        dragState->pressed = false;
        root.ReleasePointerCapture(args.Pointer());
        if (!dragState->dragging) {
            g_widgetDragOverrides.erase(WidgetElementKey(root.as<wux::UIElement>()));
            return;
        }

        dragState->dragging = false;
        dragState->suppressTap = true;
        auto host = root.Parent().try_as<wux::FrameworkElement>();
        if (!host) {
            return;
        }
        double left = wuxc::Canvas::GetLeft(root);
        auto design = GetWidgetDesignFromRoot(root.as<wux::UIElement>());
        double width = WidgetDesignWidth(design);
        auto position = taskbar_widgets::PositionForIndependentLeft(
            left, width, host.ActualWidth());
        g_widgetDragOverrides[WidgetElementKey(root.as<wux::UIElement>())] = {
            left, position.anchorPercent, position.offsetPx, true,
            std::chrono::steady_clock::now() + std::chrono::seconds(3)};
        WriteTaskbarWidgetsCommand(
            L"moveWidget", L"", design,
            position.anchorPercent, position.offsetPx);
        args.Handled(true);
    });
    root.PointerCaptureLost([root, dragState](auto const&, wuxi::PointerRoutedEventArgs const&) {
        if (dragState->pressed) {
            dragState->pressed = false;
            dragState->dragging = false;
            g_widgetDragOverrides.erase(WidgetElementKey(root.as<wux::UIElement>()));
        }
    });
    root.Tapped([root, dragState](auto const&, wuxi::TappedRoutedEventArgs const& args) {
        if (dragState->suppressTap) {
            dragState->suppressTap = false;
            args.Handled(true);
            return;
        }
        std::wstring activeDesign = GetWidgetDesignFromRoot(root.as<wux::UIElement>());
        if (activeDesign == L"weather-static") {
            ShowWeatherMenu(root);
        } else if (activeDesign == L"discord-voice") {
            ShowWidgetLibraryWindow();
        } else if (activeDesign == L"media-player") {
            wf::Point point = args.GetPosition(root);
            if (point.X >= 190.0) {
                WriteTaskbarWidgetsCommand(L"mediaToggle");
            } else {
                ShowWidgetLibraryWindow();
            }
        } else if (activeDesign == L"steam-download" ||
                   activeDesign.rfind(L"system-", 0) == 0) {
            if (activeDesign.rfind(L"system-", 0) == 0) {
                WriteTaskbarWidgetsCommand(L"openTaskManager");
            } else {
                ShowWidgetLibraryWindow();
            }
        } else {
            ShowAccountMenu(root);
        }
        args.Handled(true);
    });
    root.RightTapped([root](auto const&, wuxi::RightTappedRoutedEventArgs const& args) {
        auto design = GetWidgetDesignFromRoot(root.as<wux::UIElement>());
        if (design.rfind(L"system-", 0) == 0) {
            WriteTaskbarWidgetsCommand(L"openSettings", L"", design);
        } else {
            ShowWidgetContextMenu();
        }
        args.Handled(true);
    });

    return root;
}

wux::FrameworkElement MakeTaskbarWidgetsRoot() {
    wuxc::Grid root;
    root.Name(L"TaskbarWidgetsRoot");
    root.VerticalAlignment(wux::VerticalAlignment::Center);
    root.HorizontalAlignment(wux::HorizontalAlignment::Right);
    root.Height(48);
    root.Width(1);
    root.Margin(wux::ThicknessHelper::FromLengths(6, 0, 6, 0));

    wuxc::Canvas host;
    host.Name(L"TaskbarWidgetsWidgetHost");
    host.HorizontalAlignment(wux::HorizontalAlignment::Right);
    host.VerticalAlignment(wux::VerticalAlignment::Center);
    host.Height(48);
    host.Width(1);
    root.Children().Append(host.as<wux::UIElement>());

    wuxc::Border layoutMarker;
    layoutMarker.Name(kTaskbarWidgetsLayoutMarkerName);
    layoutMarker.Visibility(wux::Visibility::Collapsed);
    root.Children().Append(layoutMarker.as<wux::UIElement>());

    return root;
}

std::wstring GetCodexStatusPath() {
    return GetTaskbarWidgetsPath(L"State\\codex-status.json");
}

std::wstring GetWeatherStatusPath() {
    return GetTaskbarWidgetsPath(L"State\\weather-static.json");
}

std::wstring GetDiscordStatusPath() {
    return GetTaskbarWidgetsPath(L"State\\discord-voice.json");
}

std::wstring GetMediaStatusPath() {
    return GetTaskbarWidgetsPath(L"State\\media-player.json");
}

std::wstring GetSteamDownloadStatusPath() {
    return GetTaskbarWidgetsPath(L"State\\steam-download.json");
}

std::wstring GetSystemMetricStatusPath(const std::wstring& widgetId) {
    return GetTaskbarWidgetsPath((L"State\\" + widgetId + L".json").c_str());
}

std::wstring GetWidgetSettingsPath() {
    return GetTaskbarWidgetsPath(L"config.json");
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

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
    HANDLE file = CreateFile(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                             nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD written = 0;
    BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()),
                        &written, nullptr);
    CloseHandle(file);
    return ok && static_cast<size_t>(written) == data.size();
}

bool ExtractJsonString(const std::string& json,
                       const char* key,
                       std::wstring& value) {
    std::string decoded;
    if (!taskbar_widgets::json::ExtractStringUtf8(json, key, decoded)) {
        return false;
    }

    int wideLength = MultiByteToWideChar(CP_UTF8, 0, decoded.c_str(),
                                         static_cast<int>(decoded.size()), nullptr, 0);
    if (wideLength <= 0) {
        value.assign(decoded.begin(), decoded.end());
        return true;
    }

    value.resize(wideLength);
    MultiByteToWideChar(CP_UTF8, 0, decoded.c_str(), static_cast<int>(decoded.size()),
                        value.data(), wideLength);
    return true;
}

bool ExtractJsonBool(const std::string& json, const char* key, bool& value) {
    std::string pattern = "\"";
    pattern += key;
    pattern += "\":";

    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) {
        return false;
    }

    size_t start = json.find_first_not_of(" \t\r\n", keyPos + pattern.size());
    if (start == std::string::npos) {
        return false;
    }

    if (json.compare(start, 4, "true") == 0) {
        value = true;
        return true;
    }

    if (json.compare(start, 5, "false") == 0) {
        value = false;
        return true;
    }

    return false;
}

bool IsKnownWidgetDesign(const std::wstring& designId) {
    return std::any_of(
        taskbar_widgets::generated::kWidgets.begin(),
        taskbar_widgets::generated::kWidgets.end(),
        [&designId](const auto& widget) {
            return _wcsicmp(designId.c_str(), std::wstring(widget.id).c_str()) == 0;
        });
}

std::vector<std::wstring> ExtractJsonStringArray(const std::string& json,
                                                 const char* key) {
    std::vector<std::wstring> values;
    std::string pattern = "\"";
    pattern += key;
    pattern += "\"";

    size_t keyPos = json.find(pattern);
    size_t arrayStart = keyPos == std::string::npos
                            ? std::string::npos
                            : json.find('[', keyPos + pattern.size());
    size_t arrayEnd = arrayStart == std::string::npos
                          ? std::string::npos
                          : json.find(']', arrayStart);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos) {
        return values;
    }

    size_t cursor = arrayStart + 1;
    while (cursor < arrayEnd) {
        size_t quote = json.find('"', cursor);
        if (quote == std::string::npos || quote >= arrayEnd) {
            break;
        }

        size_t end = quote + 1;
        bool escaped = false;
        while (end < arrayEnd) {
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

        if (end >= arrayEnd) {
            break;
        }

        std::string itemJson = "{\"value\":\"" + json.substr(quote + 1, end - quote - 1) + "\"}";
        std::wstring item;
        if (ExtractJsonString(itemJson, "value", item) && IsKnownWidgetDesign(item)) {
            bool exists = std::find_if(values.begin(), values.end(),
                                       [&item](const std::wstring& value) {
                                           return _wcsicmp(value.c_str(), item.c_str()) == 0;
                                       }) != values.end();
            if (!exists) {
                values.push_back(item);
            }
        }

        cursor = end + 1;
    }

    return values;
}

struct WidgetRuntimeSettings {
    std::wstring activeDesign = L"codex-status";
    bool enabled = true;
    bool rotationEnabled{};
    long long rotationIntervalSecs = 30;
    std::vector<std::wstring> rotationDesigns;
    bool discordBackgroundEnabled = true;
    bool mediaDarkMode = true;
    long long widgetOffsetPx = 0;
    long long widgetMoveX = 0;
};

std::vector<std::string> ExtractJsonObjectArray(const std::string& json,
                                                const char* key);

WidgetRuntimeSettings ReadWidgetRuntimeSettings() {
    WidgetRuntimeSettings settings;
    std::string json = ReadUtf8File(GetWidgetSettingsPath());
    for (const auto& object : ExtractJsonObjectArray(json, "widgets")) {
        std::wstring id;
        bool enabled = false;
        if (ExtractJsonString(object, "id", id) &&
            ExtractJsonBool(object, "enabled", enabled) && enabled &&
            IsKnownWidgetDesign(id)) {
            settings.activeDesign = id;
            settings.enabled = true;
            break;
        }
    }

    std::wstring layoutMode;
    if (ExtractJsonString(json, "mode", layoutMode)) {
        settings.rotationEnabled = _wcsicmp(layoutMode.c_str(), L"rotation") == 0;
    }

    bool discordBackgroundEnabled = true;
    if (ExtractJsonBool(json, "backgroundEnabled", discordBackgroundEnabled)) {
        settings.discordBackgroundEnabled = discordBackgroundEnabled;
    }

    bool mediaDarkMode = true;
    if (ExtractJsonBool(json, "darkMode", mediaDarkMode)) {
        settings.mediaDarkMode = mediaDarkMode;
    }

    long long rotationInterval = 0;
    if (ExtractJsonInt64(json, "intervalSeconds", rotationInterval)) {
        settings.rotationIntervalSecs = std::clamp(rotationInterval, 5LL, 3600LL);
    }

    long long widgetOffset = 0;
    if (ExtractJsonInt64(json, "widgetOffsetPx", widgetOffset)) {
        settings.widgetOffsetPx = std::clamp(widgetOffset, 0LL, 480LL);
        settings.widgetMoveX = -settings.widgetOffsetPx;
    }

    long long widgetMove = 0;
    if (ExtractJsonInt64(json, "widgetMoveX", widgetMove)) {
        settings.widgetMoveX = std::clamp(widgetMove, -640LL, 640LL);
    }

    settings.rotationDesigns = ExtractJsonStringArray(json, "widgetIds");
    if (settings.rotationDesigns.empty()) {
        settings.rotationDesigns.push_back(settings.activeDesign);
    }

    return settings;
}

std::wstring ReadActiveWidgetDesign() {
    WidgetRuntimeSettings settings = ReadWidgetRuntimeSettings();
    if (settings.rotationEnabled && !settings.rotationDesigns.empty()) {
        long long interval = std::max(5LL, settings.rotationIntervalSecs);
        size_t index = static_cast<size_t>(
            (CurrentUnixTime() / interval) %
            static_cast<long long>(settings.rotationDesigns.size()));
        return settings.rotationDesigns[index];
    }

    return settings.activeDesign;
}

bool WriteActiveWidgetDesign(const std::wstring& designId) {
    if (!IsKnownWidgetDesign(designId)) {
        return false;
    }

    WriteTaskbarWidgetsCommand(L"openSettings", L"", designId);
    return true;
}

std::vector<std::string> ExtractJsonObjectArray(const std::string& json,
                                                 const char* key) {
    std::vector<std::string> objects;
    std::string pattern = "\"";
    pattern += key;
    pattern += "\"";

    size_t keyPos = json.find(pattern);
    size_t arrayStart = keyPos == std::string::npos
                            ? std::string::npos
                            : json.find('[', keyPos + pattern.size());
    if (arrayStart == std::string::npos) {
        return objects;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t objectStart = std::string::npos;
    for (size_t i = arrayStart + 1; i < json.size(); ++i) {
        char ch = json[i];
        if (inString) {
            if (!escaped && ch == '"') {
                inString = false;
            }
            escaped = !escaped && ch == '\\';
            if (ch != '\\') {
                escaped = false;
            }
            continue;
        }

        if (ch == '"') {
            inString = true;
            escaped = false;
        } else if (ch == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }

    return objects;
}

bool ExtractJsonDouble(const std::string& json, const char* key, double& value);

struct SystemMetricWidgetSettings {
    std::wstring displayMode = L"bar";
    std::wstring outlineColor = L"#FFFFFFFF";
    std::wstring firstColor = L"systemAccent";
    std::wstring secondColor = L"#FFFFFFFF";
    bool showIndividualCores = true;
    bool combineLogicalCores = false;
    bool separateUtilization = true;
    bool autoBandwidth = true;
    double bandwidthKiloBytes = 125000.0;
};

double SystemMetricWidgetWidth(const std::wstring& widgetId);

SystemMetricWidgetSettings ReadSystemMetricWidgetSettings(
    const std::wstring& widgetId) {
    SystemMetricWidgetSettings settings;
    if (widgetId == L"system-storage") {
        settings.displayMode = L"text";
        settings.firstColor = L"#FFFFFFFF";
        settings.secondColor = L"#FFFFFFFF";
    } else if (widgetId == L"system-network") {
        settings.displayMode = L"text";
        settings.firstColor = L"systemAccent";  // send
        settings.secondColor = L"systemAccent"; // receive
    } else if (widgetId == L"system-memory") {
        settings.displayMode = L"pie";
        settings.firstColor = L"systemAccent";
        settings.secondColor = L"systemAccent";
        settings.outlineColor = L"systemAccent";
    }

    std::string json = ReadUtf8File(GetWidgetSettingsPath());
    for (const auto& object : ExtractJsonObjectArray(json, "widgets")) {
        std::wstring id;
        if (!ExtractJsonString(object, "id", id) ||
            _wcsicmp(id.c_str(), widgetId.c_str()) != 0) {
            continue;
        }

        ExtractJsonString(object, "displayMode", settings.displayMode);
        ExtractJsonString(object, "outlineColor", settings.outlineColor);
        if (widgetId == L"system-cpu") {
            ExtractJsonString(object, "userColor", settings.firstColor);
            ExtractJsonString(object, "systemColor", settings.secondColor);
            ExtractJsonBool(object, "showIndividualCores", settings.showIndividualCores);
            ExtractJsonBool(object, "combineLogicalCores", settings.combineLogicalCores);
            ExtractJsonBool(object, "separateUtilization", settings.separateUtilization);
            if (!settings.separateUtilization) {
                ExtractJsonString(object, "cpuColor", settings.firstColor);
            }
        } else if (widgetId == L"system-storage") {
            ExtractJsonString(object, "readColor", settings.firstColor);
            ExtractJsonString(object, "writeColor", settings.secondColor);
        } else if (widgetId == L"system-network") {
            ExtractJsonString(object, "sendColor", settings.firstColor);
            ExtractJsonString(object, "receiveColor", settings.secondColor);
            ExtractJsonBool(object, "autoBandwidth", settings.autoBandwidth);
            ExtractJsonDouble(object, "bandwidthKiloBytes", settings.bandwidthKiloBytes);
        } else if (widgetId == L"system-memory") {
            ExtractJsonString(object, "usedColor", settings.firstColor);
        }
        break;
    }

    if (settings.displayMode != L"text" && settings.displayMode != L"bar" &&
        settings.displayMode != L"pie") {
        settings.displayMode = L"text";
    }
    settings.bandwidthKiloBytes = std::clamp(settings.bandwidthKiloBytes, 1.0, 1000000000.0);
    return settings;
}

std::vector<WidgetInstanceRuntime> ReadWidgetInstances() {
    std::string json = ReadUtf8File(GetWidgetSettingsPath());
    std::vector<WidgetInstanceRuntime> widgets;

    auto objects = ExtractJsonObjectArray(json, "widgets");
    long long order = 0;
    for (const auto& object : objects) {
        WidgetInstanceRuntime widget;
        widget.order = order++;
        ExtractJsonString(object, "id", widget.id);
        ExtractJsonString(object, "design", widget.designId);
        if (widget.designId.empty()) {
            ExtractJsonString(object, "designId", widget.designId);
        }
        if (widget.designId.empty()) {
            widget.designId = widget.id;
        }
        if (!IsKnownWidgetDesign(widget.designId)) {
            continue;
        }

        bool enabled = true;
        if (ExtractJsonBool(object, "enabled", enabled)) {
            widget.enabled = enabled;
        }

        long long moveX = 0;
        if (ExtractJsonInt64(object, "offsetPx", moveX) ||
            ExtractJsonInt64(object, "moveX", moveX) ||
            ExtractJsonInt64(object, "widgetMoveX", moveX)) {
            widget.moveX = std::clamp(moveX, -640LL, 640LL);
        }

        long long positionPct = -1;
        if (ExtractJsonInt64(object, "anchorPercent", positionPct) ||
            ExtractJsonInt64(object, "positionPct", positionPct)) {
            widget.positionPct = std::clamp(positionPct, 0LL, 100LL);
        }

        long long explicitOrder = widget.order;
        if (ExtractJsonInt64(object, "order", explicitOrder)) {
            widget.order = explicitOrder;
        }

        if (widget.id.empty()) {
            widget.id = widget.designId;
        }
        widgets.push_back(widget);
    }

    if (objects.empty()) {
        WidgetRuntimeSettings legacy = ReadWidgetRuntimeSettings();
        if (legacy.enabled) {
            WidgetInstanceRuntime widget;
            widget.id = legacy.activeDesign;
            widget.designId = ReadActiveWidgetDesign();
            widget.enabled = true;
            widget.moveX = legacy.widgetMoveX;
            widget.positionPct = -1;
            widget.order = 0;
            widgets.push_back(widget);
        }
    }

    WidgetRuntimeSettings runtimeSettings = ReadWidgetRuntimeSettings();
    if (runtimeSettings.rotationEnabled && !widgets.empty()) {
        std::wstring active = ReadActiveWidgetDesign();
        widgets.erase(
            std::remove_if(widgets.begin(), widgets.end(),
                           [&active](const WidgetInstanceRuntime& widget) {
                               return _wcsicmp(widget.designId.c_str(), active.c_str()) != 0;
                           }),
            widgets.end());
    }

    std::sort(widgets.begin(), widgets.end(),
              [](const WidgetInstanceRuntime& left,
                 const WidgetInstanceRuntime& right) {
                  if (left.order != right.order) {
                      return left.order < right.order;
                  }
                  return left.id < right.id;
              });
    return widgets;
}

std::vector<CodexAccountInfo> ReadCodexAccounts() {
    std::vector<CodexAccountInfo> accounts;
    std::wstring settingsPath = GetTaskbarWidgetsPath(L"settings.json");
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
            ExtractJsonString(object, "email", account.email);
            ExtractJsonString(object, "rateLimitText", account.rateLimitText);
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
        accounts.push_back({L"default", L"Default", L"", L"", true});
    } else if (std::none_of(accounts.begin(), accounts.end(),
                            [](const CodexAccountInfo& account) {
                                return account.active;
                            })) {
        accounts[0].active = true;
    }

    return accounts;
}

std::wstring MakeAccountDisplayName(const CodexAccountInfo& account) {
    std::wstring label = account.label;
    if (!account.email.empty()) {
        label += L" (";
        label += account.email;
        label += L")";
    }

    return label;
}

wuxc::TextBlock MakeMenuText(const std::wstring& value,
                             double fontSize,
                             winrt::Windows::UI::Color color) {
    wuxc::TextBlock text;
    text.Text(value);
    text.FontSize(fontSize);
    text.Foreground(wuxm::SolidColorBrush(color));
    text.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    text.TextAlignment(wux::TextAlignment::Left);
    text.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
    return text;
}

wuxc::FontIcon MakeMenuIcon(PCWSTR glyph,
                            winrt::Windows::UI::Color color,
                            double fontSize = 13) {
    wuxc::FontIcon icon;
    icon.Glyph(glyph);
    icon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(fontSize);
    icon.Width(18);
    icon.Height(18);
    icon.Foreground(wuxm::SolidColorBrush(color));
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    return icon;
}

wux::UIElement MakePngImage(PCWSTR assetName, double width, double height) {
    std::wstring assetPath = GetAssetsFolder();
    assetPath += assetName;

    if (FileExists(assetPath)) {
        wuxc::Image image;
        image.Width(width);
        image.Height(height);
        image.Stretch(wuxm::Stretch::Uniform);
        image.HorizontalAlignment(wux::HorizontalAlignment::Center);
        image.VerticalAlignment(wux::VerticalAlignment::Center);

        wuxmi::BitmapImage source;
        source.UriSource(wf::Uri(ToFileUri(assetPath)));
        image.Source(source);
        return image;
    }

    wuxc::FontIcon icon;
    icon.Glyph(L"\xE706");
    icon.FontFamily(wuxm::FontFamily(L"Segoe MDL2 Assets"));
    icon.FontSize(std::min(width, height) * 0.65);
    icon.Width(width);
    icon.Height(height);
    icon.HorizontalAlignment(wux::HorizontalAlignment::Center);
    icon.VerticalAlignment(wux::VerticalAlignment::Center);
    icon.Foreground(MakeBrush(0xFF, 0xBA, 0xE6, 0xFD));
    return icon;
}

wuxc::Button MakeMenuButton(wux::UIElement const& content, double minHeight) {
    wuxc::Button button;
    button.Content(content);
    button.MinHeight(minHeight);
    button.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(wux::HorizontalAlignment::Stretch);
    button.Padding(wux::ThicknessHelper::FromLengths(8, 5, 8, 5));
    button.Margin(wux::ThicknessHelper::FromLengths(0, 1, 0, 1));
    button.Background(MakeBrush(0x00, 0x00, 0x00, 0x00));
    button.BorderBrush(MakeBrush(0x00, 0x00, 0x00, 0x00));
    button.BorderThickness(wux::ThicknessHelper::FromUniformLength(0));
    return button;
}

wux::UIElement MakeAccountMenuContent(const CodexAccountInfo& account) {
    wuxc::Grid row;
    row.Width(292);
    row.MinHeight(48);

    wuxc::ColumnDefinition stripeColumn;
    stripeColumn.Width(wux::GridLengthHelper::FromPixels(4));
    wuxc::ColumnDefinition textColumn;
    textColumn.Width(wux::GridLengthHelper::FromPixels(246));
    wuxc::ColumnDefinition iconColumn;
    iconColumn.Width(wux::GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(stripeColumn);
    row.ColumnDefinitions().Append(textColumn);
    row.ColumnDefinitions().Append(iconColumn);

    wuxc::Border stripe;
    stripe.Width(3);
    stripe.Margin(wux::ThicknessHelper::FromLengths(0, 4, 8, 4));
    stripe.Background(account.active ? MakeBrush(0xFF, 0x38, 0xBD, 0xF8)
                                     : MakeBrush(0x00, 0x00, 0x00, 0x00));
    wuxc::Grid::SetColumn(stripe, 0);

    wuxc::StackPanel textStack;
    textStack.Orientation(wuxc::Orientation::Vertical);
    textStack.VerticalAlignment(wux::VerticalAlignment::Center);
    textStack.Margin(wux::ThicknessHelper::FromLengths(8, 0, 6, 0));

    auto title = MakeMenuText(
        MakeAccountDisplayName(account),
        12,
        account.active ? winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC}
                       : winrt::Windows::UI::Color{0xE8, 0xF8, 0xFA, 0xFC});

    auto metrics = MakeMenuText(
        account.rateLimitText.empty() ? L"-- -- -- --" : account.rateLimitText,
        11,
        account.active ? winrt::Windows::UI::Color{0xFF, 0x7D, 0xD3, 0xFC}
                       : winrt::Windows::UI::Color{0xB8, 0x94, 0xA3, 0xB8});
    metrics.Margin(wux::ThicknessHelper::FromLengths(0, 2, 0, 0));

    textStack.Children().Append(title.as<wux::UIElement>());
    textStack.Children().Append(metrics.as<wux::UIElement>());
    wuxc::Grid::SetColumn(textStack, 1);

    auto icon = MakeMenuIcon(account.active ? L"\xE73E" : L"\xE7C3",
                             account.active
                                 ? winrt::Windows::UI::Color{0xFF, 0x38, 0xBD, 0xF8}
                                 : winrt::Windows::UI::Color{0x80, 0x94, 0xA3, 0xB8},
                             12);
    icon.HorizontalAlignment(wux::HorizontalAlignment::Right);
    wuxc::Grid::SetColumn(icon, 2);

    row.Children().Append(stripe.as<wux::UIElement>());
    row.Children().Append(textStack.as<wux::UIElement>());
    row.Children().Append(icon.as<wux::UIElement>());
    return row;
}

wux::UIElement MakeActionMenuContent(PCWSTR glyph, PCWSTR label) {
    wuxc::StackPanel row;
    row.Orientation(wuxc::Orientation::Horizontal);
    row.Width(292);

    auto icon = MakeMenuIcon(glyph, winrt::Windows::UI::Color{0xCC, 0xF8, 0xFA, 0xFC}, 12);
    icon.Margin(wux::ThicknessHelper::FromLengths(1, 0, 8, 0));

    auto text = MakeMenuText(label, 12,
                             winrt::Windows::UI::Color{0xF0, 0xF8, 0xFA, 0xFC});
    text.VerticalAlignment(wux::VerticalAlignment::Center);

    row.Children().Append(icon.as<wux::UIElement>());
    row.Children().Append(text.as<wux::UIElement>());
    return row;
}

void AppendActionMenuButton(wuxc::StackPanel const& panel,
                            wuxc::Flyout const& flyout,
                            PCWSTR glyph,
                            PCWSTR label,
                            PCWSTR command) {
    auto button = MakeMenuButton(MakeActionMenuContent(glyph, label), 34);
    button.Click([flyout, command](auto const&, auto const&) {
        WriteTaskbarWidgetsCommand(command);
        flyout.Hide();
    });
    panel.Children().Append(button.as<wux::UIElement>());
}

void DrawPopupText(HDC dc,
                   const std::wstring& text,
                   RECT rect,
                   COLORREF color,
                   HFONT font,
                   UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                                 DT_END_ELLIPSIS) {
    SetTextColor(dc, color);
    HGDIOBJ oldFont = SelectObject(dc, font);
    DrawTextW(dc, text.c_str(), -1, &rect, format);
    SelectObject(dc, oldFont);
}

void DrawCodexHoverPopup(HDC dc, RECT clientRect) {
    HBRUSH background = CreateSolidBrush(RGB(24, 29, 38));
    FillRect(dc, &clientRect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);

    HFONT titleFont = CreateFontW(
        -12, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT detailFont = CreateFontW(
        -11, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT iconFont = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    RECT titleRect{14, 8, clientRect.right - 14, 28};
    DrawPopupText(dc, L"Active Antigravity Windows", titleRect,
                  RGB(248, 250, 252), titleFont);

    int y = 34;
    for (size_t i = 0; i < g_codexHoverTitles.size() && i < 5; ++i) {
        RECT row{10, y, clientRect.right - 10, y + 34};
        HBRUSH rowBrush = CreateSolidBrush(i == 0 ? RGB(34, 42, 54)
                                                  : RGB(29, 35, 45));
        FillRect(dc, &row, rowBrush);
        DeleteObject(rowBrush);

        RECT iconRect{row.left + 8, row.top, row.left + 28, row.bottom};
        RECT nameRect{row.left + 34, row.top + 3, row.right - 10, row.top + 19};
        RECT detailRect{row.left + 34, row.top + 18, row.right - 10, row.bottom - 2};
        DrawPopupText(dc, L"\xE950", iconRect, RGB(95, 212, 255), iconFont,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawPopupText(dc, g_codexHoverTitles[i], nameRect,
                      RGB(241, 245, 249), titleFont);
        DrawPopupText(dc, i == 0 ? L"Primary hover target" : L"Secondary window",
                      detailRect, RGB(148, 163, 184), detailFont);
        y += 38;
    }

    DeleteObject(titleFont);
    DeleteObject(detailFont);
    DeleteObject(iconFont);
}

LRESULT CALLBACK CodexHoverWindowProc(HWND window,
                                      UINT message,
                                      WPARAM wParam,
                                      LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT rect{};
            GetClientRect(window, &rect);
            DrawCodexHoverPopup(dc, rect);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_DESTROY:
            if (g_codexHoverWindow == window) {
                g_codexHoverWindow = nullptr;
            }
            return 0;
    }

    return DefWindowProc(window, message, wParam, lParam);
}

RECT CalculateCodexHoverRect(int width, int height) {
    HWND taskbar = FindCurrentProcessTaskbarWindow();
    RECT taskbarRect{};
    if (!taskbar || !GetWindowRect(taskbar, &taskbarRect)) {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &taskbarRect, 0);
        taskbarRect.top = taskbarRect.bottom - 48;
    }

    HMONITOR monitor = MonitorFromRect(&taskbarRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfo(monitor, &monitorInfo);

    int x = taskbarRect.right - 540;
    int y = taskbarRect.top - height - 10;
    if (taskbarRect.top <= monitorInfo.rcMonitor.top + 4) {
        y = taskbarRect.bottom + 10;
    }

    x = std::max<int>(monitorInfo.rcWork.left + 8,
                      std::min<int>(x, monitorInfo.rcWork.right - width - 8));
    y = std::max<int>(monitorInfo.rcWork.top + 8,
                      std::min<int>(y, monitorInfo.rcWork.bottom - height - 8));
    return RECT{x, y, x + width, y + height};
}

void HideCodexHoverPopup() {
    if (g_codexHoverWindow && IsWindow(g_codexHoverWindow)) {
        DestroyWindow(g_codexHoverWindow);
    }
    g_codexHoverWindow = nullptr;
}

void ShowCodexHoverPopup() {
    try {
        g_codexHoverTitles = GetAntigravityProjectTitles();
        if (g_codexHoverTitles.size() <= 1) {
            HideCodexHoverPopup();
            return;
        }

        if (g_codexHoverWindow && IsWindow(g_codexHoverWindow)) {
            DestroyWindow(g_codexHoverWindow);
        }

        constexpr PCWSTR className = L"TaskbarWidgetsCodexHoverPopup";
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASS windowClass{};
            windowClass.lpfnWndProc = CodexHoverWindowProc;
            windowClass.hInstance = g_hookModule ? g_hookModule : GetModuleHandle(nullptr);
            windowClass.lpszClassName = className;
            windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            if (!RegisterClass(&windowClass) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                Wh_Log(L"RegisterClass failed for codex hover popup: %u",
                       GetLastError());
                return;
            }
            classRegistered = true;
        }

        int width = 292;
        int rows = static_cast<int>(std::min<size_t>(g_codexHoverTitles.size(), 5));
        int height = 42 + rows * 38 + 8;
        RECT popupRect = CalculateCodexHoverRect(width, height);
        g_codexHoverWindow = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className, L"TaskbarWidgets Codex Hover", WS_POPUP,
            popupRect.left, popupRect.top, width, height,
            FindCurrentProcessTaskbarWindow(), nullptr,
            g_hookModule ? g_hookModule : GetModuleHandle(nullptr), nullptr);
        if (!g_codexHoverWindow) {
            Wh_Log(L"CreateWindowEx failed for codex hover popup: %u",
                   GetLastError());
            return;
        }

        SetWindowPos(g_codexHoverWindow, HWND_TOPMOST, popupRect.left,
                     popupRect.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(g_codexHoverWindow, nullptr, TRUE);
    } catch (...) {
        Wh_Log(L"ShowCodexHoverPopup failed");
    }
}

void DrawAccountMenuPopup(HDC dc, RECT clientRect) {
    HBRUSH background = CreateSolidBrush(RGB(31, 35, 42));
    FillRect(dc, &clientRect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);

    HFONT titleFont = CreateFontW(
        -13, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT detailFont = CreateFontW(
        -12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT iconFont = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    constexpr int width = 336;
    constexpr int padding = 10;
    constexpr int headerHeight = 46;
    constexpr int accountHeight = 56;
    constexpr int actionHeight = 36;

    RECT titleRect{padding + 4, 7, width - 54, 26};
    RECT subtitleRect{padding + 4, 25, width - 54, 42};
    RECT settingsRect{width - 44, 8, width - 10, 40};
    if (g_accountMenuHoveredIndex == 0) {
        HBRUSH hoverBrush = CreateSolidBrush(RGB(43, 50, 60));
        FillRect(dc, &settingsRect, hoverBrush);
        DeleteObject(hoverBrush);
    }

    DrawPopupText(dc, L"TaskbarWidgets", titleRect, RGB(248, 250, 252),
                  titleFont);
    DrawPopupText(dc, L"Widget library and Codex accounts", subtitleRect,
                  RGB(148, 163, 184), detailFont);
    DrawPopupText(dc, L"\xE713", settingsRect, RGB(203, 213, 225), iconFont,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT headerSeparator{padding + 4, headerHeight - 1, width - padding - 4,
                         headerHeight};
    HBRUSH headerSeparatorBrush = CreateSolidBrush(RGB(62, 70, 82));
    FillRect(dc, &headerSeparator, headerSeparatorBrush);
    DeleteObject(headerSeparatorBrush);

    int y = headerHeight + 2;
    int hitIndex = 1;
    for (const auto& account : g_accountMenuAccounts) {
        RECT row{padding, y, width - padding, y + accountHeight};
        if (account.active || g_accountMenuHoveredIndex == hitIndex) {
            COLORREF rowColor = account.active ? RGB(39, 47, 58)
                                               : RGB(36, 41, 49);
            if (account.active && g_accountMenuHoveredIndex == hitIndex) {
                rowColor = RGB(45, 55, 68);
            }

            HBRUSH rowBrush = CreateSolidBrush(rowColor);
            FillRect(dc, &row, rowBrush);
            DeleteObject(rowBrush);
        }

        if (account.active) {

            RECT stripe{row.left, row.top + 6, row.left + 3, row.bottom - 6};
            HBRUSH stripeBrush = CreateSolidBrush(RGB(56, 189, 248));
            FillRect(dc, &stripe, stripeBrush);
            DeleteObject(stripeBrush);
        }

        RECT titleRect{row.left + 12, row.top + 6, row.right - 34,
                       row.top + 28};
        RECT detailRect{row.left + 12, row.top + 28, row.right - 34,
                        row.bottom - 5};
        RECT iconRect{row.right - 27, row.top, row.right - 6, row.bottom};

        DrawPopupText(dc, MakeAccountDisplayName(account), titleRect,
                      account.active ? RGB(248, 250, 252) : RGB(225, 231, 238),
                      titleFont);
        DrawPopupText(dc,
                      account.rateLimitText.empty() ? L"-- -- -- --"
                                                    : account.rateLimitText,
                      detailRect,
                      account.active ? RGB(125, 211, 252)
                                     : RGB(148, 163, 184),
                      detailFont);
        DrawPopupText(dc, account.active ? L"\xE73E" : L"\xE7C3", iconRect,
                      account.active ? RGB(56, 189, 248)
                                     : RGB(100, 116, 139),
                      iconFont, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        y += accountHeight;
        ++hitIndex;
    }

    RECT separator{padding + 4, y + 5, width - padding - 4, y + 6};
    HBRUSH separatorBrush = CreateSolidBrush(RGB(62, 70, 82));
    FillRect(dc, &separator, separatorBrush);
    DeleteObject(separatorBrush);
    y += 12;

    struct ActionRow {
        PCWSTR glyph;
        PCWSTR label;
    };
    constexpr ActionRow actions[] = {
        {L"\xE710", L"Add Codex account"},
        {L"\xE77B", L"Login active Codex account"},
        {L"\xE74D", L"Delete active Codex account"},
        {L"\xE72C", L"Restart IDE with active account"}};

    for (const auto& action : actions) {
        RECT row{padding, y, width - padding, y + actionHeight};
        if (g_accountMenuHoveredIndex == hitIndex) {
            HBRUSH hoverBrush = CreateSolidBrush(RGB(36, 41, 49));
            FillRect(dc, &row, hoverBrush);
            DeleteObject(hoverBrush);
        }

        RECT iconRect{row.left + 8, row.top, row.left + 28, row.bottom};
        RECT textRect{row.left + 36, row.top, row.right - 8, row.bottom};
        DrawPopupText(dc, action.glyph, iconRect, RGB(203, 213, 225), iconFont,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawPopupText(dc, action.label, textRect, RGB(241, 245, 249), detailFont);
        y += actionHeight;
        ++hitIndex;
    }

    DeleteObject(titleFont);
    DeleteObject(detailFont);
    DeleteObject(iconFont);
}

void RebuildAccountMenuHitItems(int width) {
    g_accountMenuHitItems.clear();
    constexpr int padding = 10;
    constexpr int headerHeight = 46;
    constexpr int accountHeight = 56;
    constexpr int actionHeight = 36;

    g_accountMenuHitItems.push_back(AccountMenuHitItem{
        .rect = RECT{width - 44, 8, width - 10, 40},
        .command = L"__openWidgetLibrary",
    });

    int y = headerHeight + 2;
    for (const auto& account : g_accountMenuAccounts) {
        g_accountMenuHitItems.push_back(AccountMenuHitItem{
            .rect = RECT{padding, y, width - padding, y + accountHeight},
            .command = L"switchAccount",
            .accountId = account.id,
        });
        y += accountHeight;
    }

    y += 12;
    std::wstring activeAccountId;
    for (const auto& account : g_accountMenuAccounts) {
        if (account.active) {
            activeAccountId = account.id;
            break;
        }
    }

    constexpr PCWSTR commands[] = {
        L"addAccount",
        L"loginActiveAccount",
        L"deleteAccount",
        L"restartIde"};
    for (PCWSTR command : commands) {
        std::wstring accountId;
        if (_wcsicmp(command, L"deleteAccount") == 0) {
            accountId = activeAccountId;
        }
        g_accountMenuHitItems.push_back(AccountMenuHitItem{
            .rect = RECT{padding, y, width - padding, y + actionHeight},
            .command = command,
            .accountId = accountId,
        });
        y += actionHeight;
    }
}

int HitTestAccountMenuItem(POINT point) {
    for (size_t i = 0; i < g_accountMenuHitItems.size(); ++i) {
        if (PtInRect(&g_accountMenuHitItems[i].rect, point)) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

LRESULT CALLBACK AccountMenuWindowProc(HWND window,
                                       UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT rect{};
            GetClientRect(window, &rect);
            DrawAccountMenuPopup(dc, rect);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hoveredIndex = HitTestAccountMenuItem(point);
            if (hoveredIndex != g_accountMenuHoveredIndex) {
                g_accountMenuHoveredIndex = hoveredIndex;
                InvalidateRect(window, nullptr, FALSE);

                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_accountMenuHoveredIndex != -1) {
                g_accountMenuHoveredIndex = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hitIndex = HitTestAccountMenuItem(point);
            if (hitIndex >= 0 &&
                hitIndex < static_cast<int>(g_accountMenuHitItems.size())) {
                const auto& item = g_accountMenuHitItems[hitIndex];
                if (item.command == L"__openWidgetLibrary") {
                    DestroyWindow(window);
                    ShowWidgetLibraryWindow();
                    return 0;
                }

                WriteTaskbarWidgetsCommand(item.command, item.accountId);
                DestroyWindow(window);
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window, &client);
            if (!PtInRect(&client, point)) {
                DestroyWindow(window);
            }
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                DestroyWindow(window);
            }
            return 0;

        case WM_KILLFOCUS:
            DestroyWindow(window);
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_CANCELMODE:
            DestroyWindow(window);
            return 0;

        case WM_CAPTURECHANGED:
            if (g_accountMenuWindow == window &&
                reinterpret_cast<HWND>(lParam) != window) {
                DestroyWindow(window);
            }
            return 0;

        case WM_DESTROY:
            if (g_accountMenuWindow == window) {
                g_accountMenuWindow = nullptr;
            }
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            g_accountMenuHoveredIndex = -1;
            return 0;
    }

    return DefWindowProc(window, message, wParam, lParam);
}

RECT CalculateAccountMenuRect(int width, int height) {
    HWND taskbar = FindCurrentProcessTaskbarWindow();
    RECT taskbarRect{};
    if (!taskbar || !GetWindowRect(taskbar, &taskbarRect)) {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &taskbarRect, 0);
        taskbarRect.top = taskbarRect.bottom - 48;
    }

    HMONITOR monitor = MonitorFromRect(&taskbarRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfo(monitor, &monitorInfo);

    POINT anchor{};
    if (!GetCursorPos(&anchor)) {
        anchor.x = taskbarRect.right - 220;
        anchor.y = taskbarRect.top + ((taskbarRect.bottom - taskbarRect.top) / 2);
    }

    int x = anchor.x - (width / 2);
    int y = taskbarRect.top - height - 8;
    bool horizontalTaskbar =
        (taskbarRect.right - taskbarRect.left) >=
        (taskbarRect.bottom - taskbarRect.top);
    if (horizontalTaskbar) {
        if (taskbarRect.top <= monitorInfo.rcMonitor.top + 4) {
            y = taskbarRect.bottom + 8;
        }
    } else {
        y = anchor.y - (height / 2);
        if (taskbarRect.left <= monitorInfo.rcMonitor.left + 4) {
            x = taskbarRect.right + 8;
        } else {
            x = taskbarRect.left - width - 8;
        }
    }

    x = std::max<int>(monitorInfo.rcWork.left + 8,
                      std::min<int>(x, monitorInfo.rcWork.right - width - 8));
    y = std::max<int>(monitorInfo.rcWork.top + 8,
                      std::min<int>(y, monitorInfo.rcWork.bottom - height - 8));

    return RECT{x, y, x + width, y + height};
}

void ShowAccountMenu(wux::FrameworkElement const& root) {
    try {
        (void)root;

        if (g_accountMenuWindow && IsWindow(g_accountMenuWindow)) {
            DestroyWindow(g_accountMenuWindow);
            return;
        }

        g_accountMenuAccounts = ReadCodexAccounts();
        if (g_accountMenuAccounts.empty()) {
            g_accountMenuAccounts.push_back(
                {L"default", L"Default", L"", L"", true});
        }
        constexpr int width = 336;
        int height = 58 + static_cast<int>(g_accountMenuAccounts.size()) * 56 +
                     12 + 4 * 36 + 10;
        RebuildAccountMenuHitItems(width);

        constexpr PCWSTR className = L"TaskbarWidgetsAccountMenuPopup";
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASS windowClass{};
            windowClass.lpfnWndProc = AccountMenuWindowProc;
            windowClass.hInstance = g_hookModule ? g_hookModule : GetModuleHandle(nullptr);
            windowClass.lpszClassName = className;
            windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            if (!RegisterClass(&windowClass) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                Wh_Log(L"RegisterClass failed for account menu popup: %u",
                       GetLastError());
                return;
            }
            classRegistered = true;
        }

        RECT popupRect = CalculateAccountMenuRect(width, height);
        g_accountMenuWindow = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className, L"TaskbarWidgets Accounts", WS_POPUP,
            popupRect.left, popupRect.top, width, height,
            FindCurrentProcessTaskbarWindow(), nullptr,
            g_hookModule ? g_hookModule : GetModuleHandle(nullptr), nullptr);
        if (!g_accountMenuWindow) {
            Wh_Log(L"CreateWindowEx failed for account menu popup: %u",
                   GetLastError());
            return;
        }

        SetWindowPos(g_accountMenuWindow, HWND_TOPMOST, popupRect.left,
                     popupRect.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCapture(g_accountMenuWindow);
        InvalidateRect(g_accountMenuWindow, nullptr, TRUE);
        Wh_Log(L"Account menu popup shown at %d,%d %dx%d",
               popupRect.left, popupRect.top, width, height);
    } catch (winrt::hresult_error const& ex) {
        Wh_Log(L"ShowAccountMenu failed: 0x%08X %s", ex.code(),
               ex.message().c_str());
    } catch (...) {
        Wh_Log(L"ShowAccountMenu failed with unknown exception");
    }
}

void DrawFilledRoundRect(HDC dc,
                         RECT rect,
                         int radius,
                         COLORREF fill,
                         COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

RECT GetWidgetLibraryCardRect(int index, int width) {
    int contentLeft = width >= 820 ? 284 : 24;
    int contentRight = width - 28;
    int gap = 22;
    int cardWidth = width >= 1080 ? 360 : contentRight - contentLeft;
    int cardHeight = 150;
    int columns = width >= 1080 ? 2 : 1;
    int x = contentLeft + (index % columns) * (cardWidth + gap);
    int y = 150 + (index / columns) * (cardHeight + gap);

    return RECT{x, y, x + cardWidth, y + cardHeight};
}

void RebuildWidgetLibraryHitItems(int width, int height) {
    g_widgetLibraryHitItems.clear();
    g_widgetLibraryHitItems.push_back(WidgetLibraryHitItem{
        .rect = RECT{width - 62, 24, width - 24, 62},
        .command = L"__close",
    });
    g_widgetLibraryHitItems.push_back(WidgetLibraryHitItem{
        .rect = GetWidgetLibraryCardRect(0, width),
        .command = L"selectWidgetDesign",
        .designId = L"codex-status",
    });
    g_widgetLibraryHitItems.push_back(WidgetLibraryHitItem{
        .rect = GetWidgetLibraryCardRect(1, width),
        .command = L"selectWidgetDesign",
        .designId = L"weather-static",
    });
    g_widgetLibraryHitItems.push_back(WidgetLibraryHitItem{
        .rect = GetWidgetLibraryCardRect(2, width),
        .command = L"selectWidgetDesign",
        .designId = L"steam-download",
    });
    g_widgetLibraryHitItems.push_back(WidgetLibraryHitItem{
        .rect = GetWidgetLibraryCardRect(3, width),
        .command = L"addWidgetLibrary",
    });
}

int HitTestWidgetLibraryItem(POINT point) {
    for (size_t i = 0; i < g_widgetLibraryHitItems.size(); ++i) {
        if (PtInRect(&g_widgetLibraryHitItems[i].rect, point)) {
            return static_cast<int>(i);
        }
    }

    return -1;
}

void DrawWidgetLibraryCard(HDC dc,
                           RECT card,
                           PCWSTR glyph,
                           PCWSTR title,
                           PCWSTR subtitle,
                           PCWSTR detail,
                           bool selected,
                           bool hovered,
                           HFONT titleFont,
                           HFONT detailFont,
                           HFONT smallFont,
                           HFONT iconFont,
                           COLORREF accent) {
    COLORREF fill = selected ? RGB(31, 43, 56)
                             : hovered ? RGB(29, 35, 45) : RGB(24, 29, 38);
    COLORREF border = selected ? accent : RGB(48, 57, 72);
    DrawFilledRoundRect(dc, card, 18, fill, border);

    RECT stripe{card.left + 18, card.top + 18, card.left + 22, card.bottom - 18};
    HBRUSH stripeBrush = CreateSolidBrush(accent);
    FillRect(dc, &stripe, stripeBrush);
    DeleteObject(stripeBrush);

    RECT iconRect{card.left + 38, card.top + 28, card.left + 76, card.top + 68};
    DrawPopupText(dc, glyph, iconRect, accent, iconFont,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT titleRect{card.left + 88, card.top + 24, card.right - 24, card.top + 52};
    RECT subtitleRect{card.left + 88, card.top + 53, card.right - 24,
                      card.top + 78};
    RECT detailRect{card.left + 28, card.bottom - 46, card.right - 28,
                    card.bottom - 18};
    DrawPopupText(dc, title, titleRect, RGB(248, 250, 252), titleFont);
    DrawPopupText(dc, subtitle, subtitleRect, RGB(148, 163, 184), detailFont);
    DrawPopupText(dc, detail, detailRect, RGB(203, 213, 225), smallFont);

    if (selected) {
        RECT badge{card.right - 92, card.top + 18, card.right - 20, card.top + 42};
        DrawFilledRoundRect(dc, badge, 12, RGB(15, 118, 110), RGB(20, 184, 166));
        DrawPopupText(dc, L"ACTIVE", badge, RGB(236, 253, 245), smallFont,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
}

void DrawWidgetLibraryWindow(HDC dc, RECT clientRect) {
    HBRUSH background = CreateSolidBrush(RGB(13, 17, 23));
    FillRect(dc, &clientRect, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    HFONT heroFont = CreateFontW(
        -30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT titleFont = CreateFontW(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT detailFont = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT smallFont = CreateFontW(
        -11, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT iconFont = CreateFontW(
        -24, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    HFONT navIconFont = CreateFontW(
        -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    int width = clientRect.right - clientRect.left;
    int height = clientRect.bottom - clientRect.top;
    bool wide = width >= 820;
    std::wstring activeDesign = ReadActiveWidgetDesign();

    if (wide) {
        RECT sidebar{0, 0, 248, height};
        HBRUSH sidebarBrush = CreateSolidBrush(RGB(18, 23, 31));
        FillRect(dc, &sidebar, sidebarBrush);
        DeleteObject(sidebarBrush);

        RECT brand{28, 30, 220, 60};
        RECT nav1{28, 104, 220, 138};
        RECT nav2{28, 146, 220, 180};
        DrawPopupText(dc, L"TaskbarWidgets", brand, RGB(248, 250, 252), titleFont);
        DrawFilledRoundRect(dc, nav1, 12, RGB(30, 41, 59), RGB(51, 65, 85));
        DrawPopupText(dc, L"\xE8A5", RECT{nav1.left + 12, nav1.top, nav1.left + 38, nav1.bottom},
                      RGB(56, 189, 248), navIconFont,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawPopupText(dc, L"Widget library",
                      RECT{nav1.left + 46, nav1.top, nav1.right - 10, nav1.bottom},
                      RGB(248, 250, 252), detailFont);
        DrawPopupText(dc, L"\xE713", RECT{nav2.left + 12, nav2.top, nav2.left + 38, nav2.bottom},
                      RGB(148, 163, 184), navIconFont,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        DrawPopupText(dc, L"Design settings",
                      RECT{nav2.left + 46, nav2.top, nav2.right - 10, nav2.bottom},
                      RGB(148, 163, 184), detailFont);
    }

    RECT closeRect{width - 62, 24, width - 24, 62};
    if (g_widgetLibraryHoveredIndex == 0) {
        DrawFilledRoundRect(dc, closeRect, 12, RGB(35, 42, 52), RGB(58, 68, 82));
    }
    DrawPopupText(dc, L"\xE711", closeRect, RGB(203, 213, 225), navIconFont,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int contentLeft = wide ? 284 : 24;
    RECT hero{contentLeft, 30, width - 90, 72};
    RECT sub{contentLeft, 76, width - 90, 104};
    DrawPopupText(dc, L"Widget Library", hero, RGB(248, 250, 252), heroFont);
    DrawPopupText(dc,
                  L"Pick one taskbar design. New design packs will live here.",
                  sub, RGB(148, 163, 184), detailFont);

    DrawWidgetLibraryCard(
        dc, GetWidgetLibraryCardRect(0, width), L"\xE950", L"Codex Status",
        L"Current Antigravity / Codex quota design",
        L"Rate limit, reset, weekly quota, 30-day token metrics",
        activeDesign == L"codex-status", g_widgetLibraryHoveredIndex == 1,
        titleFont, detailFont, smallFont, iconFont, RGB(56, 189, 248));
    DrawWidgetLibraryCard(
        dc, GetWidgetLibraryCardRect(1, width), L"\xE706", L"Static Weather",
        L"Simple test design with fixed weather values",
        L"Istanbul 24 C, static condition, humidity placeholder",
        activeDesign == L"weather-static", g_widgetLibraryHoveredIndex == 2,
        titleFont, detailFont, smallFont, iconFont, RGB(248, 197, 85));
    DrawWidgetLibraryCard(
        dc, GetWidgetLibraryCardRect(2, width), L"\xE896", L"Steam Downloads",
        L"Active Steam download with capsule art and ETA",
        L"Game title, progress, speed, and remaining time",
        activeDesign == L"steam-download", g_widgetLibraryHoveredIndex == 3,
        titleFont, detailFont, smallFont, iconFont, RGB(102, 192, 244));
    DrawWidgetLibraryCard(
        dc, GetWidgetLibraryCardRect(3, width), L"\xE710", L"Add Library",
        L"Reserved for importing your future native design packs",
        L"Design pack loading is the next layer; this action is queued.",
        false, g_widgetLibraryHoveredIndex == 4, titleFont, detailFont,
        smallFont, iconFont, RGB(94, 234, 212));

    DeleteObject(heroFont);
    DeleteObject(titleFont);
    DeleteObject(detailFont);
    DeleteObject(smallFont);
    DeleteObject(iconFont);
    DeleteObject(navIconFont);
}

LRESULT CALLBACK WidgetLibraryWindowProc(HWND window,
                                         UINT message,
                                         WPARAM wParam,
                                         LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE: {
            RECT rect{};
            GetClientRect(window, &rect);
            RebuildWidgetLibraryHitItems(rect.right - rect.left,
                                         rect.bottom - rect.top);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT rect{};
            GetClientRect(window, &rect);
            DrawWidgetLibraryWindow(dc, rect);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hoveredIndex = HitTestWidgetLibraryItem(point);
            if (hoveredIndex != g_widgetLibraryHoveredIndex) {
                g_widgetLibraryHoveredIndex = hoveredIndex;
                InvalidateRect(window, nullptr, FALSE);

                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_widgetLibraryHoveredIndex != -1) {
                g_widgetLibraryHoveredIndex = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            int hitIndex = HitTestWidgetLibraryItem(point);
            if (hitIndex >= 0 &&
                hitIndex < static_cast<int>(g_widgetLibraryHitItems.size())) {
                const auto& item = g_widgetLibraryHitItems[hitIndex];
                if (item.command == L"__close") {
                    DestroyWindow(window);
                    return 0;
                }

                if (item.command == L"selectWidgetDesign") {
                    if (WriteActiveWidgetDesign(item.designId)) {
                        RefreshInsertedTaskbarWidgetsRoots();
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }

                if (item.command == L"addWidgetLibrary") {
                    WriteTaskbarWidgetsCommand(item.command);
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }

        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(window);
                return 0;
            }
            break;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_CANCELMODE:
            DestroyWindow(window);
            return 0;

        case WM_CAPTURECHANGED:
            if (g_widgetLibraryWindow == window &&
                reinterpret_cast<HWND>(lParam) != window) {
                DestroyWindow(window);
            }
            return 0;

        case WM_DESTROY:
            if (g_widgetLibraryWindow == window) {
                g_widgetLibraryWindow = nullptr;
            }
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            g_widgetLibraryHoveredIndex = -1;
            g_widgetLibraryHitItems.clear();
            return 0;
    }

    return DefWindowProc(window, message, wParam, lParam);
}

RECT CalculateWidgetLibraryRect() {
    HWND taskbar = FindCurrentProcessTaskbarWindow();
    RECT anchor{};
    if (!taskbar || !GetWindowRect(taskbar, &anchor)) {
        SystemParametersInfo(SPI_GETWORKAREA, 0, &anchor, 0);
    }

    HMONITOR monitor = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    GetMonitorInfo(monitor, &monitorInfo);
    return monitorInfo.rcWork;
}

void ShowWidgetLibraryWindow() {
    WriteTaskbarWidgetsCommand(L"openSettings");
    Wh_Log(L"Settings app open command queued");
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

struct WeatherDaySnapshot {
    std::wstring label = L"--";
    int weatherCode = 3;
    double max = 0;
    double min = 0;
};

struct WeatherSnapshot {
    bool loaded{};
    long long updatedAtUnix = 0;
    std::wstring location = L"Izmir";
    double temperature = 0;
    double apparentTemperature = 0;
    double humidity = 0;
    int weatherCode = 3;
    double windSpeed = 0;
    std::vector<WeatherDaySnapshot> days;
};

std::wstring WeatherIconAsset(int code) {
    if (code == 0) {
        return L"weather\\sun.png";
    }
    if (code == 1 || code == 2) {
        return L"weather\\partly.png";
    }
    if (code == 3) {
        return L"weather\\cloud.png";
    }
    if (code == 45 || code == 48) {
        return L"weather\\fog.png";
    }
    if (code >= 51 && code <= 57) {
        return L"weather\\drizzle.png";
    }
    if ((code >= 61 && code <= 65) || (code >= 80 && code <= 82)) {
        return L"weather\\rain.png";
    }
    if (code >= 71 && code <= 77) {
        return L"weather\\snow.png";
    }
    if (code >= 95) {
        return L"weather\\thunder.png";
    }

    return L"weather\\cloud.png";
}

std::wstring WeatherDescription(int code) {
    if (code == 0) {
        return L"Sunny";
    }
    if (code == 1 || code == 2) {
        return L"Partly cloudy";
    }
    if (code == 3) {
        return L"Cloudy";
    }
    if (code == 45 || code == 48) {
        return L"Fog";
    }
    if ((code >= 51 && code <= 57) || (code >= 61 && code <= 65) ||
        (code >= 80 && code <= 82)) {
        return L"Rain";
    }
    if (code >= 71 && code <= 77) {
        return L"Snow";
    }
    if (code >= 95) {
        return L"Thunderstorm";
    }

    return L"Weather";
}

std::wstring FormatTemperature(double value) {
    WCHAR buffer[24]{};
    swprintf_s(buffer, L"%.0f\x00B0", value);
    return buffer;
}

std::wstring FormatWeatherDate(long long unixTime) {
    FILETIME fileTime{};
    ULONGLONG value =
        (static_cast<ULONGLONG>(unixTime) * 10000000ULL) + 116444736000000000ULL;
    fileTime.dwLowDateTime = static_cast<DWORD>(value);
    fileTime.dwHighDateTime = static_cast<DWORD>(value >> 32);

    SYSTEMTIME utc{};
    SYSTEMTIME local{};
    FileTimeToSystemTime(&fileTime, &utc);
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);

    WCHAR buffer[32]{};
    swprintf_s(buffer, L"%02u:%02u • %02u/%02u", local.wHour, local.wMinute,
               local.wDay, local.wMonth);
    return buffer;
}

std::wstring FormatMediaPosition(long long seconds) {
    long long clamped = std::clamp(seconds, 0LL, 99LL * 60LL + 59LL);
    WCHAR buffer[16]{};
    swprintf_s(buffer, L"%02lld.%02lld", clamped / 60, clamped % 60);
    return buffer;
}

WeatherSnapshot ReadWeatherSnapshot() {
    WeatherSnapshot snapshot;
    std::string json = ReadUtf8File(GetWeatherStatusPath());
    if (json.empty()) {
        return snapshot;
    }

    snapshot.loaded = true;
    ExtractJsonInt64(json, "updatedAtUnix", snapshot.updatedAtUnix);
    ExtractJsonString(json, "location", snapshot.location);
    ExtractJsonDouble(json, "temperature", snapshot.temperature);
    ExtractJsonDouble(json, "apparentTemperature",
                      snapshot.apparentTemperature);
    ExtractJsonDouble(json, "humidity", snapshot.humidity);
    ExtractJsonDouble(json, "windSpeed", snapshot.windSpeed);

    long long weatherCode = 0;
    if (ExtractJsonInt64(json, "weatherCode", weatherCode)) {
        snapshot.weatherCode = static_cast<int>(weatherCode);
    }

    size_t daysKey = json.find("\"days\"");
    size_t arrayStart = daysKey == std::string::npos
                            ? std::string::npos
                            : json.find('[', daysKey);
    size_t arrayEnd = arrayStart == std::string::npos
                          ? std::string::npos
                          : json.find(']', arrayStart);
    if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
        size_t cursor = arrayStart;
        while (cursor < arrayEnd && snapshot.days.size() < 7) {
            size_t objectStart = json.find('{', cursor);
            if (objectStart == std::string::npos || objectStart >= arrayEnd) {
                break;
            }

            size_t objectEnd = json.find('}', objectStart);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd) {
                break;
            }

            std::string object = json.substr(objectStart, objectEnd - objectStart + 1);
            WeatherDaySnapshot day;
            ExtractJsonString(object, "label", day.label);
            ExtractJsonDouble(object, "max", day.max);
            ExtractJsonDouble(object, "min", day.min);
            long long dayCode = 0;
            if (ExtractJsonInt64(object, "weatherCode", dayCode)) {
                day.weatherCode = static_cast<int>(dayCode);
            }
            snapshot.days.push_back(day);
            cursor = objectEnd + 1;
        }
    }

    return snapshot;
}

struct DiscordVoiceUserSnapshot {
    std::wstring id;
    std::wstring displayName;
    std::wstring avatarPath;
    std::wstring animatedAvatarPath;
    bool speaking{};
};

struct DiscordVoiceSnapshot {
    bool loaded{};
    bool connected{};
    std::wstring status = L"Discord";
    std::wstring channelName;
    std::vector<DiscordVoiceUserSnapshot> users;
};

DiscordVoiceSnapshot ReadDiscordVoiceSnapshot() {
    DiscordVoiceSnapshot snapshot;
    std::string json = ReadUtf8File(GetDiscordStatusPath());
    if (json.empty()) {
        return snapshot;
    }

    snapshot.loaded = true;
    ExtractJsonBool(json, "connected", snapshot.connected);
    ExtractJsonString(json, "status", snapshot.status);
    ExtractJsonString(json, "channelName", snapshot.channelName);

    size_t usersKey = json.find("\"users\"");
    size_t arrayStart = usersKey == std::string::npos
                            ? std::string::npos
                            : json.find('[', usersKey);
    size_t arrayEnd = arrayStart == std::string::npos
                          ? std::string::npos
                          : json.find(']', arrayStart);
    if (arrayStart != std::string::npos && arrayEnd != std::string::npos) {
        size_t cursor = arrayStart;
        while (cursor < arrayEnd && snapshot.users.size() < 5) {
            size_t objectStart = json.find('{', cursor);
            if (objectStart == std::string::npos || objectStart >= arrayEnd) {
                break;
            }

            size_t objectEnd = json.find('}', objectStart);
            if (objectEnd == std::string::npos || objectEnd > arrayEnd) {
                break;
            }

            std::string object = json.substr(objectStart, objectEnd - objectStart + 1);
            DiscordVoiceUserSnapshot user;
            ExtractJsonString(object, "id", user.id);
            ExtractJsonString(object, "displayName", user.displayName);
            ExtractJsonString(object, "avatarPath", user.avatarPath);
            ExtractJsonString(object, "animatedAvatarPath", user.animatedAvatarPath);
            ExtractJsonBool(object, "speaking", user.speaking);
            if (!user.id.empty() || !user.avatarPath.empty()) {
                snapshot.users.push_back(user);
            }
            cursor = objectEnd + 1;
        }
    }

    return snapshot;
}

struct MediaSnapshot {
    bool loaded{};
    bool active{};
    bool playing{};
    long long positionSeconds = 34;
    std::wstring title;
    std::wstring artist;
    std::wstring coverPath;
    std::wstring backgroundLeftColor;
    std::wstring backgroundRightColor;
    std::wstring accentColor;
    std::wstring textColor;
};

MediaSnapshot ReadMediaSnapshot() {
    MediaSnapshot snapshot;
    snapshot.coverPath = GetAssetsFolder() + L"widgets\\media_cover.png";
    std::string json = ReadUtf8File(GetMediaStatusPath());
    if (json.empty()) {
        return snapshot;
    }

    snapshot.loaded = true;
    ExtractJsonBool(json, "active", snapshot.active);
    ExtractJsonBool(json, "playing", snapshot.playing);
    ExtractJsonString(json, "title", snapshot.title);
    ExtractJsonString(json, "artist", snapshot.artist);
    ExtractJsonString(json, "coverPath", snapshot.coverPath);
    ExtractJsonString(json, "backgroundLeftColor", snapshot.backgroundLeftColor);
    ExtractJsonString(json, "backgroundRightColor", snapshot.backgroundRightColor);
    ExtractJsonString(json, "accentColor", snapshot.accentColor);
    ExtractJsonString(json, "textColor", snapshot.textColor);
    long long position = 0;
    if (ExtractJsonInt64(json, "positionSeconds", position)) {
        snapshot.positionSeconds = std::clamp(position, 0LL, 24LL * 60LL * 60LL);
    }

    return snapshot;
}

struct SteamDownloadSnapshot {
    bool loaded{};
    bool steamRunning{};
    bool active{};
    std::wstring title;
    std::wstring subtitle;
    std::wstring detail;
    std::wstring status;
    std::wstring coverPath;
    double progressPercent{};
    double speedMbS{};
    long long remainingSeconds{};
};

SteamDownloadSnapshot ReadSteamDownloadSnapshot() {
    SteamDownloadSnapshot snapshot;
    std::string json = ReadUtf8File(GetSteamDownloadStatusPath());
    if (json.empty()) {
        return snapshot;
    }

    snapshot.loaded = true;
    ExtractJsonBool(json, "steamRunning", snapshot.steamRunning);
    ExtractJsonBool(json, "active", snapshot.active);
    ExtractJsonString(json, "title", snapshot.title);
    ExtractJsonString(json, "subtitle", snapshot.subtitle);
    ExtractJsonString(json, "detail", snapshot.detail);
    ExtractJsonString(json, "status", snapshot.status);
    ExtractJsonString(json, "coverPath", snapshot.coverPath);
    ExtractJsonDouble(json, "progressPercent", snapshot.progressPercent);
    ExtractJsonDouble(json, "speedMbS", snapshot.speedMbS);
    ExtractJsonInt64(json, "remainingSeconds", snapshot.remainingSeconds);
    snapshot.progressPercent = std::clamp(snapshot.progressPercent, 0.0, 100.0);
    return snapshot;
}

struct CpuCoreMetricSnapshot {
    std::wstring id;
    double percent{};
    double userPercent{};
    double kernelPercent{};
};

struct SystemMetricSnapshot {
    bool loaded{};
    double totalPercent{};
    double userPercent{};
    double kernelPercent{};
    std::vector<CpuCoreMetricSnapshot> cores;
    double primaryRate{};
    double secondaryRate{};
    double primaryPercent{};
    double secondaryPercent{};
    long long receiveLinkSpeedBitsPerSecond{};
    long long sendLinkSpeedBitsPerSecond{};
    long long usedBytes{};
    long long totalBytes{};
    double usedPercent{};
};

SystemMetricSnapshot ReadSystemMetricSnapshot(const std::wstring& widgetId) {
    SystemMetricSnapshot snapshot;
    std::string json = ReadUtf8File(GetSystemMetricStatusPath(widgetId));
    if (json.empty()) return snapshot;

    long long schemaVersion = 0;
    long long updatedAtUnix = 0;
    std::wstring snapshotWidgetId;
    std::wstring status;
    if (!ExtractJsonInt64(json, "schemaVersion", schemaVersion) ||
        schemaVersion != 1 ||
        !ExtractJsonInt64(json, "updatedAtUnix", updatedAtUnix) ||
        updatedAtUnix < CurrentUnixTime() - 60 ||
        updatedAtUnix > CurrentUnixTime() + 300 ||
        !ExtractJsonString(json, "widgetId", snapshotWidgetId) ||
        _wcsicmp(snapshotWidgetId.c_str(), widgetId.c_str()) != 0 ||
        !ExtractJsonString(json, "status", status) ||
        _wcsicmp(status.c_str(), L"ok") != 0) {
        return snapshot;
    }

    snapshot.loaded = true;
    if (widgetId == L"system-cpu") {
        ExtractJsonDouble(json, "totalPercent", snapshot.totalPercent);
        ExtractJsonDouble(json, "userPercent", snapshot.userPercent);
        ExtractJsonDouble(json, "kernelPercent", snapshot.kernelPercent);
        for (const auto& object : ExtractJsonObjectArray(json, "cores")) {
            CpuCoreMetricSnapshot core;
            ExtractJsonString(object, "id", core.id);
            ExtractJsonDouble(object, "percent", core.percent);
            ExtractJsonDouble(object, "userPercent", core.userPercent);
            ExtractJsonDouble(object, "kernelPercent", core.kernelPercent);
            core.percent = std::clamp(core.percent, 0.0, 100.0);
            core.userPercent = std::clamp(core.userPercent, 0.0, 100.0);
            core.kernelPercent = std::clamp(core.kernelPercent, 0.0, 100.0);
            if (!std::isfinite(core.percent)) core.percent = 0;
            if (!std::isfinite(core.userPercent)) core.userPercent = 0;
            if (!std::isfinite(core.kernelPercent)) core.kernelPercent = 0;
            if (!core.id.empty()) snapshot.cores.push_back(core);
        }
        snapshot.totalPercent = std::clamp(snapshot.totalPercent, 0.0, 100.0);
        snapshot.userPercent = std::clamp(snapshot.userPercent, 0.0, 100.0);
        snapshot.kernelPercent = std::clamp(snapshot.kernelPercent, 0.0, 100.0);
        if (!std::isfinite(snapshot.totalPercent)) snapshot.totalPercent = 0;
        if (!std::isfinite(snapshot.userPercent)) snapshot.userPercent = 0;
        if (!std::isfinite(snapshot.kernelPercent)) snapshot.kernelPercent = 0;
    } else if (widgetId == L"system-storage") {
        ExtractJsonDouble(json, "readBytesPerSecond", snapshot.primaryRate);
        ExtractJsonDouble(json, "writeBytesPerSecond", snapshot.secondaryRate);
        ExtractJsonDouble(json, "readPercent", snapshot.primaryPercent);
        ExtractJsonDouble(json, "writePercent", snapshot.secondaryPercent);
    } else if (widgetId == L"system-network") {
        ExtractJsonDouble(json, "receiveBytesPerSecond", snapshot.primaryRate);
        ExtractJsonDouble(json, "sendBytesPerSecond", snapshot.secondaryRate);
        ExtractJsonInt64(json, "receiveLinkSpeedBitsPerSecond", snapshot.receiveLinkSpeedBitsPerSecond);
        ExtractJsonInt64(json, "sendLinkSpeedBitsPerSecond", snapshot.sendLinkSpeedBitsPerSecond);
    } else if (widgetId == L"system-memory") {
        ExtractJsonInt64(json, "usedBytes", snapshot.usedBytes);
        ExtractJsonInt64(json, "totalBytes", snapshot.totalBytes);
        ExtractJsonDouble(json, "usedPercent", snapshot.usedPercent);
        snapshot.usedBytes = std::max(0LL, snapshot.usedBytes);
        snapshot.totalBytes = std::max(0LL, snapshot.totalBytes);
        snapshot.usedPercent = std::clamp(snapshot.usedPercent, 0.0, 100.0);
    }
    snapshot.primaryRate = std::max(0.0, snapshot.primaryRate);
    snapshot.secondaryRate = std::max(0.0, snapshot.secondaryRate);
    if (!std::isfinite(snapshot.primaryRate)) snapshot.primaryRate = 0;
    if (!std::isfinite(snapshot.secondaryRate)) snapshot.secondaryRate = 0;
    snapshot.primaryPercent = std::isfinite(snapshot.primaryPercent)
                                  ? std::clamp(snapshot.primaryPercent, 0.0, 100.0)
                                  : 0;
    snapshot.secondaryPercent = std::isfinite(snapshot.secondaryPercent)
                                    ? std::clamp(snapshot.secondaryPercent, 0.0, 100.0)
                                    : 0;
    snapshot.receiveLinkSpeedBitsPerSecond = std::max(0LL, snapshot.receiveLinkSpeedBitsPerSecond);
    snapshot.sendLinkSpeedBitsPerSecond = std::max(0LL, snapshot.sendLinkSpeedBitsPerSecond);
    return snapshot;
}

double SystemMetricWidgetWidth(const std::wstring& widgetId) {
    auto settings = ReadSystemMetricWidgetSettings(widgetId);
    if (widgetId == L"system-storage" || widgetId == L"system-network") {
        return settings.displayMode == L"text" ? 93.0
             : settings.displayMode == L"bar" ? 8.0 : 24.0;
    }
    if (widgetId == L"system-memory") {
        return settings.displayMode == L"bar" ? 8.0
             : settings.displayMode == L"pie" ? 24.0 : 44.0;
    }

    size_t count = 1;
    if (settings.showIndividualCores) {
        auto snapshot = ReadSystemMetricSnapshot(widgetId);
        count = std::max<size_t>(1, snapshot.cores.size());
        if (settings.combineLogicalCores) count = (count + 1) / 2;
    }
    double unit = settings.displayMode == L"bar" ? 8.0
                : settings.displayMode == L"pie" ? 24.0 : 44.0;
    return unit * static_cast<double>(count) + 3.0 * static_cast<double>(count - 1);
}

std::wstring FormatMetricRate(double bytesPerSecond) {
    const wchar_t* unit = L"B/s";
    double value = std::max(0.0, bytesPerSecond);
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0 * 1024.0;
        unit = L"GB/s";
    } else if (value >= 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0;
        unit = L"MB/s";
    } else if (value >= 1024.0) {
        value /= 1024.0;
        unit = L"KB/s";
    }
    WCHAR buffer[32]{};
    swprintf_s(buffer, L"%.0f %s", value, unit);
    return buffer;
}

std::wstring FormatMetricBytes(long long bytes) {
    double value = static_cast<double>(std::max(0LL, bytes));
    const wchar_t* unit = L"B";
    if (value >= 1024.0 * 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0 * 1024.0;
        unit = L"GB";
    } else if (value >= 1024.0 * 1024.0) {
        value /= 1024.0 * 1024.0;
        unit = L"MB";
    }
    WCHAR buffer[24]{};
    swprintf_s(buffer, value >= 100 ? L"%.0f %s" : L"%.1f %s", value, unit);
    return buffer;
}

void DrawWeatherIcon(HDC dc,
                     const std::wstring& assetName,
                     int x,
                     int y,
                     int width,
                     int height) {
    if (!g_gdiplusToken) {
        return;
    }

    std::wstring path = GetAssetsFolder() + assetName;
    if (!FileExists(path)) {
        return;
    }

    gdi::Graphics graphics(dc);
    graphics.SetInterpolationMode(gdi::InterpolationModeHighQualityBicubic);
    gdi::Image image(path.c_str());
    if (image.GetLastStatus() == gdi::Ok) {
        graphics.DrawImage(&image, x, y, width, height);
    }
}

RECT GetWeatherSettingsButtonRect(RECT clientRect) {
    return RECT{clientRect.right - 46, 10, clientRect.right - 14, 42};
}

int HitTestWeatherMenuItem(POINT point, RECT clientRect) {
    RECT settingsRect = GetWeatherSettingsButtonRect(clientRect);
    if (PtInRect(&settingsRect, point)) {
        return 0;
    }

    return -1;
}

void DrawWeatherMenuPopup(HDC dc, RECT clientRect) {
    HBRUSH background = CreateSolidBrush(RGB(17, 17, 17));
    FillRect(dc, &clientRect, background);
    DeleteObject(background);

    SetBkMode(dc, TRANSPARENT);

    HFONT dayFont = CreateFontW(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT tempFont = CreateFontW(
        -18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT detailFont = CreateFontW(
        -13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT iconFont = CreateFontW(
        -15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");

    RECT settingsRect = GetWeatherSettingsButtonRect(clientRect);
    if (g_weatherMenuHoveredIndex == 0) {
        HBRUSH hoverBrush = CreateSolidBrush(RGB(34, 34, 34));
        FillRect(dc, &settingsRect, hoverBrush);
        DeleteObject(hoverBrush);
    }
    DrawPopupText(dc, L"\xE713", settingsRect, RGB(218, 218, 224), iconFont,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    WeatherSnapshot weather = ReadWeatherSnapshot();
    std::vector<WeatherDaySnapshot> days = weather.days;
    if (days.empty()) {
        days.push_back(WeatherDaySnapshot{
            .label = L"Bugün",
            .weatherCode = weather.weatherCode,
            .max = weather.temperature,
            .min = weather.apparentTemperature,
        });
    }

    constexpr int paddingX = 30;
    constexpr int rowHeight = 51;
    int y = 50;
    for (size_t i = 0; i < days.size() && i < 7; ++i) {
        const auto& day = days[i];
        RECT dayRect{paddingX, y, paddingX + 90, y + rowHeight};
        RECT maxRect{clientRect.right - 108, y, clientRect.right - 64,
                     y + rowHeight};
        RECT minRect{clientRect.right - 56, y, clientRect.right - 18,
                     y + rowHeight};

        DrawPopupText(dc, day.label, dayRect, RGB(245, 245, 245), dayFont);
        DrawWeatherIcon(dc, WeatherIconAsset(day.weatherCode), paddingX + 108,
                        y + 3, 44, 44);
        DrawPopupText(dc, FormatTemperature(day.max), maxRect,
                      RGB(245, 245, 245), tempFont,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        DrawPopupText(dc, FormatTemperature(day.min), minRect,
                      RGB(168, 168, 176), tempFont,
                      DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        y += rowHeight;
    }

    if (!weather.loaded) {
        RECT statusRect{paddingX, clientRect.bottom - 34, clientRect.right - paddingX,
                        clientRect.bottom - 12};
        DrawPopupText(dc, L"Weather data is loading...", statusRect,
                      RGB(168, 168, 176), detailFont);
    }

    DeleteObject(dayFont);
    DeleteObject(tempFont);
    DeleteObject(detailFont);
    DeleteObject(iconFont);
}

LRESULT CALLBACK WeatherMenuWindowProc(HWND window,
                                       UINT message,
                                       WPARAM wParam,
                                       LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(window, &ps);
            RECT rect{};
            GetClientRect(window, &rect);
            DrawWeatherMenuPopup(dc, rect);
            EndPaint(window, &ps);
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window, &client);
            int hoveredIndex = HitTestWeatherMenuItem(point, client);
            if (hoveredIndex != g_weatherMenuHoveredIndex) {
                g_weatherMenuHoveredIndex = hoveredIndex;
                InvalidateRect(window, nullptr, FALSE);

                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = window;
                TrackMouseEvent(&track);
            }
            return 0;
        }

        case WM_MOUSELEAVE:
            if (g_weatherMenuHoveredIndex != -1) {
                g_weatherMenuHoveredIndex = -1;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window, &client);
            if (HitTestWeatherMenuItem(point, client) == 0) {
                DestroyWindow(window);
                ShowWidgetLibraryWindow();
                return 0;
            }
            return 0;
        }

        case WM_LBUTTONDOWN:
        case WM_RBUTTONDOWN:
        case WM_MBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT client{};
            GetClientRect(window, &client);
            if (!PtInRect(&client, point)) {
                DestroyWindow(window);
            }
            return 0;
        }

        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE) {
                DestroyWindow(window);
            }
            return 0;

        case WM_KILLFOCUS:
        case WM_CANCELMODE:
            DestroyWindow(window);
            return 0;

        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;

        case WM_CAPTURECHANGED:
            if (g_weatherMenuWindow == window &&
                reinterpret_cast<HWND>(lParam) != window) {
                DestroyWindow(window);
            }
            return 0;

        case WM_DESTROY:
            if (g_weatherMenuWindow == window) {
                g_weatherMenuWindow = nullptr;
            }
            if (GetCapture() == window) {
                ReleaseCapture();
            }
            g_weatherMenuHoveredIndex = -1;
            return 0;
    }

    return DefWindowProc(window, message, wParam, lParam);
}

void ShowWeatherMenu(wux::FrameworkElement const& root) {
    try {
        (void)root;

        if (g_weatherMenuWindow && IsWindow(g_weatherMenuWindow)) {
            DestroyWindow(g_weatherMenuWindow);
            return;
        }

        constexpr PCWSTR className = L"TaskbarWidgetsWeatherPopup";
        static bool classRegistered = false;
        if (!classRegistered) {
            WNDCLASS windowClass{};
            windowClass.lpfnWndProc = WeatherMenuWindowProc;
            windowClass.hInstance = g_hookModule ? g_hookModule : GetModuleHandle(nullptr);
            windowClass.lpszClassName = className;
            windowClass.hCursor = LoadCursor(nullptr, IDC_ARROW);
            if (!RegisterClass(&windowClass) &&
                GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                Wh_Log(L"RegisterClass failed for weather popup: %u",
                       GetLastError());
                return;
            }
            classRegistered = true;
        }

        constexpr int width = 336;
        constexpr int height = 440;
        RECT popupRect = CalculateAccountMenuRect(width, height);

        g_weatherMenuWindow = CreateWindowEx(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            className, L"TaskbarWidgets Weather", WS_POPUP,
            popupRect.left, popupRect.top, width, height,
            FindCurrentProcessTaskbarWindow(), nullptr,
            g_hookModule ? g_hookModule : GetModuleHandle(nullptr), nullptr);
        if (!g_weatherMenuWindow) {
            Wh_Log(L"CreateWindowEx failed for weather popup: %u",
                   GetLastError());
            return;
        }

        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 28, 28);
        SetWindowRgn(g_weatherMenuWindow, region, TRUE);
        SetWindowPos(g_weatherMenuWindow, HWND_TOPMOST, popupRect.left,
                     popupRect.top, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetCapture(g_weatherMenuWindow);
        InvalidateRect(g_weatherMenuWindow, nullptr, TRUE);
        Wh_Log(L"Weather popup shown at %d,%d %dx%d",
               popupRect.left, popupRect.top, width, height);
    } catch (...) {
        Wh_Log(L"ShowWeatherMenu failed with unknown exception");
    }
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

long long CurrentUnixMillis() {
    FILETIME fileTime{};
    GetSystemTimeAsFileTime(&fileTime);

    ULARGE_INTEGER value{};
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;

    constexpr unsigned long long windowsToUnix100ns = 116444736000000000ULL;
    return static_cast<long long>((value.QuadPart - windowsToUnix100ns) /
                                  10000ULL);
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

winrt::Windows::UI::Color GetRateLimitColor(double remainingPercent) {
    if (remainingPercent < 0) {
        return {0xF0, 0xF8, 0xFA, 0xFC};
    }

    if (remainingPercent >= 60) {
        return {0xFF, 0x22, 0xC5, 0x5E};
    }

    if (remainingPercent >= 25) {
        return {0xFF, 0xF5, 0x9E, 0x0B};
    }

    return {0xFF, 0xEF, 0x44, 0x44};
}

double GetRemainingPercent(double usedPercent) {
    if (usedPercent < 0) {
        return -1;
    }

    double remaining = 100.0 - usedPercent;
    if (remaining < 0) {
        return 0;
    }

    if (remaining > 100) {
        return 100;
    }

    return remaining;
}

void SetNamedText(wux::UIElement const& root,
                  PCWSTR name,
                  const std::wstring& text) {
    auto block = FindNamedTextBlock(root, name);
    if (block && block.Text() != text) {
        block.Text(text);
    }
}

void SetMediaTitleText(wux::UIElement const& root, const std::wstring& text) {
    auto first = FindNamedTextBlock(root, L"TaskbarWidgetsMediaTitle");
    auto second = FindNamedTextBlock(root, L"TaskbarWidgetsMediaTitleClone");
    auto marqueeElement =
        FindNamedFrameworkElement(root, L"TaskbarWidgetsMediaTitleMarquee");
    if (!first || !second || !marqueeElement) {
        return;
    }

    std::wstring value = Trim(text);
    if (value.empty()) {
        value = L"Media";
    }

    constexpr double viewportWidth = 112.0;
    constexpr double averageCharWidth = 6.2;
    constexpr double separatorWidth = 18.0;
    constexpr double pixelsPerSecond = 18.0;
    const std::wstring separator = L"  •";

    double textWidth = static_cast<double>(value.size()) * averageCharWidth;
    if (textWidth <= viewportWidth) {
        if (first.Text() != value) {
            first.Text(value);
        }
        if (second.Text() != L"") {
            second.Text(L"");
        }
        first.Width(viewportWidth);
        second.Width(0);
        auto transform =
            marqueeElement.RenderTransform().try_as<wuxm::TranslateTransform>();
        if (!transform) {
            transform = wuxm::TranslateTransform();
            marqueeElement.RenderTransform(transform);
        }
        transform.X(0);
        return;
    }

    std::wstring item = value + separator;
    double itemWidth = textWidth + separatorWidth;
    if (first.Text() != item) {
        first.Text(item);
    }
    if (second.Text() != item) {
        second.Text(item);
    }
    first.Width(itemWidth);
    second.Width(itemWidth);

    auto transform =
        marqueeElement.RenderTransform().try_as<wuxm::TranslateTransform>();
    if (!transform) {
        transform = wuxm::TranslateTransform();
        marqueeElement.RenderTransform(transform);
    }

    double elapsed = static_cast<double>(CurrentUnixMillis() % 600000LL) / 1000.0;
    double offset = std::fmod(elapsed * pixelsPerSecond, itemWidth);
    transform.X(-offset);
}

void SetSteamTitleText(wux::UIElement const& root, const std::wstring& text) {
    auto first = FindNamedTextBlock(root, L"TaskbarWidgetsSteamTitle");
    auto second = FindNamedTextBlock(root, L"TaskbarWidgetsSteamTitleClone");
    auto marqueeElement =
        FindNamedFrameworkElement(root, L"TaskbarWidgetsSteamTitleMarquee");
    if (!first || !second || !marqueeElement) {
        return;
    }

    std::wstring value = Trim(text);
    if (value.empty()) {
        value = L"Steam";
    }

    constexpr double viewportWidth = 93.0;
    constexpr double averageCharWidth = 6.2;
    constexpr double separatorWidth = 18.0;
    constexpr double pixelsPerSecond = 18.0;
    const std::wstring separator = L"  •";

    double textWidth = static_cast<double>(value.size()) * averageCharWidth;
    if (textWidth <= viewportWidth) {
        if (first.Text() != value) {
            first.Text(value);
        }
        if (second.Text() != L"") {
            second.Text(L"");
        }
        first.Width(viewportWidth);
        second.Width(0);
        auto transform =
            marqueeElement.RenderTransform().try_as<wuxm::TranslateTransform>();
        if (!transform) {
            transform = wuxm::TranslateTransform();
            marqueeElement.RenderTransform(transform);
        }
        transform.X(0);
        return;
    }

    std::wstring item = value + separator;
    double itemWidth = textWidth + separatorWidth;
    if (first.Text() != item) {
        first.Text(item);
    }
    if (second.Text() != item) {
        second.Text(item);
    }
    first.Width(itemWidth);
    second.Width(itemWidth);

    auto transform =
        marqueeElement.RenderTransform().try_as<wuxm::TranslateTransform>();
    if (!transform) {
        transform = wuxm::TranslateTransform();
        marqueeElement.RenderTransform(transform);
    }

    double elapsed = static_cast<double>(CurrentUnixMillis() % 600000LL) / 1000.0;
    double offset = std::fmod(elapsed * pixelsPerSecond, itemWidth);
    transform.X(-offset);
}

void SetNamedTextColor(wux::UIElement const& root,
                       PCWSTR name,
                       winrt::Windows::UI::Color color) {
    auto block = FindNamedTextBlock(root, name);
    if (block) {
        block.Foreground(wuxm::SolidColorBrush(color));
    }
}

void SetNamedImageSource(wux::UIElement const& root,
                         PCWSTR name,
                         const std::wstring& assetName) {
    auto element = FindNamedFrameworkElement(root, name);
    auto image = element ? element.try_as<wuxc::Image>() : nullptr;
    if (!image) {
        return;
    }

    std::wstring assetPath = GetAssetsFolder() + assetName;
    if (!FileExists(assetPath)) {
        return;
    }

    winrt::hstring pathTag(assetPath);
    winrt::hstring currentTag =
        winrt::unbox_value_or<winrt::hstring>(element.Tag(), winrt::hstring{});
    if (currentTag == pathTag) {
        return;
    }

    wuxmi::BitmapImage source;
    source.UriSource(wf::Uri(ToFileUri(assetPath)));
    image.Source(source);
    element.Tag(winrt::box_value(pathTag));
}

void SetNamedImageFileSource(wux::UIElement const& root,
                             PCWSTR name,
                             const std::wstring& path) {
    auto element = FindNamedFrameworkElement(root, name);
    auto image = element ? element.try_as<wuxc::Image>() : nullptr;
    if (!image || path.empty() || !FileExists(path)) {
        return;
    }

    unsigned long long version = GetFileWriteVersion(path);
    std::wstring taggedPath =
        path + L"#" + std::to_wstring(static_cast<unsigned long long>(version));
    winrt::hstring pathTag(taggedPath);
    winrt::hstring currentTag =
        winrt::unbox_value_or<winrt::hstring>(element.Tag(), winrt::hstring{});
    if (currentTag == pathTag) {
        return;
    }

    wuxmi::BitmapImage source;
    std::wstring uri = ToFileUri(path);
    if (version != 0) {
        uri += L"?v=" + std::to_wstring(version);
    }
    source.UriSource(wf::Uri(uri));
    image.Source(source);
    element.Tag(winrt::box_value(pathTag));
}

void SetNamedEllipseImageFill(wux::UIElement const& root,
                              PCWSTR name,
                              const std::wstring& path) {
    auto element = FindNamedFrameworkElement(root, name);
    auto ellipse = element ? element.try_as<wuxs::Ellipse>() : nullptr;
    if (!ellipse || path.empty() || !FileExists(path)) {
        return;
    }

    winrt::hstring pathTag(path);
    winrt::hstring currentTag =
        winrt::unbox_value_or<winrt::hstring>(element.Tag(), winrt::hstring{});
    if (currentTag == pathTag) {
        return;
    }

    wuxmi::BitmapImage source;
    source.UriSource(wf::Uri(ToFileUri(path)));
    wuxm::ImageBrush brush;
    brush.ImageSource(source);
    brush.Stretch(wuxm::Stretch::UniformToFill);
    ellipse.Fill(brush);
    element.Tag(winrt::box_value(pathTag));
}

void SetNamedOpacity(wux::UIElement const& root, PCWSTR name, double opacity) {
    auto element = FindNamedFrameworkElement(root, name);
    if (element) {
        element.Opacity(opacity);
    }
}

void SetNamedBorderVisual(wux::UIElement const& root,
                          PCWSTR name,
                          bool active) {
    auto element = FindNamedFrameworkElement(root, name);
    auto border = element ? element.try_as<wuxc::Border>() : nullptr;
    if (!border) {
        return;
    }

    border.BorderThickness(wux::ThicknessHelper::FromUniformLength(2));
    border.BorderBrush(active ? MakeBrush(0xFF, 0x22, 0xC5, 0x5E)
                              : MakeBrush(0x00, 0x00, 0x00, 0x00));
}

void SetNamedBorderImageBackground(wux::UIElement const& root,
                                   PCWSTR name,
                                   const std::wstring& path) {
    auto element = FindNamedFrameworkElement(root, name);
    auto border = element ? element.try_as<wuxc::Border>() : nullptr;
    if (!border || path.empty() || !FileExists(path)) {
        return;
    }

    unsigned long long version = GetFileWriteVersion(path);
    std::wstring taggedPath =
        path + L"#" + std::to_wstring(static_cast<unsigned long long>(version));
    winrt::hstring pathTag(taggedPath);
    winrt::hstring currentTag =
        winrt::unbox_value_or<winrt::hstring>(element.Tag(), winrt::hstring{});
    if (currentTag == pathTag) {
        return;
    }

    wuxmi::BitmapImage source;
    std::wstring uri = ToFileUri(path);
    if (version != 0) {
        uri += L"?v=" + std::to_wstring(version);
    }
    source.UriSource(wf::Uri(uri));
    wuxm::ImageBrush brush;
    brush.ImageSource(source);
    brush.Stretch(wuxm::Stretch::UniformToFill);
    border.Background(brush);
    element.Tag(winrt::box_value(pathTag));
}

void SetNamedBorderFill(wux::UIElement const& root,
                        PCWSTR name,
                        wuxm::Brush const& brush) {
    auto element = FindNamedFrameworkElement(root, name);
    auto border = element ? element.try_as<wuxc::Border>() : nullptr;
    if (border) {
        border.Background(brush);
        element.Tag(nullptr);
    }
}

void SetNamedIconGlyph(wux::UIElement const& root, PCWSTR name, PCWSTR glyph) {
    auto element = FindNamedFrameworkElement(root, name);
    auto icon = element ? element.try_as<wuxc::FontIcon>() : nullptr;
    if (icon) {
        icon.Glyph(glyph);
    }
}

void SetNamedIconColor(wux::UIElement const& root,
                       PCWSTR name,
                       winrt::Windows::UI::Color color) {
    auto element = FindNamedFrameworkElement(root, name);
    auto icon = element ? element.try_as<wuxc::FontIcon>() : nullptr;
    if (icon) {
        icon.Foreground(wuxm::SolidColorBrush(color));
    }
}

int HexDigit(wchar_t value) {
    if (value >= L'0' && value <= L'9') {
        return value - L'0';
    }
    if (value >= L'a' && value <= L'f') {
        return 10 + value - L'a';
    }
    if (value >= L'A' && value <= L'F') {
        return 10 + value - L'A';
    }
    return -1;
}

bool ParseHexColor(const std::wstring& value,
                   winrt::Windows::UI::Color& color) {
    if ((value.size() != 7 && value.size() != 9) || value[0] != L'#') {
        return false;
    }

    BYTE channels[4]{0xFF, 0, 0, 0};
    int channelCount = value.size() == 9 ? 4 : 3;
    int destinationOffset = value.size() == 9 ? 0 : 1;
    for (int i = 0; i < channelCount; ++i) {
        int high = HexDigit(value[1 + i * 2]);
        int low = HexDigit(value[2 + i * 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        channels[destinationOffset + i] = static_cast<BYTE>((high << 4) | low);
    }

    color = winrt::Windows::UI::Color{
        channels[0], channels[1], channels[2], channels[3]};
    return true;
}

wuxm::Brush MakeMediaGradientBrush(const winrt::Windows::UI::Color& left,
                                   const winrt::Windows::UI::Color& right) {
    wuxm::LinearGradientBrush brush;
    brush.StartPoint(wf::Point{0.0f, 0.5f});
    brush.EndPoint(wf::Point{1.0f, 0.5f});

    wuxm::GradientStop leftStop;
    leftStop.Color(left);
    leftStop.Offset(0.0);
    brush.GradientStops().Append(leftStop);

    wuxm::GradientStop rightStop;
    rightStop.Color(right);
    rightStop.Offset(1.0);
    brush.GradientStops().Append(rightStop);
    return brush;
}

wuxc::TextBlock MakeXMeterTextLine(const std::wstring& text,
                                   double width,
                                   double fontSize,
                                   const winrt::Windows::UI::Color& color,
                                   wux::TextAlignment alignment) {
    auto block = MakeText(text.c_str(), fontSize, 0xFF);
    block.Width(width);
    block.Height(12);
    block.LineHeight(12);
    block.FontFamily(wuxm::FontFamily(L"Segoe UI"));
    block.FontWeight(winrt::Windows::UI::Text::FontWeights::Normal());
    block.Foreground(wuxm::SolidColorBrush(color));
    block.HorizontalAlignment(wux::HorizontalAlignment::Center);
    block.VerticalAlignment(wux::VerticalAlignment::Center);
    block.TextAlignment(alignment);
    block.TextTrimming(wux::TextTrimming::None);
    return block;
}

wux::UIElement MakeXMeterTextPair(const std::wstring& top,
                                  const std::wstring& bottom,
                                  double width,
                                  double fontSize,
                                  const winrt::Windows::UI::Color& primary,
                                  const winrt::Windows::UI::Color& secondary,
                                  wux::TextAlignment alignment = wux::TextAlignment::Center) {
    wuxc::Grid grid;
    grid.Width(width);
    grid.Height(24);
    grid.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::RowDefinition firstRow;
    firstRow.Height(wux::GridLengthHelper::FromPixels(12));
    wuxc::RowDefinition secondRow;
    secondRow.Height(wux::GridLengthHelper::FromPixels(12));
    grid.RowDefinitions().Append(firstRow);
    grid.RowDefinitions().Append(secondRow);

    auto first = MakeXMeterTextLine(top, width, fontSize, primary, alignment);
    auto second = MakeXMeterTextLine(bottom, width, fontSize, secondary, alignment);
    wuxc::Grid::SetRow(second, 1);
    grid.Children().Append(first.as<wux::UIElement>());
    grid.Children().Append(second.as<wux::UIElement>());
    return grid;
}

wux::UIElement MakeXMeterRatePair(const std::wstring& top,
                                  const std::wstring& bottom,
                                  bool showTopArrow,
                                  bool showBottomArrow,
                                  const winrt::Windows::UI::Color& topColor,
                                  const winrt::Windows::UI::Color& bottomColor) {
    wuxc::Grid grid;
    grid.Width(93);
    grid.Height(24);
    wuxc::ColumnDefinition valueColumn;
    valueColumn.Width(wux::GridLengthHelper::FromPixels(69));
    wuxc::ColumnDefinition gapColumn;
    gapColumn.Width(wux::GridLengthHelper::FromPixels(3));
    wuxc::ColumnDefinition arrowColumn;
    arrowColumn.Width(wux::GridLengthHelper::FromPixels(21));
    grid.ColumnDefinitions().Append(valueColumn);
    grid.ColumnDefinitions().Append(gapColumn);
    grid.ColumnDefinitions().Append(arrowColumn);
    wuxc::RowDefinition topRow;
    topRow.Height(wux::GridLengthHelper::FromPixels(12));
    wuxc::RowDefinition bottomRow;
    bottomRow.Height(wux::GridLengthHelper::FromPixels(12));
    grid.RowDefinitions().Append(topRow);
    grid.RowDefinitions().Append(bottomRow);

    auto topValue = MakeXMeterTextLine(top, 69, 10, topColor, wux::TextAlignment::Right);
    auto bottomValue = MakeXMeterTextLine(bottom, 69, 10, bottomColor, wux::TextAlignment::Right);
    auto topArrow = MakeXMeterTextLine(showTopArrow ? L"\x25B2" : L"", 21, 10, topColor, wux::TextAlignment::Center);
    auto bottomArrow = MakeXMeterTextLine(showBottomArrow ? L"\x25BC" : L"", 21, 10, bottomColor, wux::TextAlignment::Center);
    topArrow.FontFamily(wuxm::FontFamily(L"Arial"));
    bottomArrow.FontFamily(wuxm::FontFamily(L"Arial"));
    wuxc::Grid::SetRow(bottomValue, 1);
    wuxc::Grid::SetRow(bottomArrow, 1);
    wuxc::Grid::SetColumn(topArrow, 2);
    wuxc::Grid::SetColumn(bottomArrow, 2);
    grid.Children().Append(topValue.as<wux::UIElement>());
    grid.Children().Append(bottomValue.as<wux::UIElement>());
    grid.Children().Append(topArrow.as<wux::UIElement>());
    grid.Children().Append(bottomArrow.as<wux::UIElement>());
    return grid;
}

wux::UIElement MakeXMeterVerticalBar(double primaryPercent,
                                     double secondaryPercent,
                                     double width,
                                     const winrt::Windows::UI::Color& primary,
                                     const winrt::Windows::UI::Color& secondary,
                                     const winrt::Windows::UI::Color& outlineColor) {
    constexpr double kBarHeight = 24.0;
    constexpr double kInnerHeight = 20.0;
    wuxc::Border outline;
    outline.Width(width);
    outline.Height(kBarHeight);
    outline.BorderThickness(wux::ThicknessHelper::FromUniformLength(1));
    outline.BorderBrush(wuxm::SolidColorBrush(outlineColor));
    outline.Background(MakeBrush(0x01, 0x00, 0x00, 0x00));
    outline.Padding(wux::ThicknessHelper::FromUniformLength(1));
    outline.VerticalAlignment(wux::VerticalAlignment::Center);

    wuxc::StackPanel fills;
    fills.Width(std::max(1.0, width - 4.0));
    fills.Height(kInnerHeight);
    fills.VerticalAlignment(wux::VerticalAlignment::Bottom);

    double first = std::clamp(primaryPercent, 0.0, 100.0);
    double second = std::clamp(secondaryPercent, 0.0, 100.0 - first);
    double emptyHeight = kInnerHeight * (100.0 - first - second) / 100.0;
    wuxc::Border empty;
    empty.Height(emptyHeight);
    empty.Background(MakeBrush(0x01, 0x00, 0x00, 0x00));
    fills.Children().Append(empty.as<wux::UIElement>());

    if (second > 0.0) {
        wuxc::Border secondaryFill;
        secondaryFill.Height(kInnerHeight * second / 100.0);
        secondaryFill.Background(wuxm::SolidColorBrush(secondary));
        fills.Children().Append(secondaryFill.as<wux::UIElement>());
    }
    if (first > 0.0) {
        wuxc::Border primaryFill;
        primaryFill.Height(kInnerHeight * first / 100.0);
        primaryFill.Background(wuxm::SolidColorBrush(primary));
        fills.Children().Append(primaryFill.as<wux::UIElement>());
    }
    outline.Child(fills);
    return outline;
}

wux::UIElement MakeXMeterPie(double percent,
                             double secondaryPercent,
                             double diameter,
                             const winrt::Windows::UI::Color& fillColor,
                             const winrt::Windows::UI::Color& secondaryColor,
                             const winrt::Windows::UI::Color& outlineColor) {
    double value = std::clamp(percent, 0.0, 100.0);
    double secondValue = std::clamp(secondaryPercent, 0.0, 100.0 - value);
    double radius = std::max(1.0, diameter / 2.0 - 1.0);
    float center = static_cast<float>(diameter / 2.0);

    wuxc::Grid pie;
    pie.Width(diameter);
    pie.Height(diameter);
    pie.VerticalAlignment(wux::VerticalAlignment::Center);

    if (value >= 99.999) {
        wuxs::Ellipse full;
        full.Width(diameter - 2.0);
        full.Height(diameter - 2.0);
        full.Fill(wuxm::SolidColorBrush(fillColor));
        full.HorizontalAlignment(wux::HorizontalAlignment::Center);
        full.VerticalAlignment(wux::VerticalAlignment::Center);
        pie.Children().Append(full.as<wux::UIElement>());
    } else if (value > 0.001) {
        constexpr double kPi = 3.14159265358979323846;
        double endAngle = (-90.0 + value * 3.6) * kPi / 180.0;
        wf::Point start{center, static_cast<float>(center - radius)};
        wf::Point end{
            static_cast<float>(center + radius * std::cos(endAngle)),
            static_cast<float>(center + radius * std::sin(endAngle))};

        wuxm::PathFigure figure;
        figure.StartPoint(wf::Point{center, center});
        figure.IsClosed(true);
        wuxm::LineSegment startLine;
        startLine.Point(start);
        figure.Segments().Append(startLine);
        wuxm::ArcSegment arc;
        arc.Point(end);
        arc.Size(wf::Size{static_cast<float>(radius), static_cast<float>(radius)});
        arc.IsLargeArc(value > 50.0);
        arc.SweepDirection(wuxm::SweepDirection::Clockwise);
        figure.Segments().Append(arc);

        wuxm::PathGeometry geometry;
        geometry.Figures().Append(figure);
        wuxs::Path wedge;
        wedge.Width(diameter);
        wedge.Height(diameter);
        wedge.Data(geometry);
        wedge.Fill(wuxm::SolidColorBrush(fillColor));
        pie.Children().Append(wedge.as<wux::UIElement>());
    }

    if (secondValue > 0.001) {
        constexpr double kPi = 3.14159265358979323846;
        double startAngle = (-90.0 + value * 3.6) * kPi / 180.0;
        double endAngle = (-90.0 + (value + secondValue) * 3.6) * kPi / 180.0;
        wf::Point start{static_cast<float>(center + radius * std::cos(startAngle)),
                        static_cast<float>(center + radius * std::sin(startAngle))};
        wf::Point end{static_cast<float>(center + radius * std::cos(endAngle)),
                      static_cast<float>(center + radius * std::sin(endAngle))};
        wuxm::PathFigure figure;
        figure.StartPoint(wf::Point{center, center});
        figure.IsClosed(true);
        wuxm::LineSegment startLine;
        startLine.Point(start);
        figure.Segments().Append(startLine);
        wuxm::ArcSegment arc;
        arc.Point(end);
        arc.Size(wf::Size{static_cast<float>(radius), static_cast<float>(radius)});
        arc.IsLargeArc(secondValue > 50.0);
        arc.SweepDirection(wuxm::SweepDirection::Clockwise);
        figure.Segments().Append(arc);
        wuxm::PathGeometry geometry;
        geometry.Figures().Append(figure);
        wuxs::Path wedge;
        wedge.Width(diameter);
        wedge.Height(diameter);
        wedge.Data(geometry);
        wedge.Fill(wuxm::SolidColorBrush(secondaryColor));
        pie.Children().Append(wedge.as<wux::UIElement>());
    }

    wuxs::Ellipse outline;
    outline.Width(diameter - 1.0);
    outline.Height(diameter - 1.0);
    outline.Fill(MakeBrush(0x01, 0x00, 0x00, 0x00));
    outline.Stroke(wuxm::SolidColorBrush(outlineColor));
    outline.StrokeThickness(1);
    outline.HorizontalAlignment(wux::HorizontalAlignment::Center);
    outline.VerticalAlignment(wux::VerticalAlignment::Center);
    pie.Children().Append(outline.as<wux::UIElement>());
    return pie;
}

void ClearXMeterChildren(const wuxc::StackPanel& meter) {
    auto children = meter.Children();
    while (children.Size() > 0) {
        children.RemoveAt(children.Size() - 1);
    }
}

void UpdateSystemMetricPanel(wux::UIElement const& root,
                             const std::wstring& widgetId) {
    SystemMetricSnapshot snapshot = ReadSystemMetricSnapshot(widgetId);
    SystemMetricWidgetSettings settings = ReadSystemMetricWidgetSettings(widgetId);
    COLORREF systemHighlight = GetSysColor(COLOR_HIGHLIGHT);
    winrt::Windows::UI::Color accent{
        0xFF, GetRValue(systemHighlight), GetGValue(systemHighlight), GetBValue(systemHighlight)};
    auto resolveColor = [&accent](const std::wstring& value,
                                  const winrt::Windows::UI::Color& fallback) {
        if (_wcsicmp(value.c_str(), L"systemAccent") == 0) return accent;
        auto result = fallback;
        ParseHexColor(value, result);
        return result;
    };
    auto primary = resolveColor(settings.firstColor, accent);
    auto secondary = resolveColor(settings.secondColor,
        winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});
    auto outline = resolveColor(settings.outlineColor,
        winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});

    std::wstring primaryText = L"--";
    std::wstring secondaryText;
    double primaryRatio = 0;
    double secondaryRatio = 0;

    if (snapshot.loaded && widgetId == L"system-cpu") {
        primaryRatio = snapshot.totalPercent;
        if (settings.separateUtilization) {
            primaryRatio = snapshot.userPercent;
            secondaryRatio = snapshot.kernelPercent;
            primaryText = std::to_wstring(static_cast<int>(std::round(snapshot.userPercent))) + L"%";
            secondaryText = std::to_wstring(static_cast<int>(std::round(snapshot.kernelPercent))) + L"%";
        } else {
            primaryText = std::to_wstring(static_cast<int>(std::round(snapshot.totalPercent))) + L"%";
        }
    } else if (snapshot.loaded && widgetId == L"system-storage") {
        primaryText = FormatMetricRate(snapshot.primaryRate);
        secondaryText = FormatMetricRate(snapshot.secondaryRate);
        primaryRatio = snapshot.primaryPercent;
        secondaryRatio = snapshot.secondaryPercent;
    } else if (snapshot.loaded && widgetId == L"system-network") {
        primaryText = FormatMetricRate(snapshot.secondaryRate); // Send, top.
        secondaryText = FormatMetricRate(snapshot.primaryRate); // Receive, bottom.
        double manualCapacity = settings.bandwidthKiloBytes * 1000.0;
        double sendCapacity = settings.autoBandwidth
            ? static_cast<double>(snapshot.sendLinkSpeedBitsPerSecond) / 8.0
            : manualCapacity;
        double receiveCapacity = settings.autoBandwidth
            ? static_cast<double>(snapshot.receiveLinkSpeedBitsPerSecond) / 8.0
            : manualCapacity;
        primaryRatio = sendCapacity > 0 ? snapshot.secondaryRate * 100.0 / sendCapacity : 0;
        secondaryRatio = receiveCapacity > 0 ? snapshot.primaryRate * 100.0 / receiveCapacity : 0;
    } else if (snapshot.loaded && widgetId == L"system-memory") {
        primaryText = std::to_wstring(static_cast<int>(std::round(snapshot.usedPercent))) + L"%";
        primaryRatio = snapshot.usedPercent;
    }
    primaryRatio = std::isfinite(primaryRatio) ? std::clamp(primaryRatio, 0.0, 100.0) : 0;
    secondaryRatio = std::isfinite(secondaryRatio) ? std::clamp(secondaryRatio, 0.0, 100.0) : 0;

    auto meterElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsSystemMeter");
    auto meter = meterElement ? meterElement.try_as<wuxc::StackPanel>() : nullptr;
    if (!meter) return;
    double widgetWidth = SystemMetricWidgetWidth(widgetId);
    if (auto rootElement = root.try_as<wux::FrameworkElement>()) {
        rootElement.Width(widgetWidth);
        rootElement.Margin(wux::ThicknessHelper::FromLengths(0, 0, 0, 0));
    }
    if (auto panel = FindNamedFrameworkElement(root, L"TaskbarWidgetsSystemPanel")) {
        panel.Width(widgetWidth);
        panel.Height(24);
    }
    meter.Width(widgetWidth);
    meter.Height(24);

    std::wstring signature = widgetId + L"|" + settings.displayMode + L"|" +
                             settings.firstColor + L"|" + settings.secondColor + L"|" + settings.outlineColor + L"|" +
                             primaryText + L"|" + secondaryText;
    if (widgetId == L"system-cpu" && settings.showIndividualCores) {
        for (const auto& core : snapshot.cores) {
            signature += L"|" + core.id + L":" +
                         std::to_wstring(static_cast<int>(std::round(core.percent))) + L":" +
                         std::to_wstring(static_cast<int>(std::round(core.userPercent))) + L":" +
                         std::to_wstring(static_cast<int>(std::round(core.kernelPercent)));
        }
    }
    auto tag = meter.Tag();
    if (tag && std::wstring(winrt::unbox_value_or<winrt::hstring>(tag, L"").c_str()) == signature) {
        return;
    }
    meter.Tag(winrt::box_value(winrt::hstring(signature)));
    ClearXMeterChildren(meter);

    if (!snapshot.loaded) {
        meter.Children().Append(MakeXMeterTextPair(L"--", L"", std::max(8.0, widgetWidth), 10, primary, secondary));
        return;
    }

    bool perCore = widgetId == L"system-cpu" && settings.showIndividualCores &&
                   !snapshot.cores.empty();
    std::vector<CpuCoreMetricSnapshot> cores = snapshot.cores;
    if (perCore && settings.combineLogicalCores) {
        std::vector<CpuCoreMetricSnapshot> combined;
        for (size_t i = 0; i < cores.size(); i += 2) {
            auto merged = cores[i];
            if (i + 1 < cores.size()) {
                merged.id += L"+" + cores[i + 1].id;
                merged.percent = (merged.percent + cores[i + 1].percent) / 2.0;
                merged.userPercent = (merged.userPercent + cores[i + 1].userPercent) / 2.0;
                merged.kernelPercent = (merged.kernelPercent + cores[i + 1].kernelPercent) / 2.0;
            }
            combined.push_back(merged);
        }
        cores = std::move(combined);
    }

    if (settings.displayMode == L"text") {
        if (perCore) {
            for (size_t index = 0; index < cores.size(); ++index) {
                const auto& core = cores[index];
                double user = core.userPercent;
                double kernel = core.kernelPercent;
                if (user <= 0.0 && kernel <= 0.0) user = core.percent;
                std::wstring top = std::to_wstring(static_cast<int>(std::round(
                    settings.separateUtilization ? kernel : core.percent))) + L"%";
                std::wstring bottom = settings.separateUtilization
                    ? std::to_wstring(static_cast<int>(std::round(user))) + L"%"
                    : L"";
                meter.Children().Append(MakeXMeterTextPair(
                    top, bottom, 44, 10, secondary, primary));
            }
        } else if (widgetId == L"system-storage") {
            meter.Children().Append(MakeXMeterRatePair(
                primaryText, secondaryText, snapshot.primaryRate >= 1000.0,
                snapshot.secondaryRate >= 1000.0, primary, secondary));
        } else if (widgetId == L"system-network") {
            meter.Children().Append(MakeXMeterRatePair(
                primaryText, secondaryText, snapshot.secondaryRate >= 1000.0,
                snapshot.primaryRate >= 1000.0, primary, secondary));
        } else {
            meter.Children().Append(MakeXMeterTextPair(
                primaryText, secondaryText, 44, 10.0,
                widgetId == L"system-cpu" && settings.separateUtilization ? secondary : primary,
                primary));
        }
    } else if (settings.displayMode == L"bar") {
        if (perCore) {
            for (size_t index = 0; index < cores.size(); ++index) {
                const auto& core = cores[index];
                double user = core.userPercent;
                double kernel = core.kernelPercent;
                if (user <= 0.0 && kernel <= 0.0) user = core.percent;
                meter.Children().Append(MakeXMeterVerticalBar(
                    settings.separateUtilization ? user : core.percent,
                    settings.separateUtilization ? kernel : 0.0,
                    8, primary, secondary, outline));
            }
        } else {
            meter.Children().Append(MakeXMeterVerticalBar(
                primaryRatio, widgetId == L"system-memory" ? 0.0 : secondaryRatio,
                8, primary, secondary, outline));
        }
    } else {
        if (perCore) {
            for (size_t index = 0; index < cores.size(); ++index) {
                double user = cores[index].userPercent;
                double kernel = cores[index].kernelPercent;
                if (user <= 0.0 && kernel <= 0.0) user = cores[index].percent;
                meter.Children().Append(MakeXMeterPie(
                    settings.separateUtilization ? user : cores[index].percent,
                    settings.separateUtilization ? kernel : 0.0,
                    24, primary, secondary, outline));
            }
        } else {
            meter.Children().Append(MakeXMeterPie(
                primaryRatio, widgetId == L"system-memory" ? 0.0 : secondaryRatio,
                24, primary, secondary, outline));
        }
    }
}

void ApplyMediaTheme(wux::UIElement const& root,
                     bool darkMode,
                     bool active,
                     const MediaSnapshot& media) {
    if (darkMode) {
        winrt::Windows::UI::Color left{0xFF, 0x0F, 0x17, 0x2A};
        winrt::Windows::UI::Color right{0xFF, 0x11, 0x18, 0x27};
        bool hasAdaptive =
            ParseHexColor(media.backgroundLeftColor, left) &&
            ParseHexColor(media.backgroundRightColor, right);
        if (hasAdaptive) {
            SetNamedBorderFill(root, L"TaskbarWidgetsMediaPanel",
                               MakeMediaGradientBrush(left, right));
        } else {
            SetNamedBorderFill(root, L"TaskbarWidgetsMediaPanel",
                               MakeBrush(0xFF, 0x0F, 0x17, 0x2A));
        }

        winrt::Windows::UI::Color accent{0xFF, 0x22, 0xD3, 0xEE};
        ParseHexColor(media.accentColor, accent);
        SetNamedTextColor(root, L"TaskbarWidgetsMediaTitle",
                          winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
        SetNamedTextColor(root, L"TaskbarWidgetsMediaTitleClone",
                          winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
        SetNamedTextColor(root, L"TaskbarWidgetsMediaArtist",
                          winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
        if (active) {
            SetNamedBorderFill(root, L"TaskbarWidgetsMediaPlayButton",
                               wuxm::SolidColorBrush(accent));
        } else {
            SetNamedBorderFill(root, L"TaskbarWidgetsMediaPlayButton",
                               MakeBrush(0xFF, 0x33, 0x41, 0x55));
        }
        SetNamedIconColor(root, L"TaskbarWidgetsMediaPlayIcon",
                          active ? winrt::Windows::UI::Color{0xFF, 0x08, 0x17, 0x1F}
                                 : winrt::Windows::UI::Color{0xFF, 0xCB, 0xD5, 0xE1});
        return;
    }

    SetNamedBorderFill(root, L"TaskbarWidgetsMediaPanel",
                       MakeBrush(0xFF, 0xFA, 0xFA, 0xF8));
    SetNamedTextColor(root, L"TaskbarWidgetsMediaTitle",
                      winrt::Windows::UI::Color{0xFF, 0x00, 0x00, 0x00});
    SetNamedTextColor(root, L"TaskbarWidgetsMediaTitleClone",
                      winrt::Windows::UI::Color{0xFF, 0x00, 0x00, 0x00});
    SetNamedTextColor(root, L"TaskbarWidgetsMediaArtist",
                      winrt::Windows::UI::Color{0xF0, 0x00, 0x00, 0x00});
    SetNamedBorderFill(root, L"TaskbarWidgetsMediaPlayButton",
                       active ? MakeBrush(0xFF, 0x00, 0x00, 0x00)
                              : MakeBrush(0x55, 0x00, 0x00, 0x00));
    SetNamedIconColor(root, L"TaskbarWidgetsMediaPlayIcon",
                      winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});
}

void ApplySteamDownloadTheme(wux::UIElement const& root,
                             bool active,
                             bool hasCoverArt) {
    SetNamedBorderFill(root, L"TaskbarWidgetsSteamPanel",
                       MakeBrush(0xFF, 0x0B, 0x12, 0x20));
    if (!hasCoverArt) {
        SetNamedBorderFill(root, L"TaskbarWidgetsSteamBackdrop",
                           MakeBrush(0xFF, 0x0B, 0x12, 0x20));
        SetNamedBorderFill(root, L"TaskbarWidgetsSteamCover",
                           MakeMediaGradientBrush(
                               winrt::Windows::UI::Color{0xFF, 0x1B, 0x28, 0x38},
                               winrt::Windows::UI::Color{0xFF, 0x2A, 0x47, 0x5E}));
    }
    SetNamedTextColor(root, L"TaskbarWidgetsSteamTitle",
                      winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
    SetNamedTextColor(root, L"TaskbarWidgetsSteamTitleClone",
                      winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
    SetNamedTextColor(root, L"TaskbarWidgetsSteamDetail",
                      active ? winrt::Windows::UI::Color{0xFF, 0xCB, 0xD5, 0xE1}
                             : winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
    SetNamedTextColor(root, L"TaskbarWidgetsSteamMetric",
                      active ? winrt::Windows::UI::Color{0xFF, 0x66, 0xC0, 0xF4}
                             : winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
}

void SetDiscordPanelBackground(wux::UIElement const& root, bool enabled) {
    auto element = FindNamedFrameworkElement(root, L"TaskbarWidgetsDiscordPanel");
    auto border = element ? element.try_as<wuxc::Border>() : nullptr;
    if (!border) {
        return;
    }

    border.Background(enabled ? MakeBrush(0xF5, 0x11, 0x11, 0x11)
                              : MakeBrush(0x00, 0x00, 0x00, 0x00));
    border.BorderBrush(enabled ? MakeBrush(0xFF, 0x2D, 0x2D, 0x2D)
                               : MakeBrush(0x00, 0x00, 0x00, 0x00));
    border.BorderThickness(wux::ThicknessHelper::FromUniformLength(enabled ? 1 : 0));
}

void SetNamedVisibility(wux::UIElement const& root,
                        PCWSTR name,
                        wux::Visibility visibility) {
    auto element = FindNamedFrameworkElement(root, name);
    if (element && element.Visibility() != visibility) {
        element.Visibility(visibility);
    }
}

bool IsNamedVisible(wux::UIElement const& root, PCWSTR name) {
    auto element = FindNamedFrameworkElement(root, name);
    return element && element.Visibility() == wux::Visibility::Visible;
}

void SetRateLimitProgressVisual(wux::UIElement const& root,
                                double usedPercent) {
    constexpr double barWidth = 126.0;
    double remaining = GetRemainingPercent(usedPercent);
    auto color = GetRateLimitColor(remaining);

    SetNamedTextColor(root, L"TaskbarWidgetsTitle", color);

    auto fillElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsLimitBarFill");
    if (fillElement) {
        fillElement.Width(remaining < 0 ? 0 : barWidth * remaining / 100.0);
        auto fill = fillElement.try_as<wuxc::Border>();
        if (fill) {
            fill.Background(wuxm::SolidColorBrush(color));
        }
    }

    auto trackElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsLimitBarTrack");
    if (trackElement) {
        auto track = trackElement.try_as<wuxc::Grid>();
        if (track) {
            track.Background(MakeBrush(0x34, 0x94, 0xA3, 0xB8));
        }
    }
}

void SetSteamDownloadProgressVisual(wux::UIElement const& root,
                                    double progressPercent,
                                    bool active) {
    constexpr double barWidth = 38.0;
    double value = std::clamp(progressPercent, 0.0, 100.0);
    auto fillElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsSteamProgressFill");
    if (fillElement) {
        fillElement.Width(active ? barWidth * value / 100.0 : 0.0);
        auto fill = fillElement.try_as<wuxc::Border>();
        if (fill) {
            fill.Background(active ? MakeBrush(0xFF, 0x66, 0xC0, 0xF4)
                                   : MakeBrush(0x77, 0x94, 0xA3, 0xB8));
        }
    }

    SetNamedOpacity(root, L"TaskbarWidgetsSteamProgressTrack", active ? 1.0 : 0.35);
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
    SetTaskStateVisualNamed(root, L"TaskbarWidgetsStateIcon",
                            L"TaskbarWidgetsStateText", state, label);
}

void UpdateExpandedTaskRows(wux::UIElement const& root,
                            const std::vector<std::wstring>& titles,
                            const std::wstring& state,
                            const std::wstring& label) {
    constexpr PCWSTR rowNames[] = {
        L"TaskbarWidgetsExpandedRow0",
        L"TaskbarWidgetsExpandedRow1",
        L"TaskbarWidgetsExpandedRow2",
        L"TaskbarWidgetsExpandedRow3",
        L"TaskbarWidgetsExpandedRow4"};
    constexpr PCWSTR titleNames[] = {
        L"TaskbarWidgetsExpandedTitle0",
        L"TaskbarWidgetsExpandedTitle1",
        L"TaskbarWidgetsExpandedTitle2",
        L"TaskbarWidgetsExpandedTitle3",
        L"TaskbarWidgetsExpandedTitle4"};
    constexpr PCWSTR iconNames[] = {
        L"TaskbarWidgetsExpandedIcon0",
        L"TaskbarWidgetsExpandedIcon1",
        L"TaskbarWidgetsExpandedIcon2",
        L"TaskbarWidgetsExpandedIcon3",
        L"TaskbarWidgetsExpandedIcon4"};
    constexpr PCWSTR stateNames[] = {
        L"TaskbarWidgetsExpandedState0",
        L"TaskbarWidgetsExpandedState1",
        L"TaskbarWidgetsExpandedState2",
        L"TaskbarWidgetsExpandedState3",
        L"TaskbarWidgetsExpandedState4"};

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

void ApplyWidgetOffset(wux::UIElement const& root,
                       const WidgetRuntimeSettings& settings) {
    double offset = static_cast<double>(
        std::clamp(settings.widgetMoveX, -640LL, 640LL));
    wuxm::TranslateTransform transform;
    transform.X(offset);
    transform.Y(0);
    root.RenderTransform(transform);
}

void SetExpandedMode(wux::UIElement const& root, bool expanded) {
    std::wstring activeDesign = GetWidgetDesignFromRoot(root);
    if (activeDesign != L"codex-status") {
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            if (activeDesign == L"media-player" ||
                activeDesign == L"steam-download") {
                rootElement.Width(230);
                rootElement.Height(44);
            } else if (activeDesign == L"discord-voice") {
                rootElement.Width(196);
                rootElement.Height(36);
            } else if (activeDesign.rfind(L"system-", 0) == 0) {
                rootElement.Width(176);
                rootElement.Height(36);
            } else {
                rootElement.Width(240);
                rootElement.Height(36);
            }
        }
        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        return;
    }

    if (expanded && GetAntigravityProjectTitles().size() <= 1) {
        expanded = false;
    }

    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement) {
        rootElement.Width(expanded ? 230 : 184);
        rootElement.Height(expanded ? 72 : 36);
    }

    SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                       expanded ? wux::Visibility::Collapsed
                                : wux::Visibility::Visible);
    SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                       expanded ? wux::Visibility::Visible
                                 : wux::Visibility::Collapsed);
}

void UpdateTaskbarWidgetsWidgetRoot(wux::UIElement const& root,
                                  const WidgetInstanceRuntime& instance) {
    wuxm::TranslateTransform transform;
    transform.X(0);
    transform.Y(0);
    root.RenderTransform(transform);
    root.Visibility(wux::Visibility::Visible);
    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement) {
        rootElement.Tag(winrt::box_value(winrt::hstring(instance.designId)));
    }

    std::wstring activeDesign = instance.designId;
    bool isSystemMetric = activeDesign == L"system-cpu" ||
                          activeDesign == L"system-storage" ||
                          activeDesign == L"system-network" ||
                          activeDesign == L"system-memory";
    SetNamedVisibility(root, L"TaskbarWidgetsSystemPanel",
                       isSystemMetric ? wux::Visibility::Visible
                                      : wux::Visibility::Collapsed);
    if (isSystemMetric) {
        if (rootElement) {
            rootElement.Width(SystemMetricWidgetWidth(activeDesign));
            rootElement.Height(24);
            rootElement.Margin(wux::ThicknessHelper::FromLengths(0, 0, 0, 0));
        }
        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel", wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel", wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel", wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel", wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel", wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel", wux::Visibility::Collapsed);
        UpdateSystemMetricPanel(root, activeDesign);
        return;
    }
    if (activeDesign == L"media-player") {
        MediaSnapshot media = ReadMediaSnapshot();
        WidgetRuntimeSettings settings = ReadWidgetRuntimeSettings();
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            rootElement.Width(230);
            rootElement.Height(44);
        }

        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel",
                           wux::Visibility::Visible);
        bool hasMediaText = !Trim(media.title).empty() || !Trim(media.artist).empty();
        SetMediaTitleText(root, hasMediaText
                                    ? (media.title.empty() ? L"Unknown media" : media.title)
                                    : L"No media");
        SetNamedText(root, L"TaskbarWidgetsMediaArtist",
                     hasMediaText
                         ? (media.artist.empty() ? L"Unknown artist" : media.artist)
                         : L"Open a player");
        SetNamedIconGlyph(root, L"TaskbarWidgetsMediaPlayIcon",
                          media.playing ? L"\xE769" : L"\xE768");
        SetNamedOpacity(root, L"TaskbarWidgetsMediaPlayIcon", media.active ? 1.0 : 0.45);
        ApplyMediaTheme(root, settings.mediaDarkMode,
                        media.active || hasMediaText, media);
        SetNamedBorderImageBackground(root, L"TaskbarWidgetsMediaCover",
                                      media.coverPath);
        return;
    }

    if (activeDesign == L"steam-download") {
        SteamDownloadSnapshot steam = ReadSteamDownloadSnapshot();
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            rootElement.Width(230);
            rootElement.Height(44);
        }

        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel",
                           wux::Visibility::Visible);

        std::wstring title = L"Steam Downloads";
        if (steam.loaded && !Trim(steam.title).empty()) {
            title = steam.title;
        }

        std::wstring detail = L"Veri bekleniyor";
        if (steam.loaded) {
            if (steam.active) {
                detail = !Trim(steam.detail).empty()
                             ? steam.detail
                             : (!Trim(steam.subtitle).empty() ? steam.subtitle
                                                              : L"Downloading");
            } else {
                detail = steam.steamRunning ? L"Indirme yok" : L"Steam kapali";
            }
        }

        std::wstring metric = L"--";
        if (steam.loaded && steam.active) {
            int percent = static_cast<int>(std::round(steam.progressPercent));
            metric = std::to_wstring(std::clamp(percent, 0, 100)) + L"%";
        }

        SetSteamTitleText(root, title);
        SetNamedText(root, L"TaskbarWidgetsSteamDetail", detail);
        SetNamedText(root, L"TaskbarWidgetsSteamMetric", metric);
        bool hasCoverArt = steam.loaded && steam.active &&
                           !steam.coverPath.empty() &&
                           FileExists(steam.coverPath);
        ApplySteamDownloadTheme(root, steam.loaded && steam.active, hasCoverArt);
        SetSteamDownloadProgressVisual(root, steam.progressPercent,
                                       steam.loaded && steam.active);
        if (hasCoverArt) {
            SetNamedBorderImageBackground(root, L"TaskbarWidgetsSteamCover",
                                          steam.coverPath);
            SetNamedBorderImageBackground(root, L"TaskbarWidgetsSteamBackdrop",
                                          steam.coverPath);
        }
        return;
    }

    if (activeDesign == L"discord-voice") {
        WidgetRuntimeSettings settings = ReadWidgetRuntimeSettings();
        DiscordVoiceSnapshot discord = ReadDiscordVoiceSnapshot();
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            rootElement.Width(196);
            rootElement.Height(36);
        }

        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel",
                           wux::Visibility::Visible);
        SetDiscordPanelBackground(root, settings.discordBackgroundEnabled);

        for (int i = 0; i < 5; ++i) {
            std::wstring frameName = L"TaskbarWidgetsDiscordAvatarFrame" + std::to_wstring(i);
            std::wstring avatarName = L"TaskbarWidgetsDiscordAvatarEllipse" + std::to_wstring(i);
            if (i < static_cast<int>(discord.users.size())) {
                const auto& user = discord.users[i];
                SetNamedVisibility(root, frameName.c_str(), wux::Visibility::Visible);
                std::wstring avatarPath = user.speaking && !user.animatedAvatarPath.empty()
                                              ? user.animatedAvatarPath
                                              : user.avatarPath;
                SetNamedEllipseImageFill(root, avatarName.c_str(), avatarPath);
                SetNamedOpacity(root, avatarName.c_str(), user.speaking ? 1.0 : 0.38);
                SetNamedBorderVisual(root, frameName.c_str(), user.speaking);
            } else {
                SetNamedVisibility(root, frameName.c_str(), wux::Visibility::Collapsed);
            }
        }
        return;
    }

    if (activeDesign == L"weather-static") {
        WeatherSnapshot weather = ReadWeatherSnapshot();
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            rootElement.Width(240);
            rootElement.Height(36);
        }

        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel",
                           wux::Visibility::Visible);
        SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel",
                           wux::Visibility::Collapsed);

        if (!weather.loaded) {
            SetNamedText(root, L"TaskbarWidgetsWeatherCity", L"Izmir");
            SetNamedText(root, L"TaskbarWidgetsWeatherCondition", L"--:-- • --/--");
            SetNamedText(root, L"TaskbarWidgetsWeatherTemp", L"--\x00B0");
            SetNamedImageSource(root, L"TaskbarWidgetsWeatherIcon",
                                L"weather\\rain.png");
            return;
        }

        SetNamedText(root, L"TaskbarWidgetsWeatherCity", weather.location);
        SetNamedText(root, L"TaskbarWidgetsWeatherCondition",
                     FormatWeatherDate(weather.updatedAtUnix));
        SetNamedText(root, L"TaskbarWidgetsWeatherTemp",
                     FormatTemperature(weather.temperature));
        SetNamedImageSource(root, L"TaskbarWidgetsWeatherIcon",
                            WeatherIconAsset(weather.weatherCode));
        return;
    }

    SetNamedVisibility(root, L"TaskbarWidgetsWeatherPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarWidgetsDiscordPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarWidgetsMediaPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarWidgetsSteamPanel",
                       wux::Visibility::Collapsed);

    CodexStatusSnapshot snapshot = ReadCodexStatusSnapshot();
    if (!snapshot.loaded) {
        SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                           wux::Visibility::Visible);
        SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedText(root, L"TaskbarWidgetsTitle", L"Antigravity");
        SetRateLimitProgressVisual(root, -1);
        SetTaskStateVisual(root, L"IDLE", L"IDLE");
        UpdateExpandedTaskRows(root, {}, L"IDLE", L"IDLE");
        SetNamedText(root, L"TaskbarWidgetsLimit", L"--");
        SetNamedText(root, L"TaskbarWidgetsReset", L"--");
        SetNamedText(root, L"TaskbarWidgetsWeek", L"--");
        SetNamedText(root, L"TaskbarWidgetsTokens", L"--");
        return;
    }

    std::vector<std::wstring> antigravityTitles = GetAntigravityProjectTitles();
    if (auto codexRoot = root.try_as<wux::FrameworkElement>()) {
        codexRoot.Width(184);
        codexRoot.Height(36);
    }
    SetNamedVisibility(root, L"TaskbarWidgetsCompactPanel",
                       wux::Visibility::Visible);
    SetNamedVisibility(root, L"TaskbarWidgetsExpandedPanel",
                       wux::Visibility::Collapsed);

    if (!antigravityTitles.empty()) {
        uint32_t titleIndex = 0;
        if (antigravityTitles.size() > 1) {
            titleIndex = static_cast<uint32_t>(
                (CurrentUnixTime() / 5) % antigravityTitles.size());
        }

        SetNamedText(root, L"TaskbarWidgetsTitle",
                     antigravityTitles[titleIndex]);
    } else {
        SetNamedText(root, L"TaskbarWidgetsTitle", L"Antigravity");
    }

    SetTaskStateVisual(root, snapshot.taskState, snapshot.taskLabel);
    UpdateExpandedTaskRows(root, antigravityTitles, snapshot.taskState,
                           snapshot.taskLabel);
    SetRateLimitProgressVisual(root, snapshot.primaryUsedPercent);

    SetNamedText(root, L"TaskbarWidgetsLimit",
                 FormatRemainingPercent(snapshot.primaryUsedPercent));
    SetNamedText(root, L"TaskbarWidgetsReset",
                 FormatReset(snapshot.primaryResetsAtUnix));
    SetNamedText(root, L"TaskbarWidgetsWeek",
                 FormatRemainingPercent(snapshot.secondaryUsedPercent));
    SetNamedText(root, L"TaskbarWidgetsTokens",
                 FormatTokenCount(snapshot.tokens30d));
}

std::wstring BuildWidgetSignature(
    const std::vector<WidgetInstanceRuntime>& widgets) {
    std::wstring signature;
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        signature += widget.id;
        signature += L":";
        signature += widget.designId;
        signature += L":";
        signature += std::to_wstring(widget.moveX);
        signature += L":";
        signature += std::to_wstring(widget.positionPct);
        signature += L";";
    }
    return signature;
}

void ClearPanelChildren(wuxc::Panel const& panel) {
    auto children = panel.Children();
    while (children.Size() > 0) {
        children.RemoveAt(children.Size() - 1);
    }
}

void RebuildWidgetHostIfNeeded(wux::UIElement const& root,
                               const std::vector<WidgetInstanceRuntime>& widgets) {
    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (!rootElement) {
        return;
    }

    std::wstring signature = BuildWidgetSignature(widgets);
    auto currentTag = rootElement.Tag();
    auto currentSignature =
        currentTag ? winrt::unbox_value_or<winrt::hstring>(currentTag, L"") : L"";
    if (std::wstring(currentSignature.c_str()) == signature) {
        return;
    }

    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsWidgetHost");
    auto host = hostElement.try_as<wuxc::Canvas>();
    if (!host) {
        return;
    }

    g_widgetDragOverrides.clear();
    ClearPanelChildren(host);
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        auto childRoot = MakeTaskbarWidgetsWidgetRoot(widget);
        host.Children().Append(childRoot.as<wux::UIElement>());
    }

    rootElement.Tag(winrt::box_value(winrt::hstring(signature)));
}

double WidgetDesignWidth(const std::wstring& designId) {
    if (designId.rfind(L"system-", 0) == 0) {
        return SystemMetricWidgetWidth(designId);
    }
    if (designId == L"media-player" ||
        designId == L"steam-download") {
        return 230.0;
    }
    if (designId == L"discord-voice") {
        return 196.0;
    }
    if (designId == L"weather-static") {
        return 240.0;
    }
    return 184.0;
}

double WidgetDesignHeight(const std::wstring& designId) {
    if (designId.rfind(L"system-", 0) == 0) {
        return 24.0;
    }
    if (designId == L"media-player" ||
        designId == L"steam-download") {
        return 44.0;
    }
    return 36.0;
}

struct WidgetLayoutBounds {
    double minLeft = 0;
    double maxRight = 0;
    double width = 1;
};

WidgetLayoutBounds ComputeWidgetLayoutBounds(
    const std::vector<WidgetInstanceRuntime>& widgets) {
    WidgetLayoutBounds bounds;
    bool first = true;
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        double width = WidgetDesignWidth(widget.designId);
        double right = static_cast<double>(widget.moveX);
        double left = right - width;
        if (first) {
            bounds.minLeft = std::min(0.0, left);
            bounds.maxRight = std::max(0.0, right);
            first = false;
        } else {
            bounds.minLeft = std::min(bounds.minLeft, left);
            bounds.maxRight = std::max(bounds.maxRight, right);
        }
    }

    bounds.width = std::max(1.0, bounds.maxRight - bounds.minLeft);
    return bounds;
}

void ApplyWidgetCanvasLayout(wux::UIElement const& root,
                             const std::vector<WidgetInstanceRuntime>& widgets) {
    auto rootElement = root.try_as<wux::FrameworkElement>();
    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsWidgetHost");
    auto host = hostElement.try_as<wuxc::Canvas>();
    if (!rootElement || !host) {
        return;
    }

    WidgetLayoutBounds bounds = ComputeWidgetLayoutBounds(widgets);
    double canvasWidth = rootElement.ActualWidth();
    if (canvasWidth < 10.0 && rootElement.Width() > 10.0) {
        canvasWidth = rootElement.Width();
    }
    if (canvasWidth < 10.0) {
        canvasWidth = bounds.width;
        rootElement.Width(canvasWidth);
    }
    rootElement.Height(48);
    host.Width(canvasWidth);
    host.Height(48);

    uint32_t childIndex = 0;
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        if (childIndex >= host.Children().Size()) {
            break;
        }

        auto child = host.Children().GetAt(childIndex);
        double width = WidgetDesignWidth(widget.designId);
        double height = WidgetDesignHeight(widget.designId);
        double left = 0;
        if (widget.positionPct >= 0) {
            left = taskbar_widgets::IndependentLeftForWidget(
                taskbar_widgets::PositionedWidget{
                    width, static_cast<int>(widget.positionPct),
                    static_cast<int>(widget.moveX)},
                canvasWidth);
        } else {
            left = canvasWidth + static_cast<double>(widget.moveX) - width;
        }
        auto overrideIt = g_widgetDragOverrides.find(WidgetElementKey(child));
        if (overrideIt != g_widgetDragOverrides.end()) {
            auto& dragOverride = overrideIt->second;
            const bool configurationApplied =
                dragOverride.awaitingConfig &&
                widget.positionPct == dragOverride.anchorPercent &&
                widget.moveX == dragOverride.offsetPx;
            const bool expired = dragOverride.awaitingConfig &&
                std::chrono::steady_clock::now() >= dragOverride.expiresAt;
            if (configurationApplied || expired) {
                g_widgetDragOverrides.erase(overrideIt);
            } else {
                left = dragOverride.left;
            }
        }
        left = std::clamp(left, 0.0, std::max(0.0, canvasWidth - width));
        double top = std::max(0.0, (48.0 - height) / 2.0);
        wuxc::Canvas::SetLeft(child, left);
        wuxc::Canvas::SetTop(child, top);
        ++childIndex;
    }
}

void UpdateTaskbarWidgetsRoot(wux::UIElement const& root) {
    std::vector<WidgetInstanceRuntime> widgets = ReadWidgetInstances();
    bool hasEnabledWidget = std::any_of(
        widgets.begin(), widgets.end(),
        [](const WidgetInstanceRuntime& widget) { return widget.enabled; });

    if (!hasEnabledWidget) {
        if (auto rootElement = root.try_as<wux::FrameworkElement>()) {
            rootElement.Tag(nullptr);
            rootElement.Width(1);
        }
        if (auto hostElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsWidgetHost")) {
            if (auto host = hostElement.try_as<wuxc::Canvas>()) {
                ClearPanelChildren(host);
                host.Width(1);
            }
        }
        root.Visibility(wux::Visibility::Collapsed);
        return;
    }

    root.Visibility(wux::Visibility::Visible);
    RebuildWidgetHostIfNeeded(root, widgets);
    ApplyWidgetCanvasLayout(root, widgets);

    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarWidgetsWidgetHost");
    auto host = hostElement.try_as<wuxc::Canvas>();
    if (!host) {
        return;
    }

    uint32_t childIndex = 0;
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        if (childIndex >= host.Children().Size()) {
            break;
        }
        UpdateTaskbarWidgetsWidgetRoot(host.Children().GetAt(childIndex), widget);
        ++childIndex;
    }
}

void RefreshInsertedTaskbarWidgetsRoots() {
    for (auto const& module : g_insertedModules) {
        if (module.root) {
            auto rootElement = module.root.try_as<wux::FrameworkElement>();
            if (rootElement && module.parent && module.trayElement) {
                ApplyTaskbarWidgetsAnchorMargin(rootElement, module.parent,
                                              module.trayElement);
            }
            UpdateTaskbarWidgetsRoot(module.root);
        }
    }
}

wux::DispatcherTimer StartTaskbarWidgetsTimer(wux::UIElement const& root,
                                            wuxc::Grid const& parent,
                                            wux::FrameworkElement const& trayElement) {
    wux::DispatcherTimer timer;
    timer.Interval(std::chrono::milliseconds(33));
    timer.Tick([root, parent, trayElement](auto const&, auto const&) {
        try {
            auto rootElement = root.try_as<wux::FrameworkElement>();
            if (rootElement && parent && trayElement) {
                ApplyTaskbarWidgetsAnchorMargin(rootElement, parent, trayElement);
            }
            UpdateTaskbarWidgetsRoot(root);
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"TaskbarWidgets update failed: 0x%08X %s", ex.code(),
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"TaskbarWidgets update failed with unknown exception");
        }
    });
    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement && parent && trayElement) {
        ApplyTaskbarWidgetsAnchorMargin(rootElement, parent, trayElement);
    }
    UpdateTaskbarWidgetsRoot(root);
    timer.Start();
    return timer;
}

bool FindTaskbarWidgetsChild(wuxc::Grid const& parent,
                           wux::UIElement& root,
                           uint32_t& childIndex) {
    childIndex = 0;
    for (auto const& child : parent.Children()) {
        auto element = child.try_as<wux::FrameworkElement>();
        if (element && element.Name() == L"TaskbarWidgetsRoot") {
            root = child;
            return true;
        }

        ++childIndex;
    }

    return false;
}

bool HasCurrentTaskbarWidgetsChild(wux::UIElement const& root) {
    return FindNamedFrameworkElement(root, kTaskbarWidgetsLayoutMarkerName) != nullptr;
}

bool HasTaskbarWidgetsChild(wuxc::Grid const& parent) {
    wux::UIElement root{nullptr};
    uint32_t childIndex = 0;
    return FindTaskbarWidgetsChild(parent, root, childIndex);
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
        Wh_Log(L"TaskbarWidgets cleanup failed: 0x%08X %s", ex.code(),
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

double GetOverlayRightReserve(wuxc::Grid const& parent,
                              wux::FrameworkElement const& trayElement) {
    double trayWidth = trayElement.ActualWidth();
    if (trayWidth < 120.0) {
        trayWidth = 190.0;
    }

    double rightReserve =
        std::max(kTaskbarWidgetsOverlayMinRightReserve,
                 trayWidth + kTaskbarWidgetsOverlayTrayGap);

    double parentWidth = parent.ActualWidth();
    if (parentWidth > 320.0) {
        double maxReserve = parentWidth - kTaskbarWidgetsMaxWidgetWidth - 20.0;
        if (maxReserve > 24.0) {
            rightReserve = std::min(rightReserve, maxReserve);
        }
    }

    return rightReserve;
}

void ApplyTaskbarWidgetsAnchorMargin(wux::FrameworkElement const& root,
                                   wuxc::Grid const& parent,
                                   wux::FrameworkElement const& trayElement) {
    if (parent.ColumnDefinitions().Size() == 0) {
        double rightReserve = GetOverlayRightReserve(parent, trayElement);
        double parentWidth = parent.ActualWidth();
        double availableWidth = parentWidth > rightReserve + 32.0
                                    ? parentWidth - rightReserve - 8.0
                                    : 1.0;
        availableWidth = std::max(1.0, availableWidth);
        if (std::fabs(root.Width() - availableWidth) > 0.5) {
            root.Width(availableWidth);
            root.Height(48);
        }
        auto current = root.Margin();
        if (std::fabs(current.Left) > 0.5 ||
            std::fabs(current.Top) > 0.5 ||
            std::fabs(current.Right - rightReserve) > 0.5 ||
            std::fabs(current.Bottom) > 0.5) {
            root.Margin(
                wux::ThicknessHelper::FromLengths(0, 0, rightReserve, 0));
            Wh_Log(L"Right-aligning TaskbarWidgetsRoot with reserved tray area %.1f",
                   rightReserve);
        }
        return;
    }

    auto current = root.Margin();
    if (std::fabs(current.Left - 6.0) > 0.5 ||
        std::fabs(current.Top) > 0.5 ||
        std::fabs(current.Right - kTaskbarWidgetsExplicitColumnRightGap) > 0.5 ||
        std::fabs(current.Bottom) > 0.5) {
        root.Margin(wux::ThicknessHelper::FromLengths(
            6, 0, kTaskbarWidgetsExplicitColumnRightGap, 0));
    }
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

    wux::UIElement existingRoot{nullptr};
    uint32_t existingRootIndex = 0;
    if (FindTaskbarWidgetsChild(parent, existingRoot, existingRootIndex)) {
        if (HasCurrentTaskbarWidgetsChild(existingRoot)) {
            Wh_Log(L"TaskbarWidgetsRoot already exists for this taskbar parent");
            return true;
        }

        auto existingElement = existingRoot.try_as<wux::FrameworkElement>();
        auto replacementRoot = MakeTaskbarWidgetsRoot();
        if (existingElement) {
            wuxc::Grid::SetColumn(replacementRoot,
                                  wuxc::Grid::GetColumn(existingElement));
            wuxc::Grid::SetRow(replacementRoot,
                               wuxc::Grid::GetRow(existingElement));
        }
        ApplyTaskbarWidgetsAnchorMargin(replacementRoot, parent, element);

        wuxc::Canvas::SetZIndex(replacementRoot, 10000);

        auto children = parent.Children();
        children.RemoveAt(existingRootIndex);
        children.InsertAt(existingRootIndex, replacementRoot.as<wux::UIElement>());
        auto timer = StartTaskbarWidgetsTimer(
            replacementRoot.as<wux::UIElement>(), parent, element);

        g_insertedModules.push_back(InsertedModule{
            .anchorHandle = handle,
            .parent = parent,
            .trayElement = element,
            .root = replacementRoot.as<wux::UIElement>(),
            .timer = timer,
            .insertedColumn = existingElement
                                  ? static_cast<uint32_t>(std::max(
                                        0, wuxc::Grid::GetColumn(existingElement)))
                                  : 0,
            .insertedGridColumn = false,
        });

        Wh_Log(L"Replaced stale TaskbarWidgetsRoot with current layout");
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

    auto root = MakeTaskbarWidgetsRoot();
    ApplyTaskbarWidgetsAnchorMargin(root, parent, element);

    // Prefer a real auto-width taskbar grid slot before the system tray when
    // explicit columns exist. Otherwise use right alignment with tray margin,
    // because adding a first column to the no-column parent centers the module.
    wuxc::Grid::SetColumn(root, targetColumn);
    wuxc::Grid::SetRow(root, wuxc::Grid::GetRow(element));
    wuxc::Canvas::SetZIndex(root, 10000);

    children.InsertAt(trayChildIndex, root.as<wux::UIElement>());
    auto timer = StartTaskbarWidgetsTimer(root.as<wux::UIElement>(), parent, element);

    g_insertedModules.push_back(InsertedModule{
        .anchorHandle = handle,
        .parent = parent,
        .trayElement = element,
        .root = root.as<wux::UIElement>(),
        .timer = timer,
        .insertedColumn = targetColumn,
        .insertedGridColumn = insertedGridColumn,
    });

    Wh_Log(L"Inserted TaskbarWidgetsRoot anchored to SystemTray.SystemTrayFrame");
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
        Wh_Log(L"Failed to schedule TaskbarWidgets insertion: 0x%08X %s",
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
        Wh_Log(L"TaskbarWidgets VisualTreeWatcher constructing");

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
static constexpr CLSID CLSID_TaskbarWidgetsTap = {
    0x3a94e3f0,
    0x8d88,
    0x4725,
    {0x9f, 0xe0, 0x8c, 0x9f, 0x45, 0x52, 0x97, 0x01}};

class TaskbarWidgetsTap
    : public winrt::implements<TaskbarWidgetsTap, IObjectWithSite,
                               winrt::non_agile> {
public:
    HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override {
        try {
            Wh_Log(L"TaskbarWidgetsTap::SetSite site=%p", site);

            if (g_visualTreeWatcher) {
                g_visualTreeWatcher->UnadviseVisualTreeChange();
                g_visualTreeWatcher = nullptr;
            }

            m_site.copy_from(site);
            if (m_site) {
                // Some XAML Diagnostics examples balance the module refcount added by
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

    if (!IsEqualCLSID(clsid, CLSID_TaskbarWidgetsTap)) {
        Wh_Log(L"DllGetClassObject unknown CLSID");
        return CLASS_E_CLASSNOTAVAILABLE;
    }

    return winrt::make<SimpleFactory<TaskbarWidgetsTap>>().as(riid, object);
}

extern "C" __declspec(dllexport) HRESULT __stdcall DllCanUnloadNow() {
    return winrt::get_module_lock() ? S_FALSE : S_OK;
}

using PFN_INITIALIZE_XAML_DIAGNOSTICS_EX =
    decltype(&InitializeXamlDiagnosticsEx);

HRESULT InjectTaskbarWidgetsTap() noexcept {
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

    g_inInjectTaskbarWidgetsTap = true;

    HRESULT hr = E_FAIL;
    for (int i = 0; i < 10000; ++i) {
        WCHAR connectionName[64]{};
        wsprintf(connectionName, L"VisualDiagConnection%d", i + 1);
        hr = initializeXamlDiagnostics(connectionName, GetCurrentProcessId(), L"",
                                       location, CLSID_TaskbarWidgetsTap, nullptr);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            break;
        }
    }

    g_inInjectTaskbarWidgetsTap = false;
    return hr;
}

void InitializeForCurrentThread() {
    if (g_initializedForThread) {
        return;
    }

    g_initializedForThread = true;
    Wh_Log(L"Initialized TaskbarWidgets for XAML thread %u",
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

    Wh_Log(L"InjectTaskbarWidgetsTap starting");
    HRESULT hr = InjectTaskbarWidgetsTap();
    if (FAILED(hr)) {
        Wh_Log(L"InjectTaskbarWidgetsTap failed: 0x%08X", hr);
        g_tapInitialized = false;
        return;
    }

    Wh_Log(L"InjectTaskbarWidgetsTap completed: 0x%08X", hr);
}

using RunFromWindowThreadProc = void(WINAPI*)(PVOID parameter);

bool RunFromWindowThread(HWND window,
                         RunFromWindowThreadProc proc,
                         PVOID procParam) {
    static const UINT registeredMessage =
        RegisterWindowMessage(L"TaskbarWidgets_RunFromWindowThread");

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

    constexpr PCWSTR className = L"TaskbarWidgetsWin32Proof";

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
        WS_EX_NOACTIVATE, className, L"TaskbarWidgets",
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
    Wh_Log(L"TaskbarWidgets product hook init");
    g_uninitializing = false;
    if (!g_gdiplusToken) {
        gdi::GdiplusStartupInput startupInput{};
        gdi::Status status =
            gdi::GdiplusStartup(&g_gdiplusToken, &startupInput, nullptr);
        if (status != gdi::Ok) {
            g_gdiplusToken = 0;
            Wh_Log(L"GdiplusStartup failed: %d", static_cast<int>(status));
        }
    }
    return TRUE;
}

void Wh_ModAfterInit() {
    Wh_Log(L"TaskbarWidgets after init");
    ScheduleDelayedInitialization(3000);
}

void Wh_ModUninit() {
    if (g_uninitializing.exchange(true)) {
        return;
    }

    Wh_Log(L"TaskbarWidgets uninit");

    if (g_weatherMenuWindow && IsWindow(g_weatherMenuWindow)) {
        DestroyWindow(g_weatherMenuWindow);
        g_weatherMenuWindow = nullptr;
    }

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

    if (g_gdiplusToken) {
        gdi::GdiplusShutdown(g_gdiplusToken);
        g_gdiplusToken = 0;
    }
}

std::wstring GetShutdownEventName() {
    WCHAR name[128]{};
    swprintf_s(name, L"Local\\TaskbarWidgetsHookShutdown_%u", GetCurrentProcessId());
    return name;
}

std::wstring GetLoadEventName() {
    WCHAR name[128]{};
    swprintf_s(name, L"Local\\TaskbarWidgetsHookLoad_%u", GetCurrentProcessId());
    return name;
}

DWORD WINAPI TaskbarWidgetsRuntimeThread(LPVOID) {
    while (!g_uninitializing) {
        InitializeExistingTaskbarThreads();
        InitializeTapOnce();
        Sleep(5000);
    }

    return 0;
}

HANDLE StartTaskbarWidgetsRuntime() {
    Wh_ModInit();
    Wh_ModAfterInit();

    HANDLE runtimeThread =
        CreateThread(nullptr, 0, TaskbarWidgetsRuntimeThread, nullptr, 0, nullptr);
    if (!runtimeThread) {
        Wh_Log(L"Failed to create runtime maintenance thread: %u", GetLastError());
    }
    return runtimeThread;
}

DWORD WINAPI TaskbarWidgetsControlThread(LPVOID) {
    std::wstring shutdownEventName = GetShutdownEventName();
    std::wstring loadEventName = GetLoadEventName();
    HANDLE shutdownEvent =
        CreateEvent(nullptr, TRUE, FALSE, shutdownEventName.c_str());
    HANDLE loadEvent = CreateEvent(nullptr, FALSE, FALSE, loadEventName.c_str());

    HANDLE runtimeThread = StartTaskbarWidgetsRuntime();
    bool runtimeActive = true;

    if (!shutdownEvent || !loadEvent) {
        Wh_Log(L"Failed to create runtime control events: %u", GetLastError());
        if (shutdownEvent) {
            CloseHandle(shutdownEvent);
        }
        if (loadEvent) {
            CloseHandle(loadEvent);
        }
        if (runtimeThread) {
            CloseHandle(runtimeThread);
        }
        return 0;
    }

    Wh_Log(L"Runtime control events ready: %s ; %s", shutdownEventName.c_str(),
           loadEventName.c_str());

    HANDLE events[] = {shutdownEvent, loadEvent};
    for (;;) {
        DWORD result = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            ResetEvent(shutdownEvent);
            if (!runtimeActive) {
                continue;
            }

            Wh_Log(L"Runtime unload event signaled");
            Wh_ModUninit();
            if (runtimeThread) {
                WaitForSingleObject(runtimeThread, 7000);
                CloseHandle(runtimeThread);
                runtimeThread = nullptr;
            }
            runtimeActive = false;
            Wh_Log(L"Runtime unloaded; hook module remains ready for Load");
        } else if (result == WAIT_OBJECT_0 + 1) {
            if (runtimeActive) {
                continue;
            }

            Wh_Log(L"Runtime load event signaled");
            runtimeThread = StartTaskbarWidgetsRuntime();
            runtimeActive = true;
        } else {
            Wh_Log(L"Runtime control wait failed: %u", GetLastError());
            break;
        }
    }

    if (runtimeThread) {
        CloseHandle(runtimeThread);
    }
    CloseHandle(loadEvent);
    CloseHandle(shutdownEvent);
    return 0;
}

extern "C" __declspec(dllexport) DWORD __stdcall TaskbarWidgetsHookVersion() {
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hookModule = reinterpret_cast<HMODULE>(instance);
        DisableThreadLibraryCalls(instance);
        HANDLE thread = CreateThread(nullptr, 0, TaskbarWidgetsControlThread,
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
