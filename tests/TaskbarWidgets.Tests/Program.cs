global using System.IO;
global using System.Net.Http;

using System.Text.Json;
using System.Reflection;
using System.Runtime.InteropServices;
using TaskbarWidgets.Loader;
using TaskbarWidgets.Loader.Core;

var failures = new List<string>();
Run("legacy config migration", TestLegacyMigration);
Run("unknown widget preservation", TestUnknownWidgetPreservation);
Run("atomic snapshot write", TestAtomicWrite);
Run("command allowlist", TestCommandValidation);
Run("native media controls", TestMediaControlContract);
Run("widget position command", TestWidgetPositionCommand);
Run("updater asset selection", TestUpdaterAssetSelection);
Run("system metric math", TestSystemMetricMath);
Run("worldwide weather location query", TestWeatherLocationQuery);
Run("English weather day labels", TestWeatherDayLabels);
Run("system meter settings reset", TestSystemMeterSettingsReset);
Run("system PDH sampler", TestSystemPdhSampler);
Run("community widget validation", TestCommunityWidgetValidation);
Run("web widget validation", TestWebWidgetValidation);
Run("native expanded widget validation", TestNativeWidgetValidation);
Run("schema v4 permission and process validation", TestSchemaV4Validation);
Run("cross-runtime content hash contract", TestContentHashContract);
Run("full-access json-lines protocol", TestFullAccessProtocolSerialization);
Run("community widget update version", TestCommunityWidgetUpdateVersion);
Run("unsafe instance id normalization", TestUnsafeInstanceIdNormalization);
Run("notification icon native entry point", TestNotificationIconEntryPoint);
Run("notification icon Explorer recovery", TestNotificationIconExplorerRecovery);
Run("taskbar UI Automation button scan", TestTaskbarUiaButtonScanContract);
Run("local Discord voice detection", TestLocalDiscordVoiceDetectionContract);
Run("Parking Lot native drag and drop contract", TestParkingLotContract);

if (failures.Count > 0)
{
    Console.Error.WriteLine(string.Join(Environment.NewLine, failures));
    return 1;
}

Console.WriteLine("All Taskbar Widgets contract tests passed.");
return 0;

void Run(string name, Action test)
{
    try
    {
        test();
        Console.WriteLine($"PASS {name}");
    }
    catch (Exception ex)
    {
        failures.Add($"FAIL {name}: {ex.Message}");
    }
}

void TestNotificationIconEntryPoint()
{
    var method = typeof(NotificationAreaIcon).GetMethod(
        "ShellNotifyIcon",
        BindingFlags.NonPublic | BindingFlags.Static);
    var import = method?.GetCustomAttribute<DllImportAttribute>();
    Assert(import is not null, "ShellNotifyIcon import missing");
    Assert(import!.EntryPoint == "Shell_NotifyIconW", "wrong Shell_NotifyIcon entry point");
    Assert(import.ExactSpelling, "Shell_NotifyIcon import must use exact spelling");
}

void TestWeatherLocationQuery()
{
    var url = WeatherLocationQuery.Build(" Berlin ", "de");
    Assert(url.Contains("name=Berlin", StringComparison.Ordinal),
        "weather city is not URL encoded");
    Assert(url.Contains("language=de", StringComparison.Ordinal),
        "weather query does not use the requested language");
    Assert(!url.Contains("countryCode", StringComparison.OrdinalIgnoreCase),
        "weather query must not be restricted to a single country");

    var escaped = WeatherLocationQuery.Build("Sao Paulo/Brazil", "invalid-language");
    Assert(escaped.Contains("name=Sao%20Paulo%2FBrazil", StringComparison.Ordinal),
        "weather city special characters are not URL encoded");
    Assert(escaped.Contains("language=en", StringComparison.Ordinal),
        "invalid weather language must fall back to English");
}

void TestWeatherDayLabels()
{
    var monday = new DateTime(2026, 8, 3);
    var expected = new[] { "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun" };
    var actual = Enumerable.Range(0, expected.Length)
        .Select(offset => WeatherDayLabel.Format(monday.AddDays(offset), isToday: false))
        .ToArray();

    Assert(actual.SequenceEqual(expected),
        $"weather weekday labels must be English: {string.Join(", ", actual)}");
    Assert(WeatherDayLabel.Format(monday, isToday: true) == "Today",
        "current weather day label must be Today");
}

void TestParkingLotContract()
{
    var manifestPath = Path.Combine(
        Directory.GetCurrentDirectory(), "widgets", "parking-lot", "widget.json");
    Assert(File.Exists(manifestPath), "Parking Lot manifest missing");
    using var manifest = JsonDocument.Parse(File.ReadAllText(manifestPath));
    Assert(manifest.RootElement.GetProperty("id").GetString() == "parking-lot",
        "Parking Lot manifest id mismatch");
    Assert(manifest.RootElement.GetProperty("defaultSize").GetProperty("width").GetInt32() == 64,
        "Parking Lot compact width changed");
    Assert(manifest.RootElement.GetProperty("defaultSize").GetProperty("height").GetInt32() == 32,
        "Parking Lot compact height changed");

    var sourcePath = Path.Combine(
        Directory.GetCurrentDirectory(), "src", "native", "taskbar-hook",
        "taskbar_widgets_hook.cpp");
    var source = File.ReadAllText(sourcePath);
    Assert(source.Contains("DRAG\\nHERE", StringComparison.Ordinal),
        "Parking Lot empty prompt missing");
    Assert(source.Contains("ReArmParkingLotDropTarget(root)", StringComparison.Ordinal),
        "Parking Lot clear transition does not re-arm its drop target");
    Assert(source.Contains("StandardDataFormats::StorageItems", StringComparison.Ordinal),
        "Parking Lot does not accept files and folders");
    Assert(source.Contains("StandardDataFormats::Text", StringComparison.Ordinal) &&
           source.Contains("StandardDataFormats::WebLink", StringComparison.Ordinal),
        "Parking Lot text/link formats missing");
    Assert(source.Contains("SetStorageItems", StringComparison.Ordinal),
        "Parking Lot cannot export parked files");
}

