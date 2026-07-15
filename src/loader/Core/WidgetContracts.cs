using System.Text.Json;
using System.Text.Json.Serialization;

namespace TaskbarWidgets.Loader.Core;

public interface IWidgetProvider
{
    string Id { get; }
    Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken);
}

public sealed record WidgetProviderContext(string DataDirectory, WidgetStateStore StateStore);

public sealed record WidgetSnapshot<T>(
    int SchemaVersion,
    string WidgetId,
    long Sequence,
    long UpdatedAtUnix,
    string Status,
    T? Data,
    string? Error);

public sealed record WidgetCommand(
    int SchemaVersion,
    string CommandId,
    string? WidgetId,
    string Action,
    JsonElement? Arguments,
    long CreatedAtUnix);

public static class WidgetCommandValidator
{
    private static readonly HashSet<string> AllowedActions = new(StringComparer.OrdinalIgnoreCase)
    {
        "addAccount", "loginActiveAccount", "deleteActiveAccount", "deleteAccount",
        "switchAccount", "restartIde", "openSettings", "mediaToggle", "quit"
    };

    public static bool IsValid(WidgetCommand command, long nowUnix)
    {
        return command.SchemaVersion == 1 &&
               !string.IsNullOrWhiteSpace(command.CommandId) &&
               AllowedActions.Contains(command.Action) &&
               command.CreatedAtUnix > 0 &&
               Math.Abs(nowUnix - command.CreatedAtUnix) <= 300;
    }
}

public sealed class WidgetStateStore
{
    private static long _sequence;
    private readonly JsonSerializerOptions _jsonOptions = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
    };

    public void Write<T>(string widgetId, T data, string status = "ok", string? error = null)
    {
        Directory.CreateDirectory(AppPaths.StateDirectory);
        var snapshot = new WidgetSnapshot<T>(
            1,
            widgetId,
            Interlocked.Increment(ref _sequence),
            DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            status,
            data,
            error);
        AtomicJson.Write(PathFor(widgetId), snapshot, _jsonOptions);
    }

    public T? ReadData<T>(string widgetId)
    {
        var path = PathFor(widgetId);
        if (!File.Exists(path))
        {
            return default;
        }

        using var document = JsonDocument.Parse(File.ReadAllText(path));
        if (!document.RootElement.TryGetProperty("data", out var data))
        {
            return JsonSerializer.Deserialize<T>(document.RootElement.GetRawText(), _jsonOptions);
        }

        return JsonSerializer.Deserialize<T>(data.GetRawText(), _jsonOptions);
    }

    public static string PathFor(string widgetId) =>
        IsSafeWidgetId(widgetId)
            ? Path.Combine(AppPaths.StateDirectory, $"{widgetId}.json")
            : throw new ArgumentException("Invalid widget id.", nameof(widgetId));

    private static bool IsSafeWidgetId(string id) =>
        id.Length is >= 3 and <= 64 &&
        id[0] is >= 'a' and <= 'z' &&
        id.All(character => character is >= 'a' and <= 'z' or >= '0' and <= '9' or '-');
}

internal static class AtomicJson
{
    public static void Write<T>(string path, T value, JsonSerializerOptions options)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        var temporaryPath = $"{path}.{Environment.ProcessId}.{Guid.NewGuid():N}.tmp";
        try
        {
            File.WriteAllText(temporaryPath, JsonSerializer.Serialize(value, options));
            File.Move(temporaryPath, path, overwrite: true);
        }
        finally
        {
            File.Delete(temporaryPath);
        }
    }
}
