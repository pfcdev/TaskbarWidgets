using System.Buffers.Binary;
using System.IO.Pipes;
using System.Net.Http.Json;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class DiscordVoiceWorker
{
    private const int OpHandshake = 0;
    private const int OpFrame = 1;
    private const int OpClose = 2;
    private const int OpPing = 3;
    private const int OpPong = 4;

    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string SettingsPath = Path.Combine(AppDirectory, "config.json");
    private static readonly string AvatarDirectory = Path.Combine(AppDirectory, "DiscordAvatars");
    private static readonly WidgetStateStore StateStore = new();
    private static readonly HttpClient Http = new() { Timeout = TimeSpan.FromSeconds(15) };

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(LogsDirectory);
        Directory.CreateDirectory(AvatarDirectory);

        while (!cancellationToken.IsCancellationRequested)
        {
            var settings = ReadSettings();
            if (!settings.Enabled)
            {
                WriteStatus(DiscordStatus.Disabled());
                await Task.Delay(TimeSpan.FromSeconds(5), cancellationToken);
                continue;
            }

            try
            {
                await RunSessionAsync(settings, cancellationToken);
            }
            catch (OperationCanceledException)
            {
                throw;
            }
            catch (Exception ex)
            {
                Log($"Discord voice update failed: {ex.Message}");
                WriteStatus(DiscordStatus.Error(ex.Message));
                await Task.Delay(TimeSpan.FromSeconds(8), cancellationToken);
            }
        }
    }

    private static async Task RunSessionAsync(
        DiscordSettings settings,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(settings.ClientId))
        {
            WriteStatus(DiscordStatus.Error("Discord client id missing"));
            await Task.Delay(TimeSpan.FromSeconds(10), cancellationToken);
            return;
        }

        await using var ipc = await DiscordIpc.ConnectAsync(cancellationToken);
        await ipc.SendAsync(OpHandshake, new
        {
            v = 1,
            client_id = settings.ClientId
        }, cancellationToken);

        var ready = await ipc.ReceiveJsonAsync(cancellationToken);
        if (ready.Opcode != OpFrame ||
            !string.Equals(ready.Root.TryGetProperty("evt", out var evt) ? evt.GetString() : null, "READY", StringComparison.Ordinal))
        {
            throw new InvalidOperationException("Discord READY frame was not received");
        }

        if (string.IsNullOrWhiteSpace(settings.ClientSecret) ||
            string.IsNullOrWhiteSpace(settings.RedirectUri))
        {
            WriteStatus(DiscordStatus.Error("Discord authorization settings missing"));
            await Task.Delay(TimeSpan.FromSeconds(20), cancellationToken);
            return;
        }

        var token = await AuthorizeAndExchangeAsync(ipc, settings, cancellationToken);
        await AuthenticateAsync(ipc, token, cancellationToken);

        var monitor = new VoiceMonitor(ipc);
        await monitor.SubscribeGlobalAsync(cancellationToken);
        await monitor.RequestSelectedChannelAsync(cancellationToken);
        monitor.WriteStatus();

        while (!cancellationToken.IsCancellationRequested)
        {
            var frame = await ipc.ReceiveJsonAsync(cancellationToken);
            if (frame.Opcode == OpClose)
            {
                throw new InvalidOperationException("Discord IPC closed");
            }

            if (frame.Opcode != OpFrame)
            {
                continue;
            }

            await monitor.HandleAsync(frame.Root, cancellationToken);
        }
    }

    private static async Task<string> AuthorizeAndExchangeAsync(
        DiscordIpc ipc,
        DiscordSettings settings,
        CancellationToken cancellationToken)
    {
        var nonce = await ipc.CommandAsync("AUTHORIZE", new
        {
            client_id = settings.ClientId,
            scopes = new[] { "rpc", "identify", "rpc.voice.read" }
        }, cancellationToken: cancellationToken);
        using var authorize = await ipc.WaitForNonceAsync(nonce, cancellationToken);
        ThrowIfRpcError(authorize.RootElement, "AUTHORIZE");

        var code = authorize.RootElement.GetProperty("data").GetProperty("code").GetString();
        if (string.IsNullOrWhiteSpace(code))
        {
            throw new InvalidOperationException("Discord authorize response did not include a code");
        }

        using var response = await Http.PostAsync(
            "https://discord.com/api/v10/oauth2/token",
            new FormUrlEncodedContent(new Dictionary<string, string>
            {
                ["client_id"] = settings.ClientId,
                ["client_secret"] = settings.ClientSecret,
                ["grant_type"] = "authorization_code",
                ["code"] = code,
                ["redirect_uri"] = settings.RedirectUri
            }),
            cancellationToken);

        using var tokenJson = await JsonDocument.ParseAsync(
            await response.Content.ReadAsStreamAsync(cancellationToken),
            cancellationToken: cancellationToken);
        if (!response.IsSuccessStatusCode ||
            !tokenJson.RootElement.TryGetProperty("access_token", out var accessToken))
        {
            throw new InvalidOperationException($"Discord token exchange failed: HTTP {(int)response.StatusCode}");
        }

        return accessToken.GetString() ??
               throw new InvalidOperationException("Discord token response was empty");
    }

    private static async Task AuthenticateAsync(
        DiscordIpc ipc,
        string accessToken,
        CancellationToken cancellationToken)
    {
        var nonce = await ipc.CommandAsync("AUTHENTICATE", new
        {
            access_token = accessToken
        }, cancellationToken: cancellationToken);
        using var response = await ipc.WaitForNonceAsync(nonce, cancellationToken);
        ThrowIfRpcError(response.RootElement, "AUTHENTICATE");
    }

    private static void ThrowIfRpcError(JsonElement payload, string operation)
    {
        if (payload.TryGetProperty("evt", out var evt) &&
            evt.GetString() == "ERROR" &&
            payload.TryGetProperty("data", out var data))
        {
            var code = data.TryGetProperty("code", out var codeElement)
                ? codeElement.ToString()
                : "-";
            var message = data.TryGetProperty("message", out var messageElement)
                ? messageElement.GetString()
                : "unknown";
            throw new InvalidOperationException($"{operation} failed: {code}: {message}");
        }
    }

    private static DiscordSettings ReadSettings()
    {
        try
        {
            if (!File.Exists(SettingsPath))
            {
                return new DiscordSettings();
            }

            using var document = JsonDocument.Parse(File.ReadAllText(SettingsPath, Encoding.UTF8));
            var root = FindWidget(document.RootElement, "discord-voice");
            if (root.ValueKind == JsonValueKind.Undefined)
            {
                return new DiscordSettings();
            }
            var isEnabled = root.TryGetProperty("enabled", out var enabled) && enabled.GetBoolean();
            var values = root.TryGetProperty("settings", out var widgetSettings) ? widgetSettings : default;
            return new DiscordSettings
            {
                Enabled = isEnabled,
                ClientId = GetString(values, "clientId"),
                ClientSecret = GetString(values, "clientSecret"),
                RedirectUri = GetString(values, "redirectUri")
            };
        }
        catch (Exception ex)
        {
            Log($"Discord settings read failed: {ex.Message}");
            return new DiscordSettings();
        }
    }

    private static JsonElement FindWidget(JsonElement root, string id)
    {
        if (root.TryGetProperty("widgets", out var widgets) && widgets.ValueKind == JsonValueKind.Array)
        {
            foreach (var widget in widgets.EnumerateArray())
            {
                if (widget.TryGetProperty("id", out var widgetId) && widgetId.GetString() == id)
                {
                    return widget;
                }
            }
        }

        return default;
    }

    private static string GetString(JsonElement root, string property) =>
        root.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? ""
            : "";

    private static void WriteStatus(DiscordStatus status)
    {
        StateStore.Write("discord-voice", status,
            string.IsNullOrWhiteSpace(status.ErrorMessage) ? "ok" : "error",
            status.ErrorMessage);
    }

    private static JsonSerializerOptions JsonOptions() => new()
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    private sealed record AvatarUrls(string? StaticUrl, string? AnimatedUrl);

    private static AvatarUrls AvatarUrlsFor(JsonElement user)
    {
        if (!user.TryGetProperty("id", out var idElement))
        {
            return new AvatarUrls(null, null);
        }

        var userId = idElement.GetString();
        if (string.IsNullOrWhiteSpace(userId))
        {
            return new AvatarUrls(null, null);
        }

        if (user.TryGetProperty("avatar", out var avatarElement) &&
            avatarElement.ValueKind == JsonValueKind.String &&
            !string.IsNullOrWhiteSpace(avatarElement.GetString()))
        {
            var avatar = avatarElement.GetString()!;
            var baseUrl = $"https://cdn.discordapp.com/avatars/{userId}/{avatar}";
            return new AvatarUrls(
                $"{baseUrl}.png?size=128",
                avatar.StartsWith("a_", StringComparison.Ordinal) ? $"{baseUrl}.gif?size=128" : null);
        }

        var defaultIndex = (ulong.Parse(userId) >> 22) % 6;
        return new AvatarUrls($"https://cdn.discordapp.com/embed/avatars/{defaultIndex}.png", null);
    }

    private static async Task<string> DownloadAvatarAsync(
        string userId,
        string suffix,
        string? url,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(url))
        {
            return "";
        }

        var path = Path.Combine(AvatarDirectory, $"{userId}{suffix}");
        if (File.Exists(path))
        {
            return path;
        }

        try
        {
            var bytes = await Http.GetByteArrayAsync(url, cancellationToken);
            await File.WriteAllBytesAsync(path, bytes, cancellationToken);
        }
        catch (Exception ex)
        {
            Log($"Discord avatar download failed for {userId}: {ex.Message}");
        }

        return File.Exists(path) ? path : "";
    }

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

    private sealed class VoiceMonitor(DiscordIpc ipc)
    {
        private readonly Dictionary<string, VoiceUser> _users = new();
        private readonly HashSet<string> _speaking = new(StringComparer.Ordinal);
        private readonly Dictionary<string, string> _pending = new();
        private string? _channelId;
        private string _channelName = "";

        public async Task SubscribeGlobalAsync(CancellationToken cancellationToken)
        {
            await SendTrackedAsync("SUBSCRIBE", new { }, "VOICE_CHANNEL_SELECT", "subscribe_global", cancellationToken);
            await SendTrackedAsync("SUBSCRIBE", new { }, "VOICE_CONNECTION_STATUS", "subscribe_connection", cancellationToken);
        }

        public Task RequestSelectedChannelAsync(CancellationToken cancellationToken) =>
            SendTrackedAsync("GET_SELECTED_VOICE_CHANNEL", new { }, null, "get_selected_channel", cancellationToken);

        public async Task HandleAsync(JsonElement payload, CancellationToken cancellationToken)
        {
            if (payload.TryGetProperty("cmd", out var cmd) &&
                cmd.GetString() == "DISPATCH" &&
                payload.TryGetProperty("evt", out var evtElement))
            {
                await HandleDispatchAsync(evtElement.GetString() ?? "", payload.GetProperty("data"), cancellationToken);
                return;
            }

            var nonce = GetString(payload, "nonce");
            if (!string.IsNullOrWhiteSpace(nonce) &&
                _pending.Remove(nonce, out var purpose) &&
                purpose == "get_selected_channel" &&
                payload.TryGetProperty("data", out var data))
            {
                await SetChannelAsync(data, cancellationToken);
            }
        }

        public void WriteStatus()
        {
            var users = _users.Values
                .Take(5)
                .Select(user => new DiscordUserStatus
                {
            Id = user.Id,
            DisplayName = user.DisplayName,
            AvatarPath = user.AvatarPath,
            AnimatedAvatarPath = user.AnimatedAvatarPath,
            Speaking = _speaking.Contains(user.Id)
        })
                .ToList();

            DiscordVoiceWorker.WriteStatus(new DiscordStatus
            {
                Loaded = true,
                Connected = !string.IsNullOrWhiteSpace(_channelId),
                Status = string.IsNullOrWhiteSpace(_channelId) ? "No voice" : "Voice",
                ChannelName = _channelName,
                UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                Users = users
            });
        }

        private async Task HandleDispatchAsync(
            string evt,
            JsonElement data,
            CancellationToken cancellationToken)
        {
            switch (evt)
            {
                case "VOICE_CHANNEL_SELECT":
                    await RequestSelectedChannelAsync(cancellationToken);
                    break;
                case "VOICE_STATE_CREATE":
                case "VOICE_STATE_UPDATE":
                    await UpsertUserAsync(data, cancellationToken);
                    WriteStatus();
                    break;
                case "VOICE_STATE_DELETE":
                    RemoveUser(data);
                    WriteStatus();
                    break;
                case "SPEAKING_START":
                case "SPEAKING_STOP":
                    var userId = GetString(data, "user_id");
                    if (!string.IsNullOrWhiteSpace(userId))
                    {
                        if (evt == "SPEAKING_START")
                        {
                            _speaking.Add(userId);
                        }
                        else
                        {
                            _speaking.Remove(userId);
                        }
                        WriteStatus();
                    }
                    break;
            }
        }

        private async Task SetChannelAsync(JsonElement channel, CancellationToken cancellationToken)
        {
            _users.Clear();
            _speaking.Clear();

            if (channel.ValueKind != JsonValueKind.Object ||
                !channel.TryGetProperty("id", out var channelIdElement))
            {
                _channelId = null;
                _channelName = "";
                WriteStatus();
                return;
            }

            _channelId = channelIdElement.GetString();
            _channelName = GetString(channel, "name");

            if (channel.TryGetProperty("voice_states", out var voiceStates) &&
                voiceStates.ValueKind == JsonValueKind.Array)
            {
                foreach (var voiceState in voiceStates.EnumerateArray())
                {
                    await UpsertUserAsync(voiceState, cancellationToken);
                }
            }

            if (!string.IsNullOrWhiteSpace(_channelId))
            {
                foreach (var evt in new[]
                         {
                             "VOICE_STATE_CREATE",
                             "VOICE_STATE_UPDATE",
                             "VOICE_STATE_DELETE",
                             "SPEAKING_START",
                             "SPEAKING_STOP"
                         })
                {
                    await SendTrackedAsync(
                        "SUBSCRIBE",
                        new { channel_id = _channelId },
                        evt,
                        $"subscribe:{evt}",
                        cancellationToken);
                }
            }

            WriteStatus();
        }

        private async Task UpsertUserAsync(JsonElement data, CancellationToken cancellationToken)
        {
            if (!data.TryGetProperty("user", out var user) ||
                user.ValueKind != JsonValueKind.Object)
            {
                return;
            }

            var id = GetString(user, "id");
            if (string.IsNullOrWhiteSpace(id))
            {
                return;
            }

            var displayName = GetString(data, "nick");
            if (string.IsNullOrWhiteSpace(displayName))
            {
                displayName = GetString(user, "global_name");
            }
            if (string.IsNullOrWhiteSpace(displayName))
            {
                displayName = GetString(user, "username");
            }
            if (string.IsNullOrWhiteSpace(displayName))
            {
                displayName = id;
            }

            var avatarUrls = AvatarUrlsFor(user);
            _users[id] = new VoiceUser
            {
                Id = id,
                DisplayName = displayName,
                AvatarPath = await DownloadAvatarAsync(id, ".png", avatarUrls.StaticUrl, cancellationToken),
                AnimatedAvatarPath = await DownloadAvatarAsync(id, ".gif", avatarUrls.AnimatedUrl, cancellationToken)
            };
        }

        private void RemoveUser(JsonElement data)
        {
            var userId = data.TryGetProperty("user", out var user)
                ? GetString(user, "id")
                : GetString(data, "user_id");
            if (!string.IsNullOrWhiteSpace(userId))
            {
                _users.Remove(userId);
                _speaking.Remove(userId);
            }
        }

        private async Task SendTrackedAsync(
            string command,
            object args,
            string? evt,
            string purpose,
            CancellationToken cancellationToken)
        {
            var nonce = await ipc.CommandAsync(command, args, evt, cancellationToken);
            _pending[nonce] = purpose;
        }
    }

    private sealed class DiscordIpc : IAsyncDisposable
    {
        private readonly NamedPipeClientStream _pipe;

        private DiscordIpc(NamedPipeClientStream pipe) => _pipe = pipe;

        public static async Task<DiscordIpc> ConnectAsync(CancellationToken cancellationToken)
        {
            Exception? last = null;
            for (var i = 0; i < 10; i++)
            {
                var pipe = new NamedPipeClientStream(
                    ".",
                    $"discord-ipc-{i}",
                    PipeDirection.InOut,
                    PipeOptions.Asynchronous);
                try
                {
                    await pipe.ConnectAsync(250, cancellationToken);
                    return new DiscordIpc(pipe);
                }
                catch (Exception ex)
                {
                    last = ex;
                    await pipe.DisposeAsync();
                }
            }

            throw new InvalidOperationException("Discord IPC pipe not found", last);
        }

        public async Task<string> CommandAsync(
            string command,
            object args,
            string? evt = null,
            CancellationToken cancellationToken = default)
        {
            var nonce = Guid.NewGuid().ToString();
            var payload = evt is null
                ? new Dictionary<string, object?>
                {
                    ["cmd"] = command,
                    ["args"] = args,
                    ["nonce"] = nonce
                }
                : new Dictionary<string, object?>
                {
                    ["cmd"] = command,
                    ["args"] = args,
                    ["evt"] = evt,
                    ["nonce"] = nonce
                };
            await SendAsync(OpFrame, payload, cancellationToken);
            return nonce;
        }

        public async Task<JsonDocument> WaitForNonceAsync(
            string nonce,
            CancellationToken cancellationToken)
        {
            while (true)
            {
                var frame = await ReceiveJsonAsync(cancellationToken);
                if (frame.Opcode == OpClose)
                {
                    throw new InvalidOperationException("Discord IPC closed");
                }

                if (frame.Root.TryGetProperty("nonce", out var nonceElement) &&
                    nonceElement.GetString() == nonce)
                {
                    return frame.Document;
                }

                frame.Document.Dispose();
            }
        }

        public async Task SendAsync(
            int opcode,
            object payload,
            CancellationToken cancellationToken)
        {
            var body = JsonSerializer.SerializeToUtf8Bytes(payload, JsonOptions());
            var header = new byte[8];
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(0, 4), opcode);
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(4, 4), body.Length);
            await _pipe.WriteAsync(header, cancellationToken);
            await _pipe.WriteAsync(body, cancellationToken);
            await _pipe.FlushAsync(cancellationToken);
        }

        public async Task<IpcFrame> ReceiveJsonAsync(CancellationToken cancellationToken)
        {
            while (true)
            {
                var header = await ReadExactlyAsync(8, cancellationToken);
                var opcode = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(0, 4));
                var length = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4, 4));
                var body = length > 0
                    ? await ReadExactlyAsync(length, cancellationToken)
                    : Array.Empty<byte>();

                if (opcode == OpPing)
                {
                    await SendRawAsync(OpPong, body, cancellationToken);
                    continue;
                }

                var document = body.Length > 0
                    ? JsonDocument.Parse(body)
                    : JsonDocument.Parse("{}");
                return new IpcFrame(opcode, document);
            }
        }

        private async Task SendRawAsync(
            int opcode,
            byte[] body,
            CancellationToken cancellationToken)
        {
            var header = new byte[8];
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(0, 4), opcode);
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(4, 4), body.Length);
            await _pipe.WriteAsync(header, cancellationToken);
            await _pipe.WriteAsync(body, cancellationToken);
            await _pipe.FlushAsync(cancellationToken);
        }

        private async Task<byte[]> ReadExactlyAsync(int count, CancellationToken cancellationToken)
        {
            var buffer = new byte[count];
            var offset = 0;
            while (offset < count)
            {
                var read = await _pipe.ReadAsync(
                    buffer.AsMemory(offset, count - offset),
                    cancellationToken);
                if (read <= 0)
                {
                    throw new EndOfStreamException("Discord IPC stream closed");
                }
                offset += read;
            }
            return buffer;
        }

        public ValueTask DisposeAsync() => _pipe.DisposeAsync();
    }

    private sealed class DiscordSettings
    {
        public bool Enabled { get; set; }
        public string ClientId { get; set; } = "";
        public string ClientSecret { get; set; } = "";
        public string RedirectUri { get; set; } = "";
    }

    private sealed class VoiceUser
    {
        public string Id { get; set; } = "";
        public string DisplayName { get; set; } = "";
        public string AvatarPath { get; set; } = "";
        public string AnimatedAvatarPath { get; set; } = "";
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

    private sealed class DiscordUserStatus
    {
        public string Id { get; set; } = "";
        public string DisplayName { get; set; } = "";
        public string AvatarPath { get; set; } = "";
        public string AnimatedAvatarPath { get; set; } = "";
        public bool Speaking { get; set; }
    }

    private sealed class IpcFrame(int opcode, JsonDocument document) : IDisposable
    {
        public int Opcode { get; } = opcode;
        public JsonDocument Document { get; } = document;
        public JsonElement Root => Document.RootElement;
        public void Dispose() => Document.Dispose();
    }
}