void TestNotificationIconExplorerRecovery()
{
    var sourcePath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "src", "loader", "NotificationAreaIcon.cs");
    Assert(File.Exists(sourcePath), "notification icon source missing");
    var source = File.ReadAllText(sourcePath);
    Assert(source.Contains("RegisterWindowMessage(\"TaskbarCreated\")", StringComparison.Ordinal),
        "Explorer taskbar recreation message is not registered");
    Assert(source.Contains("message == _taskbarCreatedMessage", StringComparison.Ordinal),
        "Explorer taskbar recreation message is not handled");
    Assert(source.Contains("ShellNotifyIcon(NimModify", StringComparison.Ordinal),
        "notification icon registration health is not checked");
    Assert(source.Contains("RegistrationHealthTimerId", StringComparison.Ordinal),
        "notification icon registration retry timer is missing");
}

void TestTaskbarUiaButtonScanContract()
{
    var sourcePath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "src", "native", "taskbar-hook", "taskbar_widgets_hook.cpp");
    Assert(File.Exists(sourcePath), "native hook source missing");
    var source = File.ReadAllText(sourcePath);
    Assert(source.Contains("ElementFromHandle(taskbar", StringComparison.Ordinal),
        "Shell_TrayWnd is not used as the UIA root");
    Assert(source.Contains("UIA_ButtonControlTypeId", StringComparison.Ordinal),
        "UIA scan is not filtering Button controls");
    Assert(source.Contains("TreeScope_Descendants", StringComparison.Ordinal),
        "UIA scan does not include descendant app buttons");
    Assert(source.Contains("get_CurrentBoundingRectangle", StringComparison.Ordinal),
        "UIA button geometry is not read");
}

void TestLocalDiscordVoiceDetectionContract()
{
    Assert(
        DiscordLocalVoiceProbe.ParseActiveChannelName("Voice Room | Example Server - Discord") ==
        "Voice Room",
        "Discord channel title parsing failed");
    Assert(
        DiscordLocalVoiceProbe.ParseActiveChannelName("Discord") == "Discord",
        "plain Discord title parsing failed");
    Assert(
        DiscordLocalVoiceProbe.StableUserId("Example User") ==
        DiscordLocalVoiceProbe.StableUserId("example user"),
        "local Discord user id is not case stable");
    Assert(
        DiscordLocalVoiceProbe.StableUserId("Example User") !=
        DiscordLocalVoiceProbe.StableUserId("Example User", 1),
        "duplicate local Discord users collide");
    var webRtc = DiscordWebRtcLogProbe.Parse("""
        [2026-08-01 16:54:39.294] [1] (connection.cpp:1325): [default] Inbound stats for user: 123, audio ssrc: 10, packets received: 44, lost: 0, audio frames normal: 22, silent: 100, expand: 0,
        [2026-08-01 16:54:39.294] [1] (connection.cpp:1363): [default] Outbound audio stats for user: 456, audio ssrc: 11, packets sent: 55, packets skipped: 2,
        """);
    Assert(webRtc.Users.Select(user => user.Id).SequenceEqual(["123", "456"]),
        "Discord WebRTC user order is not parsed");
    Assert(webRtc.Users[1].Local, "local Discord WebRTC user is not identified");
    Assert(webRtc.Users.Select(user => user.Ssrc).SequenceEqual([10u, 11u]),
        "Discord WebRTC SSRC mapping is not parsed");

    var workerPath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "widgets", "discord-voice", "provider", "DiscordVoiceWorker.cs");
    var manifestPath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "widgets", "discord-voice", "widget.json");
    var worker = File.ReadAllText(workerPath);
    var manifest = File.ReadAllText(manifestPath);
    Assert(!worker.Contains("discord-ipc", StringComparison.OrdinalIgnoreCase),
        "official Discord RPC pipe remains in the worker");
    Assert(!worker.Contains("AUTHORIZE", StringComparison.Ordinal),
        "official Discord OAuth flow remains in the worker");
    Assert(!manifest.Contains("clientSecret", StringComparison.Ordinal),
        "Discord client secret remains in widget settings");

    var nativePath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "src", "native", "taskbar-hook", "taskbar_widgets_hook.cpp");
    var native = File.ReadAllText(nativePath);
    Assert(!native.Contains("user.speaking ? 1.0 : 0.38", StringComparison.Ordinal),
        "inactive Discord avatars are still dimmed");
    var localProbePath = Path.Combine(
        Directory.GetCurrentDirectory(),
        "widgets", "discord-voice", "provider", "DiscordLocalVoiceProbe.cs");
    var localProbe = File.ReadAllText(localProbePath);
    Assert(!localProbe.Contains("GetPixel", StringComparison.Ordinal) &&
           !localProbe.Contains("BitBlt", StringComparison.Ordinal) &&
           !localProbe.Contains("CaptureAvatar", StringComparison.Ordinal),
        "Discord monitor-pixel capture remains in the provider");
    Assert(worker.Contains("Muted = user.Muted", StringComparison.Ordinal),
        "Discord mute state is not exported");
    Assert(worker.Contains("Streaming = user.Streaming", StringComparison.Ordinal),
        "Discord stream state is not exported");
    Assert(native.Contains("user.muted || user.deafened", StringComparison.Ordinal),
        "Discord mute/deafen state is not rendered");
    Assert(native.Contains("TaskbarWidgetsDiscordChannelName", StringComparison.Ordinal) &&
           native.Contains("discordDisplayMode", StringComparison.Ordinal),
        "Discord voice-room theme is missing");
}

