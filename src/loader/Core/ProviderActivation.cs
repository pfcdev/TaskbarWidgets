using System.Text.Json;

namespace TaskbarWidgets.Loader.Core;

internal static class ProviderActivation
{
    private static readonly object SyncRoot = new();
    private static readonly Timer ReloadTimer =
        new(_ => Reload(), null, Timeout.Infinite, Timeout.Infinite);
    private static Dictionary<string, bool> _enabled =
        new(StringComparer.OrdinalIgnoreCase);
    private static FileSystemWatcher? _watcher;
    private static bool _initialized;

    public static bool IsEnabled(string widgetId)
    {
        lock (SyncRoot)
        {
            EnsureInitialized();
            return _enabled.GetValueOrDefault(widgetId);
        }
    }

    private static void EnsureInitialized()
    {
        if (_initialized) return;
        _initialized = true;
        Directory.CreateDirectory(AppPaths.DataDirectory);
        _enabled = Read(Path.Combine(AppPaths.DataDirectory, "config.json"));
        _watcher = new FileSystemWatcher(AppPaths.DataDirectory, "config.json")
        {
            NotifyFilter = NotifyFilters.FileName |
                           NotifyFilters.LastWrite |
                           NotifyFilters.Size
        };
        _watcher.Changed += ScheduleReload;
        _watcher.Created += ScheduleReload;
        _watcher.Renamed += ScheduleReload;
        _watcher.EnableRaisingEvents = true;
    }

    private static void ScheduleReload(object sender, FileSystemEventArgs args) =>
        ReloadTimer.Change(TimeSpan.FromMilliseconds(100), Timeout.InfiniteTimeSpan);

    private static void Reload()
    {
        var enabled = Read(Path.Combine(AppPaths.DataDirectory, "config.json"));
        lock (SyncRoot)
        {
            _enabled = enabled;
        }
    }

    private static Dictionary<string, bool> Read(string path)
    {
        var result = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (!document.RootElement.TryGetProperty("widgets", out var widgets) ||
                widgets.ValueKind != JsonValueKind.Array)
            {
                return result;
            }
            foreach (var widget in widgets.EnumerateArray())
            {
                var id = widget.TryGetProperty("id", out var idValue)
                    ? idValue.GetString()
                    : null;
                if (string.IsNullOrWhiteSpace(id)) continue;
                result[id] =
                    widget.TryGetProperty("enabled", out var enabled) &&
                    enabled.ValueKind == JsonValueKind.True;
            }
        }
        catch
        {
            // A partial atomic replacement is retried after the next write.
        }
        return result;
    }
}
