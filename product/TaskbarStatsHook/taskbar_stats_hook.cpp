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
#include <windowsx.h>
#include <gdiplus.h>
#include <ocidl.h>
#include <xamlom.h>

#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

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
thread_local bool g_initializedForThread = false;
bool g_inInjectTaskbarStatsTap = false;
constexpr PCWSTR kTaskbarStatsLayoutMarkerName =
    L"TaskbarStatsLayoutV20260714PercentMove";
constexpr double kTaskbarStatsExplicitColumnRightGap = 10.0;
constexpr double kTaskbarStatsOverlayTrayGap = 28.0;
constexpr double kTaskbarStatsOverlayMinRightReserve = 220.0;
constexpr double kTaskbarStatsMaxWidgetWidth = 240.0;
HWND g_win32ProofWindow = nullptr;
HWND g_win32ProofParent = nullptr;
HMODULE g_hookModule = nullptr;
ULONG_PTR g_gdiplusToken = 0;

HMODULE GetCurrentModuleHandle();
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

std::wstring GetFallbackTaskbarStatsRootPath() {
    WCHAR localAppData[MAX_PATH]{};
    DWORD length = GetEnvironmentVariable(L"LOCALAPPDATA", localAppData,
                                          ARRAYSIZE(localAppData));
    if (length == 0 || length >= ARRAYSIZE(localAppData)) {
        return {};
    }

    std::wstring path = localAppData;
    path += L"\\TaskbarStats";
    return path;
}

std::wstring GetTaskbarStatsRootPath() {
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

    return GetFallbackTaskbarStatsRootPath();
}

std::wstring GetTaskbarStatsLogPath(PCWSTR leaf) {
    std::wstring path = GetTaskbarStatsRootPath();
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
    std::wstring path = GetTaskbarStatsLogPath(L"hook.log");
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
    std::wstring path = GetTaskbarStatsRootPath();
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
    wux::FrameworkElement trayElement{nullptr};
    wux::UIElement root{nullptr};
    wux::DispatcherTimer timer{nullptr};
    uint32_t insertedColumn{};
    bool insertedGridColumn{};
};

thread_local std::vector<InsertedModule> g_insertedModules;