void TestLegacyMigration()
{
    using var document = JsonDocument.Parse("""
    {
      "activeDesign": "btc-fees",
      "rotationEnabled": true,
      "rotationIntervalSecs": 3,
      "rotationDesigns": ["btc-fees", "weather-static"],
      "widgets": [
        { "id": "btc-fees", "design": "btc-fees", "enabled": true, "order": 0 },
        { "id": "future-widget", "design": "future-widget", "enabled": true, "order": 1 },
        { "id": "weather-static", "design": "weather-static", "enabled": true, "moveX": 12, "positionPct": 65, "order": 2 }
      ]
    }
    """);
    var config = WidgetConfiguration.FromLegacy(document.RootElement);
    Assert(config.ConfigVersion == 3, "configVersion");
    Assert(config.Layout.Mode == "rotation", "layout mode");
    Assert(config.Rotation.IntervalSeconds == 5, "rotation minimum");
    Assert(config.Widgets.All(widget => widget.Id != "btc-fees"), "crypto removal");
    Assert(config.Widgets.Single(widget => widget.Id == "future-widget").Enabled == false, "unknown widget disabled");
    var weather = config.Widgets.Single(widget => widget.Id == "weather-static");
    Assert(weather.WidgetId == "weather-static" && weather.InstanceId == "weather-static", "v3 widget identity migration");
    Assert(weather.Position.AnchorPercent == 65 && weather.Position.OffsetPx == 12, "position migration");
    Assert(config.Widgets.Single(widget => widget.Id == "system-cpu").Enabled == false, "new system widget default");
}

void TestCommandValidation()
{
    var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    var valid = new WidgetCommand(1, "test", null, "openSettings", null, now);
    var invalidAction = valid with { Action = "runArbitraryProcess" };
    var taskManager = valid with { Action = "openTaskManager", WidgetId = "system-cpu" };
    var moveWidget = valid with { Action = "moveWidget", WidgetId = "weather-static" };
    var mediaPrevious = valid with { Action = "mediaPrevious", WidgetId = "media-player" };
    var mediaNext = valid with { Action = "mediaNext", WidgetId = "media-player" };
    var stale = valid with { CreatedAtUnix = now - 301 };
    Assert(WidgetCommandValidator.IsValid(valid, now), "valid command rejected");
    Assert(!WidgetCommandValidator.IsValid(invalidAction, now), "unknown command accepted");
    Assert(WidgetCommandValidator.IsValid(taskManager, now), "Task Manager command rejected");
    Assert(WidgetCommandValidator.IsValid(moveWidget, now), "widget move command rejected");
    Assert(WidgetCommandValidator.IsValid(mediaPrevious, now), "previous media command rejected");
    Assert(WidgetCommandValidator.IsValid(mediaNext, now), "next media command rejected");
    Assert(!WidgetCommandValidator.IsValid(stale, now), "stale command accepted");
}

