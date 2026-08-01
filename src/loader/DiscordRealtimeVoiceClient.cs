using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Pipes;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace TaskbarWidgets.Loader;

internal static class DiscordRealtimeVoiceClient
{
    private const string ServiceName = "TaskbarWidgetsVoiceCapture";
    private const string PipeName = "TaskbarWidgets.VoiceCapture.v1";
    private static readonly object Gate = new();
    private static readonly ConcurrentDictionary<uint, SpeakingState> States = new();
    private static CancellationTokenSource? _lifetime;
    private static Task? _worker;
    private static volatile bool _connected;

    public static void Configure(bool enabled, CancellationToken applicationToken)
    {
        lock (Gate)
        {
            if (enabled)
            {
                if (_worker is { IsCompleted: false })
                {
                    return;
                }

                _lifetime?.Dispose();
                _lifetime = CancellationTokenSource.CreateLinkedTokenSource(applicationToken);
                var token = _lifetime.Token;
                _worker = Task.Run(() => RunAsync(token), token);
                return;
            }

            _lifetime?.Cancel();
            _lifetime?.Dispose();
            _lifetime = null;
            _worker = null;
            _connected = false;
            States.Clear();
        }
    }

    public static bool TryGetSpeaking(uint ssrc, out bool speaking)
    {
        speaking = false;
        if (!_connected || !States.TryGetValue(ssrc, out var state))
        {
            return false;
        }

        if (DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() - state.UpdatedAtUnixMs > 5000)
        {
            States.TryRemove(ssrc, out _);
            return false;
        }

        speaking = state.Active;
        return true;
    }

    private static async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await using var pipe = new NamedPipeServerStream(
                    PipeName,
                    PipeDirection.In,
                    1,
                    PipeTransmissionMode.Byte,
                    PipeOptions.Asynchronous,
                    0,
                    4096);

                TryStartService();
                await pipe.WaitForConnectionAsync(cancellationToken);
                _connected = true;
                using var reader = new StreamReader(pipe);
                while (!cancellationToken.IsCancellationRequested && pipe.IsConnected)
                {
                    var line = await reader.ReadLineAsync(cancellationToken);
                    if (line is null)
                    {
                        break;
                    }
                    Apply(line);
                }
            }
            catch (OperationCanceledException)
            {
                break;
            }
            catch
            {
                // The helper is optional. Existing log-based detection remains active.
            }
            finally
            {
                _connected = false;
                States.Clear();
            }

            try
            {
                await Task.Delay(2000, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                break;
            }
        }
    }

    private static void Apply(string line)
    {
        try
        {
            var message = JsonSerializer.Deserialize<VoiceMessage>(line);
            if (message is null ||
                !string.Equals(message.Type, "speaking", StringComparison.Ordinal) ||
                message.Ssrc == 0)
            {
                return;
            }

            States[message.Ssrc] = new SpeakingState(message.Active, message.UnixMs);
        }
        catch (JsonException)
        {
        }
    }

    private static void TryStartService()
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.SystemDirectory, "sc.exe"),
                Arguments = $"start {ServiceName}",
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            });
            process?.WaitForExit(5000);
        }
        catch
        {
        }
    }

    private sealed class VoiceMessage
    {
        [JsonPropertyName("type")]
        public string Type { get; init; } = "";

        [JsonPropertyName("ssrc")]
        public uint Ssrc { get; init; }

        [JsonPropertyName("active")]
        public bool Active { get; init; }

        [JsonPropertyName("unixMs")]
        public long UnixMs { get; init; }
    }

    private sealed record SpeakingState(bool Active, long UpdatedAtUnixMs);
}
