using System.Text.Json;
using TaskbarWidgets.Loader.Core;

var failures = new List<string>();
Run("legacy config migration", TestLegacyMigration);
Run("unknown widget preservation", TestUnknownWidgetPreservation);
Run("atomic snapshot write", TestAtomicWrite);
Run("command allowlist", TestCommandValidation);
Run("updater asset selection", TestUpdaterAssetSelection);

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
}

void TestCommandValidation()
{
    var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    var valid = new WidgetCommand(1, "test", null, "openSettings", null, now);
    var invalidAction = valid with { Action = "runArbitraryProcess" };
    var stale = valid with { CreatedAtUnix = now - 301 };
    Assert(WidgetCommandValidator.IsValid(valid, now), "valid command rejected");
    Assert(!WidgetCommandValidator.IsValid(invalidAction, now), "unknown command accepted");
    Assert(!WidgetCommandValidator.IsValid(stale, now), "stale command accepted");
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
        ("TaskbarStatsSetup.exe", "legacy-setup"),
        ("TaskbarStatsSetup.exe.sha256", "legacy-sha"),
        (ReleaseAssetPolicy.SetupSha256Name, "sha"),
        (ReleaseAssetPolicy.SetupName, "setup")
    ]);
    Assert(full?.DownloadUrl == "setup" && full?.Sha256Url == "sha",
        "new updater did not prefer the Taskbar Widgets setup asset");
}

void Assert(bool condition, string message)
{
    if (!condition) throw new InvalidOperationException(message);
}