void TestMediaControlContract()
{
    var manifestPath = Path.Combine(
        Directory.GetCurrentDirectory(), "widgets", "media-player", "widget.json");
    using var manifest = JsonDocument.Parse(File.ReadAllText(manifestPath));
    Assert(manifest.RootElement.GetProperty("defaultSize").GetProperty("width").GetInt32() == 292,
        "media widget should use the requested 30 percent narrower width");
    var settings = manifest.RootElement.GetProperty("settings")
        .EnumerateArray()
        .Select(item => item.GetProperty("key").GetString())
        .ToHashSet(StringComparer.Ordinal);
    Assert(settings.Contains("showControls"), "media control setting missing");
    Assert(settings.Contains("showVisualizer"), "media visualizer setting missing");
    Assert(settings.Contains("visualizerBarCount"), "media visualizer bar count missing");
    Assert(settings.Contains("visualizerCentered"), "centered visualizer setting missing");
    Assert(settings.Contains("visualizerBaseline"), "visualizer baseline setting missing");
    Assert(settings.Contains("visualizerSensitivity"), "visualizer sensitivity setting missing");
    Assert(settings.Contains("visualizerPeakLevel"), "visualizer peak setting missing");
    Assert(settings.Contains("controlsPosition"), "media controls position missing");
    Assert(settings.Contains("visualizerPosition"), "media visualizer position missing");
    Assert(settings.Contains("showPauseOverlay"), "media pause overlay setting missing");

    var native = File.ReadAllText(Path.Combine(
        Directory.GetCurrentDirectory(), "src", "native", "taskbar-hook",
        "taskbar_widgets_hook.cpp"));
    Assert(native.Contains("TaskbarWidgetsMediaPreviousButton", StringComparison.Ordinal),
        "native previous media button missing");
    Assert(native.Contains("TaskbarWidgetsMediaNextButton", StringComparison.Ordinal),
        "native next media button missing");
    Assert(native.Contains("TaskbarWidgetsMediaCard", StringComparison.Ordinal),
        "single-layer media card missing");
    Assert(native.Contains("TaskbarWidgetsMediaVisualizerCard", StringComparison.Ordinal),
        "separate Fluent visualizer card missing");
    Assert(native.Contains("TaskbarWidgetsMediaControlTint", StringComparison.Ordinal) &&
           native.Contains("MakeMediaControlsGradientBrush", StringComparison.Ordinal),
        "album-color transport gradient layer missing");
    Assert(native.Contains("controlTint.Width(166)", StringComparison.Ordinal) &&
           native.Contains("appendStop(0.18, 0x00)", StringComparison.Ordinal) &&
           native.Contains("darkMode ? 0x78 : 0x58", StringComparison.Ordinal),
        "transport gradient must extend visibly left from the media controls");
    Assert(native.Contains("L\"\\x23EE\"", StringComparison.Ordinal) &&
           native.Contains("L\"\\x25B6\"", StringComparison.Ordinal) &&
           native.Contains("L\"\\x23ED\"", StringComparison.Ordinal),
        "solid transport media glyphs missing");
    Assert(!native.Contains("TaskbarWidgetsMediaBackdrop", StringComparison.Ordinal),
        "media album-art backdrop layer must not be rendered");
    Assert(native.Contains("mediaCard.Background(MakeBrush(0x00", StringComparison.Ordinal),
        "Fluent media surface must be transparent until hover");
    Assert(native.Contains("playButton.Background(MakeBrush(0x01", StringComparison.Ordinal),
        "Fluent transport play button must not use a permanent accent circle");
    Assert(native.Contains("const float db = raw * 100.0f - 100.0f", StringComparison.Ordinal),
        "visualizer dB frame must be decoded exactly once");
    Assert(native.Contains("height < 0.15 ? 0.0", StringComparison.Ordinal),
        "silent visualizer bars must disappear instead of drawing a dotted floor");
    Assert(native.Contains("const double targetHeight = hasAudio ? level * 32.0", StringComparison.Ordinal),
        "media visualizer must retain its full physical bar height");
    Assert(native.Contains("AnimateMediaWave", StringComparison.Ordinal),
        "native media wave animation missing");
    Assert(native.Contains("OpenFileMappingW", StringComparison.Ordinal),
        "native visualizer shared-memory reader missing");
    Assert(native.Contains("FILE_MAP_READ", StringComparison.Ordinal),
        "visualizer mapping should remain read-only inside Explorer");
    Assert(native.Contains("try_as<wuxc::Canvas>()", StringComparison.Ordinal),
        "media layout must use a stable Canvas instead of reparenting children");
    Assert(native.Contains("TaskbarWidgetsMediaWaveBar20", StringComparison.Ordinal) ||
           native.Contains("index < 20", StringComparison.Ordinal),
        "native 20-bar visualizer capacity missing");

    var helper = File.ReadAllText(Path.Combine(
        Directory.GetCurrentDirectory(), "src", "native", "media-helper",
        "media_helper.cpp"));
    Assert(helper.Contains("TrySkipPreviousAsync", StringComparison.Ordinal),
        "GSMTC previous command missing");
    Assert(helper.Contains("TrySkipNextAsync", StringComparison.Ordinal),
        "GSMTC next command missing");
    Assert(helper.Contains("AUDCLNT_STREAMFLAGS_LOOPBACK", StringComparison.Ordinal),
        "WASAPI loopback capture missing");
    Assert(helper.Contains("CaptureAudioStream(writer, 0)", StringComparison.Ordinal),
        "visualizer must use FluentFlyout-style render endpoint loopback");
    Assert(helper.Contains("FindMediaAudioSessionProcessId", StringComparison.Ordinal),
        "media audio-session PID resolver missing");
    Assert(helper.Contains("FindSourceProcessTreeRoot", StringComparison.Ordinal),
        "source-app process tree fallback missing");
    Assert(helper.Contains("g_visualizerMediaPlaying", StringComparison.Ordinal),
        "paused media must gate visualizer capture");
    Assert(helper.Contains("g_visualizerSessionPeak", StringComparison.Ordinal),
        "selected media audio-session peak diagnostics missing");
    Assert(helper.Contains("constexpr float visualizerResponseScale = 0.60f", StringComparison.Ordinal) &&
           helper.Contains("(1.0f / 8.0f) * visualizerResponseScale", StringComparison.Ordinal) &&
           !helper.Contains("static_cast<float>(count)", StringComparison.Ordinal),
        "visualizer FFT response must be reduced by 40 percent without shortening bars");
    Assert(!helper.Contains("automaticGain", StringComparison.Ordinal),
        "FluentFlyout-compatible FFT must not apply a second automatic gain stage");
    Assert(helper.Contains("inputPeak > 0.00032f", StringComparison.Ordinal),
        "visualizer quiet-track noise gate calibration missing");
    Assert(helper.Contains("FastFourierTransform", StringComparison.Ordinal),
        "FFT audio analysis missing");
    Assert(helper.Contains("strongest = std::abs(sample)", StringComparison.Ordinal),
        "multichannel FFT input must preserve the strongest real channel");
    Assert(helper.Contains("progress * 75.0f", StringComparison.Ordinal),
        "FluentFlyout high-frequency visualizer gain is missing");
    Assert(helper.Contains("constexpr std::size_t fftSize = 4096", StringComparison.Ordinal),
        "FluentFlyout 4096-sample FFT window is missing");
    Assert(helper.Contains("CreateFileMappingW", StringComparison.Ordinal),
        "visualizer shared-memory writer missing");
    Assert(helper.Contains("return SendMediaCommand(MediaCommand::Toggle) ? 0 : 2", StringComparison.Ordinal),
        "media toggle must report whether GSMTC accepted the command");
    Assert(helper.Contains("visualizerCaptureReady", StringComparison.Ordinal),
        "visualizer capture diagnostics missing");
    Assert(!helper.Contains("void SendMediaCommand", StringComparison.Ordinal),
        "media command helper must not hide rejected commands from fallback");

    var shared = File.ReadAllText(Path.Combine(
        Directory.GetCurrentDirectory(), "src", "native", "common",
        "media_visualizer_shared.h"));
    Assert(!shared.Contains("InterlockedCompareExchange64(", StringComparison.Ordinal),
        "read-only visualizer mapping must not use a read-modify-write interlocked read");
    Assert(shared.Contains("const LONG64 before = source->sequence", StringComparison.Ordinal),
        "aligned read-only visualizer sequence load missing");
    Assert(shared.Contains("MediaVisualizer.v6", StringComparison.Ordinal),
        "visualizer frame semantic changes require a new mapping version");
    Assert(!native.Contains("StartMediaEntranceAnimation", StringComparison.Ordinal),
        "global media entrance animation is unsafe across multiple monitors");
}

