using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class DiscordVoiceWorker
{
    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string SettingsPath = Path.Combine(AppDirectory, "config.json");
    private static readonly string AvatarDirectory = Path.Combine(AppDirectory, "DiscordAvatars");
    private static readonly WidgetStateStore StateStore = new();

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(LogsDirectory);
        Directory.CreateDirectory(AvatarDirectory);

        string? lastPayload = null;
        string? lastError = null;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var options = ReadOptions();
                DiscordRealtimeVoiceClient.Configure(
                    options.Enabled && options.RealtimeVoiceEnabled,
                    cancellationToken);
                if (!options.Enabled)
                {
                    var disabled = DiscordStatus.Disabled();
                    lastPayload = WriteStatusIfChanged(disabled, lastPayload);
                    await Task.Delay(TimeSpan.FromSeconds(2), cancellationToken);
                    continue;
                }

                DiscordStatus status;
                try
                {
                    var snapshot = DiscordLocalVoiceProbe.Capture(AvatarDirectory);
                    status = new DiscordStatus
                    {
                        Loaded = true,
                        Connected = snapshot.Connected,
                        Status = snapshot.Connected ? "Voice" : snapshot.Status,
                        ChannelName = snapshot.ChannelName,
                        UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                        Users = snapshot.Users.Take(5).Select(user => new DiscordUserStatus
                        {
                            Id = user.Id,
                            DisplayName = user.DisplayName,
                            AvatarPath = user.AvatarPath,
                            AnimatedAvatarPath = "",
                            Speaking = user.Speaking,
                            Muted = user.Muted,
                            Deafened = user.Deafened,
                            Streaming = user.Streaming
                        }).ToList()
                    };
                    lastError = null;
                }
                catch (Exception ex)
                {
                    status = DiscordStatus.Error(ex.Message);
                    if (!string.Equals(lastError, ex.Message, StringComparison.Ordinal))
                    {
                        Log($"Discord local voice detection failed: {ex.Message}");
                        lastError = ex.Message;
                    }
                }

                lastPayload = WriteStatusIfChanged(status, lastPayload);
                await Task.Delay(
                    status.Connected ? TimeSpan.FromMilliseconds(150) : TimeSpan.FromSeconds(1),
                    cancellationToken);
            }
        }
        finally
        {
            DiscordRealtimeVoiceClient.Configure(false, cancellationToken);
        }
    }

    private static string WriteStatusIfChanged(DiscordStatus status, string? previous)
    {
        var payload = JsonSerializer.Serialize(status, JsonOptions());
        if (!string.Equals(payload, previous, StringComparison.Ordinal))
        {
            StateStore.Write(
                "discord-voice",
                status,
                string.IsNullOrWhiteSpace(status.ErrorMessage) ? "ok" : "error",
                status.ErrorMessage);
        }
        return payload;
    }

    private static DiscordOptions ReadOptions()
    {
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new DiscordOptions(false, false);
            }

            using var document = JsonDocument.Parse(File.ReadAllText(SettingsPath, Encoding.UTF8));
            if (!document.RootElement.TryGetProperty("widgets", out var widgets) ||
                widgets.ValueKind != JsonValueKind.Array)
            {
                return new DiscordOptions(false, false);
            }

            foreach (var widget in widgets.EnumerateArray())
            {
                if (widget.TryGetProperty("id", out var id) &&
                    id.GetString() == "discord-voice")
                {
                    var enabledValue = widget.TryGetProperty("enabled", out var enabled) &&
                                       enabled.ValueKind == JsonValueKind.True;
                    var realtime = widget.TryGetProperty("settings", out var settings) &&
                                   settings.ValueKind == JsonValueKind.Object &&
                                   settings.TryGetProperty("realTimeVoiceEnabled", out var value) &&
                                   value.ValueKind == JsonValueKind.True;
                    return new DiscordOptions(enabledValue, realtime);
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Discord settings read failed: {ex.Message}");
        }

        return new DiscordOptions(false, false);
    }

    private static JsonSerializerOptions JsonOptions() => new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(LogsDirectory);
            File.AppendAllText(
                LogPath,
                $"{DateTimeOffset.Now:O} [loader] {message}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never break the loader.
        }
    }

    private sealed class DiscordStatus
    {
        public bool Loaded { get; set; }
        public bool Connected { get; set; }
        public string Status { get; set; } = "";
        public string? ErrorMessage { get; set; }
        public string ChannelName { get; set; } = "";
        public long UpdatedAtUnix { get; set; }
        public List<DiscordUserStatus> Users { get; set; } = [];

        public static DiscordStatus Disabled() => new()
        {
            Loaded = true,
            Connected = false,
            Status = "Disabled",
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
        };

        public static DiscordStatus Error(string message) => new()
        {
            Loaded = true,
            Connected = false,
            Status = message.Length > 48 ? message[..48] : message,
            ErrorMessage = message,
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
        };
    }

    private sealed record DiscordOptions(bool Enabled, bool RealtimeVoiceEnabled);

    private sealed class DiscordUserStatus
    {
        public string Id { get; set; } = "";
        public string DisplayName { get; set; } = "";
        public string AvatarPath { get; set; } = "";
        public string AnimatedAvatarPath { get; set; } = "";
        public bool Speaking { get; set; }
        public bool Muted { get; set; }
        public bool Deafened { get; set; }
        public bool Streaming { get; set; }
    }
}