std::wstring GetAssetsFolder() {
    std::wstring path = GetTaskbarStatsRootPath();
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
void ShowAccountMenu(wux::FrameworkElement const& root);
void ShowWeatherMenu(wux::FrameworkElement const& root);
HWND FindCurrentProcessTaskbarWindow();
void UpdateTaskbarStatsRoot(wux::UIElement const& root);
void ShowWidgetLibraryWindow();
void RefreshInsertedTaskbarStatsRoots();
void ApplyTaskbarStatsAnchorMargin(wux::FrameworkElement const& root,
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

HWND g_accountMenuWindow = nullptr;
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

wux::FrameworkElement MakeWeatherPanel() {
    wuxc::Border weather;
    weather.Name(L"TaskbarStatsWeatherPanel");
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

    auto city = MakeNamedText(L"TaskbarStatsWeatherCity", L"Izmir", 11, 0xFF);
    city.Width(112);
    city.HorizontalAlignment(wux::HorizontalAlignment::Left);
    city.TextAlignment(wux::TextAlignment::Left);
    city.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    city.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());

    auto condition =
        MakeNamedText(L"TaskbarStatsWeatherCondition", L"--:-- • --/--", 9, 0xB8);
    condition.Width(112);
    condition.HorizontalAlignment(wux::HorizontalAlignment::Left);
    condition.TextAlignment(wux::TextAlignment::Left);
    condition.TextTrimming(wux::TextTrimming::CharacterEllipsis);

    wuxc::Grid::SetRow(city, 0);
    wuxc::Grid::SetRow(condition, 1);
    textBlock.Children().Append(city.as<wux::UIElement>());
    textBlock.Children().Append(condition.as<wux::UIElement>());
    wuxc::Grid::SetColumn(textBlock, 0);

    auto temp = MakeNamedText(L"TaskbarStatsWeatherTemp", L"--\x00B0", 21, 0xFF);
    temp.Width(44);
    temp.VerticalAlignment(wux::VerticalAlignment::Center);
    temp.HorizontalAlignment(wux::HorizontalAlignment::Right);
    temp.TextAlignment(wux::TextAlignment::Right);
    wuxc::Grid::SetColumn(temp, 1);

    auto icon = MakePngImage(L"weather\\rain.png", 46, 34);
    SetElementName(icon.as<wux::FrameworkElement>(), L"TaskbarStatsWeatherIcon");
    wuxc::Grid::SetColumn(icon.as<wux::FrameworkElement>(), 2);

    layout.Children().Append(textBlock.as<wux::UIElement>());
    layout.Children().Append(temp.as<wux::UIElement>());
    layout.Children().Append(icon);
    weather.Child(layout);
    return weather;
}

wux::FrameworkElement MakeDiscordPanel() {
    wuxc::Border discord;
    discord.Name(L"TaskbarStatsDiscordPanel");
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
        std::wstring frameName = L"TaskbarStatsDiscordAvatarFrame" + std::to_wstring(i);
        std::wstring avatarName = L"TaskbarStatsDiscordAvatarEllipse" + std::to_wstring(i);

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

wux::FrameworkElement MakeBtcPanel() {
    wuxc::Border panel;
    panel.Name(L"TaskbarStatsBtcPanel");
    panel.Width(220);
    panel.Height(44);
    panel.Visibility(wux::Visibility::Collapsed);
    panel.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(8));
    panel.Background(MakeBrush(0xFF, 0x12, 0x07, 0x18));
    panel.Padding(wux::ThicknessHelper::FromLengths(12, 5, 12, 5));

    wuxc::Grid layout;
    layout.Width(196);
    layout.Height(34);

    wuxc::RowDefinition topRow;
    topRow.Height(wux::GridLengthHelper::FromPixels(17));
    wuxc::RowDefinition bottomRow;
    bottomRow.Height(wux::GridLengthHelper::FromPixels(17));
    layout.RowDefinitions().Append(topRow);
    layout.RowDefinitions().Append(bottomRow);

    wuxc::ColumnDefinition labelColumn;
    labelColumn.Width(wux::GridLengthHelper::FromPixels(70));
    wuxc::ColumnDefinition valueColumn;
    valueColumn.Width(wux::GridLengthHelper::FromPixels(108));
    wuxc::ColumnDefinition iconColumn;
    iconColumn.Width(wux::GridLengthHelper::FromPixels(18));
    layout.ColumnDefinitions().Append(labelColumn);
    layout.ColumnDefinitions().Append(valueColumn);
    layout.ColumnDefinitions().Append(iconColumn);

    auto dateLabel = MakeNamedText(L"TaskbarStatsBtcDateLabel", L"Current date", 10, 0xD8);
    dateLabel.HorizontalAlignment(wux::HorizontalAlignment::Left);
    dateLabel.TextAlignment(wux::TextAlignment::Left);
    dateLabel.Foreground(MakeBrush(0xD8, 0xD0, 0xBF, 0xD8));
    dateLabel.Width(70);
    wuxc::Grid::SetRow(dateLabel, 0);
    wuxc::Grid::SetColumn(dateLabel, 0);

    auto dateValue =
        MakeNamedText(L"TaskbarStatsBtcDateValue", L"January 22, 2022 - 7:23 AM", 10, 0xFF);
    dateValue.HorizontalAlignment(wux::HorizontalAlignment::Right);
    dateValue.TextAlignment(wux::TextAlignment::Right);
    dateValue.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    dateValue.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    dateValue.Width(126);
    wuxc::Grid::SetRow(dateValue, 0);
    wuxc::Grid::SetColumn(dateValue, 1);
    wuxc::Grid::SetColumnSpan(dateValue, 2);

    wuxc::StackPanel fees;
    fees.Orientation(wuxc::Orientation::Horizontal);
    fees.HorizontalAlignment(wux::HorizontalAlignment::Left);
    fees.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::Grid::SetRow(fees, 1);
    wuxc::Grid::SetColumn(fees, 0);

    auto feesText = MakeText(L"Fees", 10, 0xD8);
    feesText.Foreground(MakeBrush(0xD8, 0xD0, 0xBF, 0xD8));
    feesText.HorizontalAlignment(wux::HorizontalAlignment::Left);
    feesText.TextAlignment(wux::TextAlignment::Left);
    auto arrow = MakeText(L"\x2197", 13, 0xFF);
    arrow.Margin(wux::ThicknessHelper::FromLengths(7, -1, 0, 0));
    arrow.Foreground(MakeBrush(0xFF, 0x17, 0xFF, 0xCF));
    fees.Children().Append(feesText.as<wux::UIElement>());
    fees.Children().Append(arrow.as<wux::UIElement>());

    auto feeValue = MakeNamedText(L"TaskbarStatsBtcFeeValue", L"0.00004353 ETH", 12, 0xFF);
    feeValue.HorizontalAlignment(wux::HorizontalAlignment::Right);
    feeValue.TextAlignment(wux::TextAlignment::Right);
    feeValue.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    feeValue.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    feeValue.Width(108);
    wuxc::Grid::SetRow(feeValue, 1);
    wuxc::Grid::SetColumn(feeValue, 1);

    auto ethIcon = MakeNamedText(L"TaskbarStatsBtcEthIcon", L"\x25C7", 15, 0xFF);
    ethIcon.Foreground(MakeBrush(0xFF, 0xC6, 0x69, 0xFF));
    ethIcon.HorizontalAlignment(wux::HorizontalAlignment::Right);
    ethIcon.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxc::Grid::SetRow(ethIcon, 1);
    wuxc::Grid::SetColumn(ethIcon, 2);

    layout.Children().Append(dateLabel.as<wux::UIElement>());
    layout.Children().Append(dateValue.as<wux::UIElement>());
    layout.Children().Append(fees.as<wux::UIElement>());
    layout.Children().Append(feeValue.as<wux::UIElement>());
    layout.Children().Append(ethIcon.as<wux::UIElement>());
    panel.Child(layout);
    return panel;
}

wux::FrameworkElement MakeMediaPanel() {
    wuxc::Border panel;
    panel.Name(L"TaskbarStatsMediaPanel");
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
    cover.Name(L"TaskbarStatsMediaCover");
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
    titleMarquee.Name(L"TaskbarStatsMediaTitleMarquee");
    titleMarquee.Orientation(wuxc::Orientation::Horizontal);
    titleMarquee.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleMarquee.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxm::TranslateTransform titleTransform;
    titleMarquee.RenderTransform(titleTransform);

    auto title = MakeNamedText(L"TaskbarStatsMediaTitle", L"No media", 11, 0xFF);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.Foreground(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.Width(112);

    auto titleClone = MakeNamedText(L"TaskbarStatsMediaTitleClone", L"", 11, 0xFF);
    titleClone.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleClone.TextAlignment(wux::TextAlignment::Left);
    titleClone.Foreground(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    titleClone.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    titleClone.Width(0);

    titleMarquee.Children().Append(title.as<wux::UIElement>());
    titleMarquee.Children().Append(titleClone.as<wux::UIElement>());
    titleViewport.Children().Append(titleMarquee.as<wux::UIElement>());

    auto artist = MakeNamedText(L"TaskbarStatsMediaArtist", L"Open a player", 9, 0xF0);
    artist.HorizontalAlignment(wux::HorizontalAlignment::Left);
    artist.TextAlignment(wux::TextAlignment::Left);
    artist.TextTrimming(wux::TextTrimming::CharacterEllipsis);
    artist.Foreground(MakeBrush(0xF0, 0x00, 0x00, 0x00));
    artist.Width(112);
    wuxc::Grid::SetRow(artist, 1);

    textGrid.Children().Append(titleViewport.as<wux::UIElement>());
    textGrid.Children().Append(artist.as<wux::UIElement>());

    wuxc::Border playButton;
    playButton.Name(L"TaskbarStatsMediaPlayButton");
    playButton.Width(18);
    playButton.Height(18);
    playButton.CornerRadius(wux::CornerRadiusHelper::FromUniformRadius(9));
    playButton.HorizontalAlignment(wux::HorizontalAlignment::Right);
    playButton.VerticalAlignment(wux::VerticalAlignment::Center);
    playButton.Background(MakeBrush(0xFF, 0x00, 0x00, 0x00));
    wuxc::Grid::SetColumn(playButton, 2);

    wuxc::FontIcon playIcon;
    playIcon.Name(L"TaskbarStatsMediaPlayIcon");
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
    panel.Name(L"TaskbarStatsSteamPanel");
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
    backdrop.Name(L"TaskbarStatsSteamBackdrop");
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
    cover.Name(L"TaskbarStatsSteamCover");
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
    titleMarquee.Name(L"TaskbarStatsSteamTitleMarquee");
    titleMarquee.Orientation(wuxc::Orientation::Horizontal);
    titleMarquee.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleMarquee.VerticalAlignment(wux::VerticalAlignment::Center);
    wuxm::TranslateTransform titleTransform;
    titleMarquee.RenderTransform(titleTransform);

    auto title = MakeNamedText(L"TaskbarStatsSteamTitle", L"Steam", 11, 0xFF);
    title.HorizontalAlignment(wux::HorizontalAlignment::Left);
    title.TextAlignment(wux::TextAlignment::Left);
    title.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    title.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    title.Width(93);

    auto titleClone = MakeNamedText(L"TaskbarStatsSteamTitleClone", L"", 11, 0xFF);
    titleClone.HorizontalAlignment(wux::HorizontalAlignment::Left);
    titleClone.TextAlignment(wux::TextAlignment::Left);
    titleClone.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    titleClone.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    titleClone.Width(0);

    titleMarquee.Children().Append(title.as<wux::UIElement>());
    titleMarquee.Children().Append(titleClone.as<wux::UIElement>());
    titleViewport.Children().Append(titleMarquee.as<wux::UIElement>());

    auto detail = MakeNamedText(L"TaskbarStatsSteamDetail", L"Indirme yok", 9, 0xF0);
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

    auto metric = MakeNamedText(L"TaskbarStatsSteamMetric", L"--", 11, 0xFF);
    metric.HorizontalAlignment(wux::HorizontalAlignment::Right);
    metric.TextAlignment(wux::TextAlignment::Right);
    metric.Foreground(MakeBrush(0xFF, 0xF8, 0xFA, 0xFC));
    metric.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    metric.Width(44);
    wuxc::Grid::SetRow(metric, 0);

    wuxc::Grid progressTrack;
    progressTrack.Name(L"TaskbarStatsSteamProgressTrack");
    progressTrack.Width(38);
    progressTrack.Height(3);
    progressTrack.Margin(wux::ThicknessHelper::FromLengths(0, 5, 0, 0));
    progressTrack.HorizontalAlignment(wux::HorizontalAlignment::Right);
    progressTrack.VerticalAlignment(wux::VerticalAlignment::Top);
    progressTrack.Background(MakeBrush(0x48, 0xCB, 0xD5, 0xE1));
    wuxc::Grid::SetRow(progressTrack, 1);

    wuxc::Border progressFill;
    progressFill.Name(L"TaskbarStatsSteamProgressFill");
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

void ShowWidgetContextMenu() {
    constexpr UINT_PTR kSettingsCommand = 1001;
    constexpr UINT_PTR kQuitCommand = 1002;

    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kSettingsCommand, L"Settings");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand, L"Quit TaskbarStats");

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
        WriteTaskbarStatsCommand(L"openSettings");
    } else if (command == kQuitCommand) {
        WriteTaskbarStatsCommand(L"quit");
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

wux::FrameworkElement MakeTaskbarStatsWidgetRoot(const WidgetInstanceRuntime& instance) {
    wuxc::Grid root;
    root.Name(L"TaskbarStatsWidget");
    root.Tag(winrt::box_value(winrt::hstring(instance.designId)));
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

    wuxc::Grid limitBar;
    limitBar.Name(L"TaskbarStatsLimitBarTrack");
    limitBar.Height(2);
    limitBar.Width(126);
    limitBar.HorizontalAlignment(wux::HorizontalAlignment::Center);
    limitBar.Background(MakeBrush(0x34, 0x94, 0xA3, 0xB8));

    wuxc::Border limitBarFill;
    limitBarFill.Name(L"TaskbarStatsLimitBarFill");
    limitBarFill.Height(2);
    limitBarFill.Width(0);
    limitBarFill.HorizontalAlignment(wux::HorizontalAlignment::Left);
    limitBarFill.Background(MakeBrush(0xFF, 0x94, 0xA3, 0xB8));
    limitBar.Children().Append(limitBarFill.as<wux::UIElement>());

    wuxc::StackPanel metrics;
    metrics.Orientation(wuxc::Orientation::Horizontal);
    metrics.HorizontalAlignment(wux::HorizontalAlignment::Center);
    metrics.VerticalAlignment(wux::VerticalAlignment::Center);
    metrics.Children().Append(MakeSmallMetric(L"\xE950", L"TaskbarStatsLimit", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE823", L"TaskbarStatsReset", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE9D2", L"TaskbarStatsWeek", L"--"));
    metrics.Children().Append(MakeSmallMetric(L"\xE8D4", L"TaskbarStatsTokens", L"--"));

    wuxc::Grid::SetRow(titleLine, 0);
    wuxc::Grid::SetRow(limitBar, 1);
    wuxc::Grid::SetRow(metrics, 2);
    compact.Children().Append(titleLine.as<wux::UIElement>());
    compact.Children().Append(limitBar.as<wux::UIElement>());
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
    root.Children().Append(MakeWeatherPanel().as<wux::UIElement>());
    root.Children().Append(MakeDiscordPanel().as<wux::UIElement>());
    root.Children().Append(MakeBtcPanel().as<wux::UIElement>());
    root.Children().Append(MakeMediaPanel().as<wux::UIElement>());
    root.Children().Append(MakeSteamDownloadPanel().as<wux::UIElement>());

    wuxc::Border layoutMarker;
    layoutMarker.Name(kTaskbarStatsLayoutMarkerName);
    layoutMarker.Visibility(wux::Visibility::Collapsed);
    root.Children().Append(layoutMarker.as<wux::UIElement>());

    root.PointerEntered([root](auto const&, auto const&) {
        SetExpandedMode(root.as<wux::UIElement>(), true);
    });
    root.PointerExited([root](auto const&, auto const&) {
        SetExpandedMode(root.as<wux::UIElement>(), false);
    });
    root.Tapped([root](auto const&, wuxi::TappedRoutedEventArgs const& args) {
        std::wstring activeDesign = GetWidgetDesignFromRoot(root.as<wux::UIElement>());
        if (activeDesign == L"weather-static") {
            ShowWeatherMenu(root);
        } else if (activeDesign == L"discord-voice") {
            ShowWidgetLibraryWindow();
        } else if (activeDesign == L"media-player") {
            wf::Point point = args.GetPosition(root);
            if (point.X >= 190.0) {
                WriteTaskbarStatsCommand(L"mediaToggle");
            } else {
                ShowWidgetLibraryWindow();
            }
        } else if (activeDesign == L"steam-download") {
            ShowWidgetLibraryWindow();
        } else if (activeDesign == L"btc-fees") {
            ShowWidgetLibraryWindow();
        } else {
            ShowAccountMenu(root);
        }
        args.Handled(true);
    });
    root.RightTapped([](auto const&, wuxi::RightTappedRoutedEventArgs const& args) {
        ShowWidgetContextMenu();
        args.Handled(true);
    });

    return root;
}

wux::FrameworkElement MakeTaskbarStatsRoot() {
    wuxc::Grid root;
    root.Name(L"TaskbarStatsRoot");
    root.VerticalAlignment(wux::VerticalAlignment::Center);
    root.HorizontalAlignment(wux::HorizontalAlignment::Right);
    root.Height(48);
    root.Width(1);
    root.Margin(wux::ThicknessHelper::FromLengths(6, 0, 6, 0));

    wuxc::Canvas host;
    host.Name(L"TaskbarStatsWidgetHost");
    host.HorizontalAlignment(wux::HorizontalAlignment::Right);
    host.VerticalAlignment(wux::VerticalAlignment::Center);
    host.Height(48);
    host.Width(1);
    root.Children().Append(host.as<wux::UIElement>());

    wuxc::Border layoutMarker;
    layoutMarker.Name(kTaskbarStatsLayoutMarkerName);
    layoutMarker.Visibility(wux::Visibility::Collapsed);
    root.Children().Append(layoutMarker.as<wux::UIElement>());

    return root;
}

std::wstring GetCodexStatusPath() {
    return GetTaskbarStatsPath(L"codex-status.json");
}

std::wstring GetWeatherStatusPath() {
    return GetTaskbarStatsPath(L"weather-status.json");
}

std::wstring GetDiscordStatusPath() {
    return GetTaskbarStatsPath(L"discord-voice-status.json");
}

std::wstring GetMediaStatusPath() {
    return GetTaskbarStatsPath(L"media-status.json");
}

std::wstring GetSteamDownloadStatusPath() {
    return GetTaskbarStatsPath(L"steam-download-status.json");
}

std::wstring GetWidgetSettingsPath() {
    return GetTaskbarStatsPath(L"widget-settings.json");
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
    std::string decoded;
    decoded.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 1 < raw.size()) {
            char next = raw[++i];
            switch (next) {
                case '\\':
                case '"':
                case '/':
                    decoded += next;
                    break;
                case 'n':
                    decoded += '\n';
                    break;
                case 'r':
                    decoded += '\r';
                    break;
                case 't':
                    decoded += '\t';
                    break;
                default:
                    decoded += next;
                    break;
            }
        } else {
            decoded += raw[i];
        }
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
    return _wcsicmp(designId.c_str(), L"codex-status") == 0 ||
           _wcsicmp(designId.c_str(), L"weather-static") == 0 ||
           _wcsicmp(designId.c_str(), L"discord-voice") == 0 ||
           _wcsicmp(designId.c_str(), L"btc-fees") == 0 ||
           _wcsicmp(designId.c_str(), L"media-player") == 0 ||
           _wcsicmp(designId.c_str(), L"steam-download") == 0;
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

WidgetRuntimeSettings ReadWidgetRuntimeSettings() {
    WidgetRuntimeSettings settings;
    std::string json = ReadUtf8File(GetWidgetSettingsPath());
    std::wstring activeDesign;
    if (!json.empty() && ExtractJsonString(json, "activeDesign", activeDesign) &&
        IsKnownWidgetDesign(activeDesign)) {
        settings.activeDesign = activeDesign;
    }

    bool enabled = true;
    if (ExtractJsonBool(json, "enabled", enabled)) {
        settings.enabled = enabled;
    }

    bool rotationEnabled = false;
    if (ExtractJsonBool(json, "rotationEnabled", rotationEnabled)) {
        settings.rotationEnabled = rotationEnabled;
    }

    bool discordBackgroundEnabled = true;
    if (ExtractJsonBool(json, "discordBackgroundEnabled", discordBackgroundEnabled)) {
        settings.discordBackgroundEnabled = discordBackgroundEnabled;
    }

    bool mediaDarkMode = true;
    if (ExtractJsonBool(json, "mediaDarkMode", mediaDarkMode)) {
        settings.mediaDarkMode = mediaDarkMode;
    }

    long long rotationInterval = 0;
    if (ExtractJsonInt64(json, "rotationIntervalSecs", rotationInterval)) {
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

    settings.rotationDesigns = ExtractJsonStringArray(json, "rotationDesigns");
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

    WidgetRuntimeSettings settings = ReadWidgetRuntimeSettings();
    std::vector<std::wstring> rotationDesigns = settings.rotationDesigns;
    if (rotationDesigns.empty()) {
        rotationDesigns.push_back(designId);
    }

    std::string json = "{\n  \"activeDesign\": \"" + JsonEscapeUtf8(designId) +
                       "\",\n  \"enabled\": " +
                       (settings.enabled ? "true" : "false") +
                       ",\n  \"rotationEnabled\": " +
                       (settings.rotationEnabled ? "true" : "false") +
                       ",\n  \"rotationIntervalSecs\": " +
                       std::to_string(settings.rotationIntervalSecs) +
                       ",\n  \"discordBackgroundEnabled\": " +
                       (settings.discordBackgroundEnabled ? "true" : "false") +
                       ",\n  \"mediaDarkMode\": " +
                       (settings.mediaDarkMode ? "true" : "false") +
                       ",\n  \"widgetOffsetPx\": " +
                       std::to_string(settings.widgetOffsetPx) +
                       ",\n  \"widgetMoveX\": " +
                       std::to_string(settings.widgetMoveX) +
                       ",\n  \"rotationDesigns\": [";
    for (size_t i = 0; i < rotationDesigns.size(); ++i) {
        if (i > 0) {
            json += ", ";
        }
        json += "\"" + JsonEscapeUtf8(rotationDesigns[i]) + "\"";
    }
    json += "]\n}\n";
    return WriteUtf8File(GetWidgetSettingsPath(), json);
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
        if (!IsKnownWidgetDesign(widget.designId)) {
            continue;
        }

        bool enabled = true;
        if (ExtractJsonBool(object, "enabled", enabled)) {
            widget.enabled = enabled;
        }

        long long moveX = 0;
        if (ExtractJsonInt64(object, "moveX", moveX) ||
            ExtractJsonInt64(object, "widgetMoveX", moveX)) {
            widget.moveX = std::clamp(moveX, -640LL, 640LL);
        }

        long long positionPct = -1;
        if (ExtractJsonInt64(object, "positionPct", positionPct)) {
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
        WriteTaskbarStatsCommand(command);
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

    DrawPopupText(dc, L"TaskbarStats", titleRect, RGB(248, 250, 252),
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

                WriteTaskbarStatsCommand(item.command, item.accountId);
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

    int x = taskbarRect.right - 540;
    int y = taskbarRect.top - height - 8;
    if (taskbarRect.top <= monitorInfo.rcMonitor.top + 4) {
        y = taskbarRect.bottom + 8;
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

        constexpr PCWSTR className = L"TaskbarStatsAccountMenuPopup";
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
            className, L"TaskbarStats Accounts", WS_POPUP,
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
        DrawPopupText(dc, L"TaskbarStats", brand, RGB(248, 250, 252), titleFont);
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
                        RefreshInsertedTaskbarStatsRoots();
                        InvalidateRect(window, nullptr, FALSE);
                    }
                    return 0;
                }

                if (item.command == L"addWidgetLibrary") {
                    WriteTaskbarStatsCommand(item.command);
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
    WriteTaskbarStatsCommand(L"openSettings");
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

        constexpr PCWSTR className = L"TaskbarStatsWeatherPopup";
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
            className, L"TaskbarStats Weather", WS_POPUP,
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
    auto first = FindNamedTextBlock(root, L"TaskbarStatsMediaTitle");
    auto second = FindNamedTextBlock(root, L"TaskbarStatsMediaTitleClone");
    auto marqueeElement =
        FindNamedFrameworkElement(root, L"TaskbarStatsMediaTitleMarquee");
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
    auto first = FindNamedTextBlock(root, L"TaskbarStatsSteamTitle");
    auto second = FindNamedTextBlock(root, L"TaskbarStatsSteamTitleClone");
    auto marqueeElement =
        FindNamedFrameworkElement(root, L"TaskbarStatsSteamTitleMarquee");
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

    wuxmi::BitmapImage source;
    source.UriSource(wf::Uri(ToFileUri(assetPath)));
    image.Source(source);
}

void SetNamedImageFileSource(wux::UIElement const& root,
                             PCWSTR name,
                             const std::wstring& path) {
    auto element = FindNamedFrameworkElement(root, name);
    auto image = element ? element.try_as<wuxc::Image>() : nullptr;
    if (!image || path.empty() || !FileExists(path)) {
        return;
    }

    wuxmi::BitmapImage source;
    source.UriSource(wf::Uri(ToFileUri(path)));
    image.Source(source);
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
    if (value.size() != 7 || value[0] != L'#') {
        return false;
    }

    BYTE channels[3]{};
    for (int i = 0; i < 3; ++i) {
        int high = HexDigit(value[1 + i * 2]);
        int low = HexDigit(value[2 + i * 2]);
        if (high < 0 || low < 0) {
            return false;
        }
        channels[i] = static_cast<BYTE>((high << 4) | low);
    }

    color = winrt::Windows::UI::Color{
        0xFF, channels[0], channels[1], channels[2]};
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
            SetNamedBorderFill(root, L"TaskbarStatsMediaPanel",
                               MakeMediaGradientBrush(left, right));
        } else {
            SetNamedBorderFill(root, L"TaskbarStatsMediaPanel",
                               MakeBrush(0xFF, 0x0F, 0x17, 0x2A));
        }

        winrt::Windows::UI::Color accent{0xFF, 0x22, 0xD3, 0xEE};
        ParseHexColor(media.accentColor, accent);
        SetNamedTextColor(root, L"TaskbarStatsMediaTitle",
                          winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
        SetNamedTextColor(root, L"TaskbarStatsMediaTitleClone",
                          winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
        SetNamedTextColor(root, L"TaskbarStatsMediaArtist",
                          winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
        if (active) {
            SetNamedBorderFill(root, L"TaskbarStatsMediaPlayButton",
                               wuxm::SolidColorBrush(accent));
        } else {
            SetNamedBorderFill(root, L"TaskbarStatsMediaPlayButton",
                               MakeBrush(0xFF, 0x33, 0x41, 0x55));
        }
        SetNamedIconColor(root, L"TaskbarStatsMediaPlayIcon",
                          active ? winrt::Windows::UI::Color{0xFF, 0x08, 0x17, 0x1F}
                                 : winrt::Windows::UI::Color{0xFF, 0xCB, 0xD5, 0xE1});
        return;
    }

    SetNamedBorderFill(root, L"TaskbarStatsMediaPanel",
                       MakeBrush(0xFF, 0xFA, 0xFA, 0xF8));
    SetNamedTextColor(root, L"TaskbarStatsMediaTitle",
                      winrt::Windows::UI::Color{0xFF, 0x00, 0x00, 0x00});
    SetNamedTextColor(root, L"TaskbarStatsMediaTitleClone",
                      winrt::Windows::UI::Color{0xFF, 0x00, 0x00, 0x00});
    SetNamedTextColor(root, L"TaskbarStatsMediaArtist",
                      winrt::Windows::UI::Color{0xF0, 0x00, 0x00, 0x00});
    SetNamedBorderFill(root, L"TaskbarStatsMediaPlayButton",
                       active ? MakeBrush(0xFF, 0x00, 0x00, 0x00)
                              : MakeBrush(0x55, 0x00, 0x00, 0x00));
    SetNamedIconColor(root, L"TaskbarStatsMediaPlayIcon",
                      winrt::Windows::UI::Color{0xFF, 0xFF, 0xFF, 0xFF});
}

void ApplySteamDownloadTheme(wux::UIElement const& root,
                             bool active,
                             bool hasCoverArt) {
    SetNamedBorderFill(root, L"TaskbarStatsSteamPanel",
                       MakeBrush(0xFF, 0x0B, 0x12, 0x20));
    if (!hasCoverArt) {
        SetNamedBorderFill(root, L"TaskbarStatsSteamBackdrop",
                           MakeBrush(0xFF, 0x0B, 0x12, 0x20));
        SetNamedBorderFill(root, L"TaskbarStatsSteamCover",
                           MakeMediaGradientBrush(
                               winrt::Windows::UI::Color{0xFF, 0x1B, 0x28, 0x38},
                               winrt::Windows::UI::Color{0xFF, 0x2A, 0x47, 0x5E}));
    }
    SetNamedTextColor(root, L"TaskbarStatsSteamTitle",
                      winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
    SetNamedTextColor(root, L"TaskbarStatsSteamTitleClone",
                      winrt::Windows::UI::Color{0xFF, 0xF8, 0xFA, 0xFC});
    SetNamedTextColor(root, L"TaskbarStatsSteamDetail",
                      active ? winrt::Windows::UI::Color{0xFF, 0xCB, 0xD5, 0xE1}
                             : winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
    SetNamedTextColor(root, L"TaskbarStatsSteamMetric",
                      active ? winrt::Windows::UI::Color{0xFF, 0x66, 0xC0, 0xF4}
                             : winrt::Windows::UI::Color{0xFF, 0x94, 0xA3, 0xB8});
}

void SetDiscordPanelBackground(wux::UIElement const& root, bool enabled) {
    auto element = FindNamedFrameworkElement(root, L"TaskbarStatsDiscordPanel");
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

void SetRateLimitProgressVisual(wux::UIElement const& root,
                                double usedPercent) {
    constexpr double barWidth = 126.0;
    double remaining = GetRemainingPercent(usedPercent);
    auto color = GetRateLimitColor(remaining);

    SetNamedTextColor(root, L"TaskbarStatsTitle", color);

    auto fillElement = FindNamedFrameworkElement(root, L"TaskbarStatsLimitBarFill");
    if (fillElement) {
        fillElement.Width(remaining < 0 ? 0 : barWidth * remaining / 100.0);
        auto fill = fillElement.try_as<wuxc::Border>();
        if (fill) {
            fill.Background(wuxm::SolidColorBrush(color));
        }
    }

    auto trackElement = FindNamedFrameworkElement(root, L"TaskbarStatsLimitBarTrack");
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
    auto fillElement = FindNamedFrameworkElement(root, L"TaskbarStatsSteamProgressFill");
    if (fillElement) {
        fillElement.Width(active ? barWidth * value / 100.0 : 0.0);
        auto fill = fillElement.try_as<wuxc::Border>();
        if (fill) {
            fill.Background(active ? MakeBrush(0xFF, 0x66, 0xC0, 0xF4)
                                   : MakeBrush(0x77, 0x94, 0xA3, 0xB8));
        }
    }

    SetNamedOpacity(root, L"TaskbarStatsSteamProgressTrack", active ? 1.0 : 0.35);
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
            if (activeDesign == L"btc-fees" ||
                activeDesign == L"media-player" ||
                activeDesign == L"steam-download") {
                rootElement.Width(230);
                rootElement.Height(44);
            } else if (activeDesign == L"discord-voice") {
                rootElement.Width(196);
                rootElement.Height(36);
            } else {
                rootElement.Width(240);
                rootElement.Height(36);
            }
        }
        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        return;
    }

    if (expanded && GetAntigravityProjectTitles().size() <= 1) {
        expanded = false;
    }

    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement) {
        rootElement.Width(184);
        rootElement.Height(36);
    }

    SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                       expanded ? wux::Visibility::Collapsed
                                : wux::Visibility::Visible);
    SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                       expanded ? wux::Visibility::Visible
                                 : wux::Visibility::Collapsed);
}

void UpdateTaskbarStatsWidgetRoot(wux::UIElement const& root,
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
    if (activeDesign == L"btc-fees") {
        auto rootElement = root.try_as<wux::FrameworkElement>();
        if (rootElement) {
            rootElement.Width(230);
            rootElement.Height(44);
        }

        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                           wux::Visibility::Visible);
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

        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                           wux::Visibility::Visible);
        bool hasMediaText = !Trim(media.title).empty() || !Trim(media.artist).empty();
        SetMediaTitleText(root, hasMediaText
                                    ? (media.title.empty() ? L"Unknown media" : media.title)
                                    : L"No media");
        SetNamedText(root, L"TaskbarStatsMediaArtist",
                     hasMediaText
                         ? (media.artist.empty() ? L"Unknown artist" : media.artist)
                         : L"Open a player");
        SetNamedIconGlyph(root, L"TaskbarStatsMediaPlayIcon",
                          media.playing ? L"\xE769" : L"\xE768");
        SetNamedOpacity(root, L"TaskbarStatsMediaPlayIcon", media.active ? 1.0 : 0.45);
        ApplyMediaTheme(root, settings.mediaDarkMode,
                        media.active || hasMediaText, media);
        SetNamedBorderImageBackground(root, L"TaskbarStatsMediaCover",
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

        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
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
        SetNamedText(root, L"TaskbarStatsSteamDetail", detail);
        SetNamedText(root, L"TaskbarStatsSteamMetric", metric);
        bool hasCoverArt = steam.loaded && steam.active &&
                           !steam.coverPath.empty() &&
                           FileExists(steam.coverPath);
        ApplySteamDownloadTheme(root, steam.loaded && steam.active, hasCoverArt);
        SetSteamDownloadProgressVisual(root, steam.progressPercent,
                                       steam.loaded && steam.active);
        if (hasCoverArt) {
            SetNamedBorderImageBackground(root, L"TaskbarStatsSteamCover",
                                          steam.coverPath);
            SetNamedBorderImageBackground(root, L"TaskbarStatsSteamBackdrop",
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

        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                           wux::Visibility::Visible);
        SetDiscordPanelBackground(root, settings.discordBackgroundEnabled);

        for (int i = 0; i < 5; ++i) {
            std::wstring frameName = L"TaskbarStatsDiscordAvatarFrame" + std::to_wstring(i);
            std::wstring avatarName = L"TaskbarStatsDiscordAvatarEllipse" + std::to_wstring(i);
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

        SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsExpandedPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                           wux::Visibility::Visible);
        SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                           wux::Visibility::Collapsed);
        SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
                           wux::Visibility::Collapsed);

        if (!weather.loaded) {
            SetNamedText(root, L"TaskbarStatsWeatherCity", L"Izmir");
            SetNamedText(root, L"TaskbarStatsWeatherCondition", L"--:-- • --/--");
            SetNamedText(root, L"TaskbarStatsWeatherTemp", L"--\x00B0");
            SetNamedImageSource(root, L"TaskbarStatsWeatherIcon",
                                L"weather\\rain.png");
            return;
        }

        SetNamedText(root, L"TaskbarStatsWeatherCity", weather.location);
        SetNamedText(root, L"TaskbarStatsWeatherCondition",
                     FormatWeatherDate(weather.updatedAtUnix));
        SetNamedText(root, L"TaskbarStatsWeatherTemp",
                     FormatTemperature(weather.temperature));
        SetNamedImageSource(root, L"TaskbarStatsWeatherIcon",
                            WeatherIconAsset(weather.weatherCode));
        return;
    }

    SetNamedVisibility(root, L"TaskbarStatsWeatherPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarStatsDiscordPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarStatsBtcPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarStatsMediaPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarStatsSteamPanel",
                       wux::Visibility::Collapsed);
    SetNamedVisibility(root, L"TaskbarStatsCompactPanel",
                       wux::Visibility::Visible);

    CodexStatusSnapshot snapshot = ReadCodexStatusSnapshot();
    if (!snapshot.loaded) {
        SetNamedText(root, L"TaskbarStatsTitle", L"Antigravity");
        SetRateLimitProgressVisual(root, -1);
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
    SetRateLimitProgressVisual(root, snapshot.primaryUsedPercent);

    SetNamedText(root, L"TaskbarStatsLimit",
                 FormatRemainingPercent(snapshot.primaryUsedPercent));
    SetNamedText(root, L"TaskbarStatsReset",
                 FormatReset(snapshot.primaryResetsAtUnix));
    SetNamedText(root, L"TaskbarStatsWeek",
                 FormatRemainingPercent(snapshot.secondaryUsedPercent));
    SetNamedText(root, L"TaskbarStatsTokens",
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

    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarStatsWidgetHost");
    auto host = hostElement.try_as<wuxc::Canvas>();
    if (!host) {
        return;
    }

    ClearPanelChildren(host);
    for (const auto& widget : widgets) {
        if (!widget.enabled) {
            continue;
        }
        auto childRoot = MakeTaskbarStatsWidgetRoot(widget);
        host.Children().Append(childRoot.as<wux::UIElement>());
    }

    rootElement.Tag(winrt::box_value(winrt::hstring(signature)));
}

double WidgetDesignWidth(const std::wstring& designId) {
    if (designId == L"btc-fees" ||
        designId == L"media-player" ||
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
    if (designId == L"btc-fees" ||
        designId == L"media-player" ||
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
    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarStatsWidgetHost");
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
            double usableWidth = std::max(0.0, canvasWidth - width);
            left = usableWidth * (static_cast<double>(widget.positionPct) / 100.0);
        } else {
            left = canvasWidth + static_cast<double>(widget.moveX) - width;
        }
        left = std::clamp(left, 0.0, std::max(0.0, canvasWidth - width));
        double top = std::max(0.0, (48.0 - height) / 2.0);
        wuxc::Canvas::SetLeft(child, left);
        wuxc::Canvas::SetTop(child, top);
        ++childIndex;
    }
}

void UpdateTaskbarStatsRoot(wux::UIElement const& root) {
    std::vector<WidgetInstanceRuntime> widgets = ReadWidgetInstances();
    bool hasEnabledWidget = std::any_of(
        widgets.begin(), widgets.end(),
        [](const WidgetInstanceRuntime& widget) { return widget.enabled; });

    if (!hasEnabledWidget) {
        if (auto rootElement = root.try_as<wux::FrameworkElement>()) {
            rootElement.Tag(nullptr);
            rootElement.Width(1);
        }
        if (auto hostElement = FindNamedFrameworkElement(root, L"TaskbarStatsWidgetHost")) {
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

    auto hostElement = FindNamedFrameworkElement(root, L"TaskbarStatsWidgetHost");
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
        UpdateTaskbarStatsWidgetRoot(host.Children().GetAt(childIndex), widget);
        ++childIndex;
    }
}

void RefreshInsertedTaskbarStatsRoots() {
    for (auto const& module : g_insertedModules) {
        if (module.root) {
            auto rootElement = module.root.try_as<wux::FrameworkElement>();
            if (rootElement && module.parent && module.trayElement) {
                ApplyTaskbarStatsAnchorMargin(rootElement, module.parent,
                                              module.trayElement);
            }
            UpdateTaskbarStatsRoot(module.root);
        }
    }
}

wux::DispatcherTimer StartTaskbarStatsTimer(wux::UIElement const& root,
                                            wuxc::Grid const& parent,
                                            wux::FrameworkElement const& trayElement) {
    wux::DispatcherTimer timer;
    timer.Interval(std::chrono::milliseconds(33));
    timer.Tick([root, parent, trayElement](auto const&, auto const&) {
        try {
            auto rootElement = root.try_as<wux::FrameworkElement>();
            if (rootElement && parent && trayElement) {
                ApplyTaskbarStatsAnchorMargin(rootElement, parent, trayElement);
            }
            UpdateTaskbarStatsRoot(root);
        } catch (winrt::hresult_error const& ex) {
            Wh_Log(L"TaskbarStats update failed: 0x%08X %s", ex.code(),
                   ex.message().c_str());
        } catch (...) {
            Wh_Log(L"TaskbarStats update failed with unknown exception");
        }
    });
    auto rootElement = root.try_as<wux::FrameworkElement>();
    if (rootElement && parent && trayElement) {
        ApplyTaskbarStatsAnchorMargin(rootElement, parent, trayElement);
    }
    UpdateTaskbarStatsRoot(root);
    timer.Start();
    return timer;
}

bool FindTaskbarStatsChild(wuxc::Grid const& parent,
                           wux::UIElement& root,
                           uint32_t& childIndex) {
    childIndex = 0;
    for (auto const& child : parent.Children()) {
        auto element = child.try_as<wux::FrameworkElement>();
        if (element && element.Name() == L"TaskbarStatsRoot") {
            root = child;
            return true;
        }

        ++childIndex;
    }

    return false;
}

bool HasCurrentTaskbarStatsChild(wux::UIElement const& root) {
    return FindNamedFrameworkElement(root, kTaskbarStatsLayoutMarkerName) != nullptr;
}

bool HasTaskbarStatsChild(wuxc::Grid const& parent) {
    wux::UIElement root{nullptr};
    uint32_t childIndex = 0;
    return FindTaskbarStatsChild(parent, root, childIndex);
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

double GetOverlayRightReserve(wuxc::Grid const& parent,
                              wux::FrameworkElement const& trayElement) {
    double trayWidth = trayElement.ActualWidth();
    if (trayWidth < 120.0) {
        trayWidth = 190.0;
    }

    double rightReserve =
        std::max(kTaskbarStatsOverlayMinRightReserve,
                 trayWidth + kTaskbarStatsOverlayTrayGap);

    double parentWidth = parent.ActualWidth();
    if (parentWidth > 320.0) {
        double maxReserve = parentWidth - kTaskbarStatsMaxWidgetWidth - 20.0;
        if (maxReserve > 24.0) {
            rightReserve = std::min(rightReserve, maxReserve);
        }
    }

    return rightReserve;
}

void ApplyTaskbarStatsAnchorMargin(wux::FrameworkElement const& root,
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
            Wh_Log(L"Right-aligning TaskbarStatsRoot with reserved tray area %.1f",
                   rightReserve);
        }
        return;
    }

    auto current = root.Margin();
    if (std::fabs(current.Left - 6.0) > 0.5 ||
        std::fabs(current.Top) > 0.5 ||
        std::fabs(current.Right - kTaskbarStatsExplicitColumnRightGap) > 0.5 ||
        std::fabs(current.Bottom) > 0.5) {
        root.Margin(wux::ThicknessHelper::FromLengths(
            6, 0, kTaskbarStatsExplicitColumnRightGap, 0));
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
    if (FindTaskbarStatsChild(parent, existingRoot, existingRootIndex)) {
        if (HasCurrentTaskbarStatsChild(existingRoot)) {
            Wh_Log(L"TaskbarStatsRoot already exists for this taskbar parent");
            return true;
        }

        auto existingElement = existingRoot.try_as<wux::FrameworkElement>();
        auto replacementRoot = MakeTaskbarStatsRoot();
        if (existingElement) {
            wuxc::Grid::SetColumn(replacementRoot,
                                  wuxc::Grid::GetColumn(existingElement));
            wuxc::Grid::SetRow(replacementRoot,
                               wuxc::Grid::GetRow(existingElement));
        }
        ApplyTaskbarStatsAnchorMargin(replacementRoot, parent, element);

        wuxc::Canvas::SetZIndex(replacementRoot, 10000);

        auto children = parent.Children();
        children.RemoveAt(existingRootIndex);
        children.InsertAt(existingRootIndex, replacementRoot.as<wux::UIElement>());
        auto timer = StartTaskbarStatsTimer(
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

        Wh_Log(L"Replaced stale TaskbarStatsRoot with current layout");
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
    ApplyTaskbarStatsAnchorMargin(root, parent, element);

    // Prefer a real auto-width taskbar grid slot before the system tray when
    // explicit columns exist. Otherwise use right alignment with tray margin,
    // because adding a first column to the no-column parent centers the module.
    wuxc::Grid::SetColumn(root, targetColumn);
    wuxc::Grid::SetRow(root, wuxc::Grid::GetRow(element));
    wuxc::Canvas::SetZIndex(root, 10000);

    children.InsertAt(trayChildIndex, root.as<wux::UIElement>());
    auto timer = StartTaskbarStatsTimer(root.as<wux::UIElement>(), parent, element);

    g_insertedModules.push_back(InsertedModule{
        .anchorHandle = handle,
        .parent = parent,
        .trayElement = element,
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
    Wh_Log(L"TaskbarStats after init");
    ScheduleDelayedInitialization(3000);
}

void Wh_ModUninit() {
    if (g_uninitializing.exchange(true)) {
        return;
    }

    Wh_Log(L"TaskbarStats uninit");

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