void TestWidgetPositionCommand()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-position-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var path = Path.Combine(directory, "config.json");
        var config = new WidgetConfiguration();
        var weather = config.Widgets.Single(widget => widget.Id == "weather-static");
        weather.Enabled = true;
        weather.Settings["city"] = "Istanbul";
        config.Save(path);

        Assert(WidgetPositionCommandHandler.TryApply(path, "weather-static", 37, -4), "valid position rejected");
        var saved = WidgetConfiguration.LoadOrCreate(path);
        var moved = saved.Widgets.Single(widget => widget.Id == "weather-static");
        Assert(moved.Position.AnchorPercent == 37 && moved.Position.OffsetPx == -4, "position not persisted");
        Assert(moved.Enabled && moved.Settings["city"]?.GetValue<string>() == "Istanbul", "unrelated widget state changed");
        Assert(!WidgetPositionCommandHandler.TryApply(path, "unknown-widget", 50, 0), "unknown widget accepted");
        Assert(!WidgetPositionCommandHandler.TryApply(path, "weather-static", 101, 0), "invalid percent accepted");
        Assert(!WidgetPositionCommandHandler.TryApply(path, "weather-static", 50, 641), "invalid offset accepted");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestUnknownWidgetPreservation()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-test-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var path = Path.Combine(directory, "config.json");
        File.WriteAllText(path, """
        {
          "configVersion": 2,
          "layout": { "mode": "row" },
          "widgets": [{ "id": "future-widget", "enabled": true, "order": 0, "position": { "anchorPercent": 500, "offsetPx": 9000 }, "settings": {} }],
          "rotation": { "intervalSeconds": 1, "widgetIds": ["future-widget", "codex-status"] }
        }
        """);
        var config = WidgetConfiguration.LoadOrCreate(path);
        var unknown = config.Widgets.Single();
        Assert(unknown.Id == "future-widget" && !unknown.Enabled, "unknown record not preserved disabled");
        Assert(unknown.Position.AnchorPercent == 100 && unknown.Position.OffsetPx == 4000, "position not clamped");
        Assert(config.Rotation.WidgetIds.SequenceEqual(["codex-status"]), "unknown rotation id retained");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestAtomicWrite()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-test-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var path = Path.Combine(directory, "snapshot.json");
        AtomicJson.Write(path, new { schemaVersion = 1, value = 42 }, WidgetConfiguration.JsonOptions());
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        Assert(document.RootElement.GetProperty("value").GetInt32() == 42, "atomic output invalid");
        Assert(Directory.GetFiles(directory, "*.tmp").Length == 0, "temporary file left behind");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestUpdaterAssetSelection()
{
    var partial = ReleaseAssetPolicy.Select([
        ("TaskbarWidgets.exe", "raw"),
        ("TaskbarWidgets-portable-x64.zip", "portable")
    ]);
    Assert(partial is null, "partial product selected as updater");
    var full = ReleaseAssetPolicy.Select([
        (ReleaseAssetPolicy.SetupSha256Name, "sha"),
        (ReleaseAssetPolicy.SetupName, "setup")
    ]);
    Assert(full?.DownloadUrl == "setup" && full?.Sha256Url == "sha",
        "new updater did not prefer the Taskbar Widgets setup asset");
}

