using System.Text.Json;
using TaskbarWidgets.Loader;
using TaskbarWidgets.Loader.Core;

var failures = new List<string>();
Run("legacy config migration", TestLegacyMigration);
Run("unknown widget preservation", TestUnknownWidgetPreservation);
Run("atomic snapshot write", TestAtomicWrite);
Run("command allowlist", TestCommandValidation);
Run("widget position command", TestWidgetPositionCommand);
Run("updater asset selection", TestUpdaterAssetSelection);
Run("system metric math", TestSystemMetricMath);
Run("system meter settings reset", TestSystemMeterSettingsReset);
Run("system PDH sampler", TestSystemPdhSampler);

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
    Assert(config.ConfigVersion == 2, "configVersion");
    Assert(config.Layout.Mode == "rotation", "layout mode");
    Assert(config.Rotation.IntervalSeconds == 5, "rotation minimum");
    Assert(config.Widgets.All(widget => widget.Id != "btc-fees"), "crypto removal");
    Assert(config.Widgets.Single(widget => widget.Id == "future-widget").Enabled == false, "unknown widget disabled");
    var weather = config.Widgets.Single(widget => widget.Id == "weather-static");
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
    var stale = valid with { CreatedAtUnix = now - 301 };
    Assert(WidgetCommandValidator.IsValid(valid, now), "valid command rejected");
    Assert(!WidgetCommandValidator.IsValid(invalidAction, now), "unknown command accepted");
    Assert(WidgetCommandValidator.IsValid(taskManager, now), "Task Manager command rejected");
    Assert(WidgetCommandValidator.IsValid(moveWidget, now), "widget move command rejected");
    Assert(!WidgetCommandValidator.IsValid(stale, now), "stale command accepted");
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

void Assert(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
