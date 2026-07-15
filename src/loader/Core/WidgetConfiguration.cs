using System.Text.Json;
using System.Text.Json.Nodes;

namespace TaskbarWidgets.Loader.Core;

internal sealed class WidgetConfiguration
{
    public int ConfigVersion { get; set; } = 2;
    public LayoutConfiguration Layout { get; set; } = new();
    public List<WidgetInstanceConfiguration> Widgets { get; set; } = DefaultWidgets();
    public RotationConfiguration Rotation { get; set; } = new();

    public static WidgetConfiguration LoadOrCreate(string path)
    {
        if (!File.Exists(path))
        {
            return new WidgetConfiguration();
        }

        using var document = JsonDocument.Parse(File.ReadAllText(path));
        if (document.RootElement.TryGetProperty("configVersion", out var version) &&
            version.GetInt32() == 2)
        {
            var configuration = JsonSerializer.Deserialize<WidgetConfiguration>(
                document.RootElement.GetRawText(), JsonOptions()) ?? new WidgetConfiguration();
            configuration.Normalize();
            return configuration;
        }

        return FromLegacy(document.RootElement);
    }

    private void Normalize()
    {
        Layout.Mode = string.Equals(Layout.Mode, "rotation", StringComparison.OrdinalIgnoreCase)
            ? "rotation"
            : "row";
        Rotation.IntervalSeconds = Math.Clamp(Rotation.IntervalSeconds, 5, 3600);
        foreach (var widget in Widgets)
        {
            widget.Position.AnchorPercent = Math.Clamp(widget.Position.AnchorPercent, 0, 100);
            widget.Position.OffsetPx = Math.Clamp(widget.Position.OffsetPx, -4000, 4000);
            if (!WidgetCatalog.IsKnown(widget.Id))
            {
                widget.Enabled = false;
            }
        }
        Rotation.WidgetIds = Rotation.WidgetIds
            .Where(WidgetCatalog.IsKnown)
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    public void Save(string path) => AtomicJson.Write(path, this, JsonOptions());

    internal static WidgetConfiguration FromLegacy(JsonElement legacy)
    {
        var result = new WidgetConfiguration();
        var active = ReadString(legacy, "activeDesign") ?? "codex-status";
        if (!WidgetCatalog.IsKnown(active))
        {
            active = "codex-status";
        }

        var items = new List<WidgetInstanceConfiguration>();
        if (legacy.TryGetProperty("widgets", out var widgets) && widgets.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in widgets.EnumerateArray())
            {
                var id = ReadString(item, "design") ?? ReadString(item, "id");
                if (string.Equals(id, "btc-fees", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                if (string.IsNullOrWhiteSpace(id))
                {
                    continue;
                }

                items.Add(new WidgetInstanceConfiguration
                {
                    Id = id,
                    Enabled = WidgetCatalog.IsKnown(id) && (ReadBool(item, "enabled") ?? false),
                    Order = ReadInt(item, "order") ?? items.Count,
                    Position = new WidgetPosition
                    {
                        AnchorPercent = ReadInt(item, "positionPct") ?? 100,
                        OffsetPx = ReadInt(item, "moveX") ?? 0
                    }
                });
            }
        }

        if (items.Count == 0)
        {
            items = DefaultWidgets();
            items.First(item => item.Id == active).Enabled = ReadBool(legacy, "enabled") ?? true;
        }

        foreach (var knownId in WidgetCatalog.KnownIds)
        {
            if (items.All(item => !string.Equals(item.Id, knownId, StringComparison.OrdinalIgnoreCase)))
            {
                items.Add(new WidgetInstanceConfiguration { Id = knownId, Order = items.Count });
            }
        }

        result.Widgets = items.OrderBy(item => item.Order).ToList();
        result.Layout.Mode = ReadBool(legacy, "rotationEnabled") == true ? "rotation" : "row";
        result.Rotation.IntervalSeconds = Math.Clamp(ReadInt(legacy, "rotationIntervalSecs") ?? 30, 5, 3600);
        if (legacy.TryGetProperty("rotationDesigns", out var rotation) && rotation.ValueKind == JsonValueKind.Array)
        {
            result.Rotation.WidgetIds = rotation.EnumerateArray()
                .Where(item => item.ValueKind == JsonValueKind.String)
                .Select(item => item.GetString()!)
                .Where(WidgetCatalog.IsKnown)
                .Distinct(StringComparer.OrdinalIgnoreCase)
                .ToList();
        }

        result.Rotation.WidgetIds = result.Rotation.WidgetIds.Count == 0
            ? result.Widgets.Where(item => item.Enabled && WidgetCatalog.IsKnown(item.Id)).Select(item => item.Id).ToList()
            : result.Rotation.WidgetIds;
        return result;
    }

    private static List<WidgetInstanceConfiguration> DefaultWidgets() =>
        WidgetCatalog.KnownIds.Select((id, index) => new WidgetInstanceConfiguration
        {
            Id = id,
            Enabled = id == "codex-status",
            Order = index
        }).ToList();

    private static string? ReadString(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    private static bool? ReadBool(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.ValueKind is JsonValueKind.True or JsonValueKind.False ? value.GetBoolean() : null;

    private static int? ReadInt(JsonElement root, string name) =>
        root.TryGetProperty(name, out var value) && value.TryGetInt32(out var number) ? number : null;

    internal static JsonSerializerOptions JsonOptions() => new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };
}

internal sealed class LayoutConfiguration
{
    public string Mode { get; set; } = "row";
}

internal sealed class WidgetInstanceConfiguration
{
    public string Id { get; set; } = "";
    public bool Enabled { get; set; }
    public int Order { get; set; }
    public WidgetPosition Position { get; set; } = new();
    public JsonObject Settings { get; set; } = new();
}

internal sealed class WidgetPosition
{
    public int AnchorPercent { get; set; } = 100;
    public int OffsetPx { get; set; }
}

internal sealed class RotationConfiguration
{
    public int IntervalSeconds { get; set; } = 30;
    public List<string> WidgetIds { get; set; } = WidgetCatalog.KnownIds.ToList();
}