void TestSystemMetricMath()
{
    Assert(SystemMetricMath.ComputeRate(1_000, 2_000, 2) == 500, "rate calculation");
    Assert(SystemMetricMath.ComputeRate(2_000, 1_000, 1) == 0, "counter reset");
    Assert(SystemMetricMath.ComputeRate(1_000, 2_000, 0) == 0, "zero duration");
    Assert(SystemMetricMath.ClampPercent(125) == 100, "upper percent clamp");
    Assert(SystemMetricMath.ClampPercent(-5) == 0, "lower percent clamp");
    Assert(SystemMetricMath.NormalizeRefreshSeconds(7.04) == 7, "refresh rounding");
    Assert(SystemMetricMath.NormalizeRefreshSeconds(0) == 3, "refresh fallback");
    Assert(SystemMetricMath.NormalizeRefreshSeconds(double.NaN) == 3, "non-finite refresh fallback");
    Assert(SystemMetricMath.ComputeLinkUtilization(12_500_000, 1_000_000_000, true, 1) == 10, "automatic link utilization");
    Assert(SystemMetricMath.ComputeLinkUtilization(50_000, 0, false, 100) == 50, "manual link utilization");
}

void TestSystemMeterSettingsReset()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-meter-test-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var path = Path.Combine(directory, "config.json");
        File.WriteAllText(path, """
        {
          "configVersion": 2,
          "layout": { "mode": "rotation" },
          "widgets": [
            { "id": "codex-status", "enabled": true, "order": 0, "position": { "anchorPercent": 40, "offsetPx": 7 }, "settings": { "projectFilter": "keep" } },
            { "id": "system-cpu", "enabled": true, "order": 8, "position": { "anchorPercent": 73, "offsetPx": -12 }, "settings": { "displayMode": "text", "primaryColor": "#123456" } }
          ],
          "rotation": { "intervalSeconds": 30, "widgetIds": ["codex-status", "system-cpu"] }
        }
        """);
        var config = WidgetConfiguration.LoadOrCreate(path);
        var cpu = config.Widgets.Single(widget => widget.Id == "system-cpu");
        Assert(cpu.Enabled && cpu.Order == 8, "system widget enable/order changed");
        Assert(cpu.Position.AnchorPercent == 73 && cpu.Position.OffsetPx == -12, "system widget position changed");
        Assert(cpu.Settings["meterStyleVersion"]?.GetValue<int>() == 1, "meter style version missing");
        Assert(cpu.Settings["displayMode"]?.GetValue<string>() == "bar", "CPU defaults not reset");
        Assert(!cpu.Settings.ContainsKey("primaryColor"), "legacy system setting retained");
        var codex = config.Widgets.Single(widget => widget.Id == "codex-status");
        Assert(codex.Settings["projectFilter"]?.GetValue<string>() == "keep", "unrelated widget settings changed");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestSystemPdhSampler()
{
    using var sampler = new PdhSampler();
    Thread.Sleep(1100);
    var sample = sampler.ReadSystemCounters();
    Assert(sample.Cpu.TotalPercent is >= 0 and <= 100, "CPU percentage range");
    Assert(sample.Cpu.Cores.Count > 0 && sample.Cpu.Cores.All(core => core.Percent is >= 0 and <= 100), "per-core CPU counters missing");
    Assert(sample.Cpu.Cores.All(core => core.UserPercent is >= 0 and <= 100 && core.KernelPercent is >= 0 and <= 100),
        "per-core user/kernel counters missing");
    Assert(sample.Storage.Disks.Any(disk => disk.Id == "_Total"), "total disk counter missing");
    Assert(sample.Storage.Disks.All(disk => disk.ReadBytesPerSecond >= 0 && disk.WriteBytesPerSecond >= 0), "negative disk rate");
    var network = WindowsNetworkTable.Read();
    Assert(network.All(adapter => !string.IsNullOrWhiteSpace(adapter.Id) && !string.IsNullOrWhiteSpace(adapter.Name)), "invalid network adapter");
    var combined = new SystemMetricsSampler();
    Thread.Sleep(1100);
    var combinedSample = combined.GetSample(1);
    Assert(combinedSample.Memory.TotalBytes > 0 && combinedSample.Memory.UsedBytes <= combinedSample.Memory.TotalBytes, "invalid memory sample");
    Assert(combinedSample.Network.Interfaces.All(adapter => adapter.ReceiveBytesPerSecond >= 0 && adapter.SendBytesPerSecond >= 0), "negative network rate");
}

void TestCommunityWidgetValidation()
{
    var directory = Path.Combine(Path.GetTempPath(), $"com.example.test-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var id = Path.GetFileName(directory).ToLowerInvariant();
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 2,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.4.0",
          "displayName": "Test",
          "description": "Test widget",
          "author": { "name": "Test Author", "website": "https://example.com" },
          "size": { "width": 96, "height": 24 },
          "entry": { "layout": "layout.json", "provider": { "type": "clock" } },
          "permissions": {},
          "settings": []
        }
        """);
        File.WriteAllText(Path.Combine(directory, "layout.json"), """
        { "type": "row", "children": [{ "type": "text", "bind": "data.time" }] }
        """);
        var valid = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(valid.Valid && valid.Id == id, valid.Error ?? "valid package rejected");

        File.WriteAllText(Path.Combine(directory, "layout.json"), """
        { "type": "image", "bind": "../../secret" }
        """);
        var invalid = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!invalid.Valid && invalid.Error?.Contains("Bindings must start") == true,
            "unsafe binding accepted");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestCommunityWidgetUpdateVersion()
{
    Assert(CommunityWidgetUpdateChecker.IsNewerVersion("1.0.1", "1.0.0"), "patch update not detected");
    Assert(CommunityWidgetUpdateChecker.IsNewerVersion("1.10.0", "1.9.9"), "numeric minor update not detected");
    Assert(!CommunityWidgetUpdateChecker.IsNewerVersion("1.0.0", "1.0.0"), "same version accepted as update");
    Assert(!CommunityWidgetUpdateChecker.IsNewerVersion("invalid", "1.0.0"), "invalid version accepted");
}

void TestWebWidgetValidation()
{
    var directory = Path.Combine(Path.GetTempPath(), $"com.example.web-{Guid.NewGuid():N}");
    Directory.CreateDirectory(Path.Combine(directory, "ui"));
    try
    {
        var id = Path.GetFileName(directory).ToLowerInvariant();
        File.WriteAllText(Path.Combine(directory, "ui", "index.html"), "<!doctype html><title>Safe</title>");
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 3,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.0",
          "displayName": "Web Test",
          "description": "Sandbox test widget",
          "author": { "name": "Test Author", "website": "https://example.com" },
          "size": { "width": 170, "height": 32 },
          "renderer": {
            "type": "web",
            "entry": "ui/index.html",
            "expandedSize": { "width": 360, "height": 180 },
            "activation": "hover"
          },
          "entry": { "provider": { "type": "clock", "refreshSeconds": 1 } },
          "permissions": { "storage": true },
          "settings": []
        }
        """);
        var valid = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(valid.Valid && valid.Renderer == "web", valid.Error ?? "valid web package rejected");
        Assert(valid.WebRenderer?.ExpandedWidth == 360, "expanded web size lost");

        File.WriteAllText(Path.Combine(directory, "widget.json"),
            File.ReadAllText(Path.Combine(directory, "widget.json"))
                .Replace("ui/index.html", "../outside.html"));
        var traversal = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!traversal.Valid && traversal.Error?.Contains("relative", StringComparison.OrdinalIgnoreCase) == true,
            "web entry path traversal accepted");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestNativeWidgetValidation()
{
    var directory = Path.Combine(
        Path.GetTempPath(), $"com.example.native-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var id = Path.GetFileName(directory).ToLowerInvariant();
        File.WriteAllText(Path.Combine(directory, "compact.json"), """
        { "type": "row", "children": [{ "type": "text", "text": "Native" }] }
        """);
        File.WriteAllText(Path.Combine(directory, "expanded.json"), """
        {
          "type": "card",
          "children": [
            { "type": "text", "bind": "data.value" },
            { "type": "button", "label": "Close", "action": "$close" }
          ]
        }
        """);
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 4,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.4",
          "displayName": "Native Test",
          "description": "Validates native compact and expanded layouts.",
          "author": { "name": "Test" },
          "size": { "width": 180, "height": 32 },
          "renderer": {
            "type": "native",
            "entry": "compact.json",
            "expandedEntry": "expanded.json",
            "expandedSize": { "width": 420, "height": 220 }
          },
          "permissions": { "required": [], "optional": [] }
        }
        """);
        var valid = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(valid.Valid && valid.Renderer == "native",
            valid.Error ?? "native widget rejected");
        Assert(valid.NativeRenderer?.ExpandedWidth == 420,
            "native expanded size lost");
        Assert(valid.ExpandedLayout?["type"]?.GetValue<string>() == "card",
            "native expanded layout lost");

        File.WriteAllText(Path.Combine(directory, "expanded.json"), """
        { "type": "button", "label": "Unsafe", "action": "../../run" }
        """);
        var unsafeAction = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!unsafeAction.Valid &&
               unsafeAction.Error?.Contains("safe action", StringComparison.OrdinalIgnoreCase) == true,
            "unsafe native button action accepted");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestSchemaV4Validation()
{
    var directory = Path.Combine(Path.GetTempPath(), $"com.example.fullaccess-{Guid.NewGuid():N}");
    Directory.CreateDirectory(Path.Combine(directory, "ui"));
    try
    {
        var id = Path.GetFileName(directory).ToLowerInvariant();
        File.WriteAllText(Path.Combine(directory, "ui", "index.html"), "<!doctype html><title>V4</title>");
        File.WriteAllText(Path.Combine(directory, "provider.ps1"), "Write-Output '{}'");
        string Manifest(string required, string runAs = "user", string optional = "") => $$"""
        {
          "schemaVersion": 4,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.0",
          "displayName": "Full Access Test",
          "description": "Validates schema v4 permissions.",
          "author": { "name": "Test Author", "website": "https://example.com" },
          "size": { "width": 170, "height": 32 },
          "renderer": { "type": "web", "entry": "ui/index.html" },
          "runtime": {
            "type": "process",
            "entry": "provider.ps1",
            "protocol": "{{(runAs == "administrator" ? "none" : "json-lines-v1")}}",
            "runAs": "{{runAs}}"
          },
          "permissions": {
            "required": [{{required}}],
            "optional": [{{optional}}]
          },
          "settings": []
        }
        """;

        File.WriteAllText(Path.Combine(directory, "widget.json"), Manifest("""
        {
          "id": "system.fullAccess",
          "reason": "Runs the provider selected by the package author."
        }
        """));
        var valid = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(valid.Valid && valid.ManifestSchemaVersion == 4, valid.Error ?? "valid v4 package rejected");
        Assert(valid.ProcessRuntime?.Protocol == "json-lines-v1", "v4 process runtime lost");
        Assert(valid.ContentSha256?.Length == 64, "review content hash missing");

        File.WriteAllText(Path.Combine(directory, "widget.json"), Manifest("""
        {
          "id": "system.fullAccess",
          "reason": "Runs the provider selected by the package author."
        }
        """, optional: """
        {
          "id": "network.internet",
          "scope": ["api.example.com"],
          "reason": "Fetches the user's selected public feed."
        }
        """));
        var optionalFullAccess = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!optionalFullAccess.Valid &&
               optionalFullAccess.Error?.Contains("optional permissions", StringComparison.Ordinal) == true,
            "full-access process accepted an unenforceable optional permission");

        File.WriteAllText(Path.Combine(directory, "widget.json"), Manifest("""
        {
          "id": "system.metrics.read",
          "scope": ["cpu"],
          "reason": "Shows CPU usage."
        }
        """));
        var missingFullAccess = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!missingFullAccess.Valid &&
               missingFullAccess.Error?.Contains("system.fullAccess", StringComparison.Ordinal) == true,
            "process runtime accepted without full access permission");

        File.WriteAllText(Path.Combine(directory, "widget.json"), Manifest("""
        {
          "id": "system.fullAccess",
          "reason": "Runs the provider selected by the package author."
        },
        {
          "id": "system.fullAccess",
          "reason": "Duplicate permission must be rejected."
        }
        """));
        var duplicate = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!duplicate.Valid && duplicate.Error?.Contains("more than once", StringComparison.Ordinal) == true,
            "duplicate v4 permission accepted");

        File.WriteAllText(Path.Combine(directory, "widget.json"), Manifest("""
        {
          "id": "system.fullAccess",
          "reason": "Runs the provider selected by the package author."
        }
        """, "administrator"));
        var missingAdministrator = CommunityWidgetRegistry.ValidateForTool(directory);
        Assert(!missingAdministrator.Valid &&
               missingAdministrator.Error?.Contains("system.administrator", StringComparison.Ordinal) == true,
            "administrator runtime accepted without administrator permission");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestContentHashContract()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-hash-{Guid.NewGuid():N}");
    Directory.CreateDirectory(Path.Combine(directory, "sub"));
    try
    {
        File.WriteAllText(Path.Combine(directory, "a.txt"), "alpha");
        File.WriteAllBytes(Path.Combine(directory, "sub", "b.bin"), [0, 1, 255]);
        Assert(
            CommunityWidgetRegistry.ComputeContentHashForTool(directory) ==
            "8a29aa8ea3b60d2ae5f62ea72e7b6cbedcaa1aee31956113610db88072c2caae",
            "C# installed-content hash contract changed");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void TestFullAccessProtocolSerialization()
{
    var message = new System.Text.Json.Nodes.JsonObject
    {
        ["type"] = "initialize",
        ["instances"] = new System.Text.Json.Nodes.JsonArray(
            new System.Text.Json.Nodes.JsonObject
            {
                ["instanceId"] = "com.example.demo",
                ["settings"] = new System.Text.Json.Nodes.JsonObject()
            })
    };
    var line = CommunityFullTrustSupervisor.SerializeProtocolMessageForTool(message);
    Assert(!line.Contains('\r') && !line.Contains('\n'), "json-lines message contains a line break");
    using var document = JsonDocument.Parse(line);
    Assert(document.RootElement.GetProperty("type").GetString() == "initialize",
        "json-lines message is not valid JSON");
}

void TestUnsafeInstanceIdNormalization()
{
    var directory = Path.Combine(Path.GetTempPath(), $"taskbar-widgets-instance-{Guid.NewGuid():N}");
    Directory.CreateDirectory(directory);
    try
    {
        var path = Path.Combine(directory, "config.json");
        File.WriteAllText(path, """
        {
          "configVersion": 3,
          "layout": { "mode": "row" },
          "widgets": [
            { "id": "../escape", "instanceId": "../escape", "widgetId": "weather-static", "enabled": true, "order": 0, "position": { "anchorPercent": 100, "offsetPx": 0 }, "settings": {} },
            { "id": "weather-static", "instanceId": "weather-static", "widgetId": "weather-static", "enabled": true, "order": 1, "position": { "anchorPercent": 100, "offsetPx": 0 }, "settings": {} }
          ],
          "rotation": { "intervalSeconds": 30, "widgetIds": [], "instanceIds": [] }
        }
        """);
        var config = WidgetConfiguration.LoadOrCreate(path);
        Assert(config.Widgets.All(widget => WidgetConfiguration.IsSafeInstanceId(widget.InstanceId)), "unsafe instance id retained");
        Assert(config.Widgets.Select(widget => widget.InstanceId).Distinct(StringComparer.OrdinalIgnoreCase).Count() == 2,
            "duplicate instance ids retained");
    }
    finally
    {
        Directory.Delete(directory, recursive: true);
    }
}

void Assert(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
