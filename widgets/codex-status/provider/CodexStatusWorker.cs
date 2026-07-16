using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class CodexStatusWorker
{
    public static async Task RunAsync(string[] args, CancellationToken cancellationToken)
    {
        var once = args.Any(arg => string.Equals(arg, "--once", StringComparison.OrdinalIgnoreCase));
        var interval = GetInterval(args);

        Directory.CreateDirectory(GetStatusDirectory());

        do
        {
            cancellationToken.ThrowIfCancellationRequested();
            var accountChangeVersion = AccountManager.GetChangeVersion();
            var status = await CollectStatusAsync(AccountManager.GetActiveAccount());
            WriteStatus(status);
            await RefreshAccountRateLimitSummariesAsync(status, cancellationToken);

            if (!once)
            {
                await DelayUntilNextIntervalOrAccountChangeAsync(
                    interval,
                    accountChangeVersion,
                    cancellationToken);
            }
        }
        while (!once && !cancellationToken.IsCancellationRequested);
    }

    private static async Task DelayUntilNextIntervalOrAccountChangeAsync(
        TimeSpan interval,
        long observedAccountChangeVersion,
        CancellationToken cancellationToken)
    {
        var deadline = DateTimeOffset.UtcNow.Add(interval);
        while (DateTimeOffset.UtcNow < deadline)
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (AccountManager.GetChangeVersion() != observedAccountChangeVersion)
            {
                return;
            }

            var remaining = deadline - DateTimeOffset.UtcNow;
            var delay = remaining < TimeSpan.FromSeconds(1)
                ? remaining
                : TimeSpan.FromSeconds(1);
            if (delay > TimeSpan.Zero)
            {
                await Task.Delay(delay, cancellationToken);
            }
        }
    }
static TimeSpan GetInterval(string[] args)
{
    for (var i = 0; i < args.Length - 1; i++)
    {
        if (string.Equals(args[i], "--interval", StringComparison.OrdinalIgnoreCase) &&
            int.TryParse(args[i + 1], out var seconds) &&
            seconds >= 5)
        {
            return TimeSpan.FromSeconds(seconds);
        }
    }

    return TimeSpan.FromSeconds(60);
}

static async Task<CodexStatus> CollectStatusAsync(AccountSnapshot activeAccount)
{
    var status = new CodexStatus
    {
        Version = 1,
        UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
        Status = "ok",
        ActiveAccountId = activeAccount.Id,
        ActiveAccountLabel = activeAccount.Label
    };

    try
    {
        var appServer = await ReadRateLimitsFromAppServerAsync(activeAccount.CodexHome);
        status.PlanType = appServer.PlanType;
        status.PrimaryUsedPercent = appServer.PrimaryUsedPercent;
        status.PrimaryWindowMins = appServer.PrimaryWindowMins;
        status.PrimaryResetsAtUnix = appServer.PrimaryResetsAtUnix;
        status.SecondaryUsedPercent = appServer.SecondaryUsedPercent;
        status.SecondaryWindowMins = appServer.SecondaryWindowMins;
        status.SecondaryResetsAtUnix = appServer.SecondaryResetsAtUnix;
        status.CreditsBalance = appServer.CreditsBalance;
        status.CreditsUnlimited = appServer.CreditsUnlimited;
    }
    catch (Exception ex)
    {
        status.Status = "degraded";
        status.Error = $"rate-limits: {ex.Message}";
        PreservePreviousRateLimitFields(status);
    }

    try
    {
        var usage = ReadLocalUsage30d(activeAccount.CodexHome);
        status.Tokens30d = usage.Tokens;
        status.ThreadCount30d = usage.Threads;
    }
    catch (Exception ex)
    {
        status.Status = "degraded";
        status.Error = string.IsNullOrWhiteSpace(status.Error)
            ? $"usage-30d: {ex.Message}"
            : $"{status.Error}; usage-30d: {ex.Message}";
    }

    try
    {
        var task = ReadLatestTaskState(activeAccount.CodexHome);
        status.TaskState = task.State;
        status.TaskLabel = task.Label;
        status.TaskTitle = task.Title;
        status.TaskUpdatedAtUnix = task.UpdatedAtUnix;
    }
    catch (Exception ex)
    {
        status.Status = "degraded";
        status.Error = string.IsNullOrWhiteSpace(status.Error)
            ? $"task-state: {ex.Message}"
            : $"{status.Error}; task-state: {ex.Message}";
    }

    return status;
}

static async Task RefreshAccountRateLimitSummariesAsync(
    CodexStatus activeStatus,
    CancellationToken cancellationToken)
{
    foreach (var account in AccountManager.GetAccounts())
    {
        cancellationToken.ThrowIfCancellationRequested();

        var summary = "-- -- -- --";
        if (string.Equals(account.Id, activeStatus.ActiveAccountId,
                StringComparison.OrdinalIgnoreCase))
        {
            summary = FormatRateLimitSummary(
                activeStatus.PrimaryUsedPercent,
                activeStatus.PrimaryResetsAtUnix,
                activeStatus.SecondaryUsedPercent,
                activeStatus.Tokens30d);
        }
        else
        {
            try
            {
                var appServer = await ReadRateLimitsFromAppServerAsync(account.CodexHome);
                var usage = ReadLocalUsage30d(account.CodexHome);
                summary = FormatRateLimitSummary(
                    appServer.PrimaryUsedPercent,
                    appServer.PrimaryResetsAtUnix,
                    appServer.SecondaryUsedPercent,
                    usage.Tokens);
            }
            catch
            {
                // Keep the menu readable even when a side account is logged out
                // or Codex cannot provide rate limits for that profile.
            }
        }

        AccountManager.SetAccountRateLimitText(account.Id, summary);
    }
}

static string FormatRateLimitSummary(double? primaryUsedPercent,
                                     long? primaryResetsAtUnix,
                                     double? secondaryUsedPercent,
                                     long tokens30d) =>
    $"{FormatRemainingPercent(primaryUsedPercent)} " +
    $"{FormatReset(primaryResetsAtUnix)} " +
    $"{FormatRemainingPercent(secondaryUsedPercent)} " +
    $"{FormatTokenCount(tokens30d)}";

static string FormatRemainingPercent(double? usedPercent)
{
    if (usedPercent is null)
    {
        return "--";
    }

    var remaining = Math.Clamp(100.0 - usedPercent.Value, 0.0, 100.0);
    return $"{remaining:0}%";
}

static string FormatReset(long? unixTime)
{
    if (unixTime is null || unixTime <= 0)
    {
        return "--";
    }

    var seconds = unixTime.Value - DateTimeOffset.UtcNow.ToUnixTimeSeconds();
    if (seconds <= 0)
    {
        return "NOW";
    }

    var minutes = (seconds + 59) / 60;
    if (minutes < 60)
    {
        return $"{minutes}M";
    }

    if (minutes < 60 * 24)
    {
        return $"{(minutes + 59) / 60}H";
    }

    return $"{(minutes + 60 * 24 - 1) / (60 * 24)}D";
}

static string FormatTokenCount(long value)
{
    if (value >= 1_000_000_000)
    {
        return $"{value / 1_000_000_000.0:0.0}B";
    }

    if (value >= 1_000_000)
    {
        return $"{value / 1_000_000.0:0}M";
    }

    if (value >= 1_000)
    {
        return $"{value / 1_000.0:0}K";
    }

    return value.ToString();
}

static void PreservePreviousRateLimitFields(CodexStatus status)
{
    try
    {
        var path = GetStatusPath();
        if (!File.Exists(path))
        {
            return;
        }

        var previous = new WidgetStateStore().ReadData<CodexStatus>("codex-status");
        if (previous is null)
        {
            return;
        }

        status.PlanType ??= previous.PlanType;
        status.PrimaryUsedPercent ??= previous.PrimaryUsedPercent;
        status.PrimaryWindowMins ??= previous.PrimaryWindowMins;
        status.PrimaryResetsAtUnix ??= previous.PrimaryResetsAtUnix;
        status.SecondaryUsedPercent ??= previous.SecondaryUsedPercent;
        status.SecondaryWindowMins ??= previous.SecondaryWindowMins;
        status.SecondaryResetsAtUnix ??= previous.SecondaryResetsAtUnix;
        status.CreditsBalance ??= previous.CreditsBalance;
        status.CreditsUnlimited ??= previous.CreditsUnlimited;
    }
    catch
    {
        // Best effort. A broken previous file must not block fresh status data.
    }
}

static async Task<AppServerStatus> ReadRateLimitsFromAppServerAsync(string codexHome)
{
    var port = GetFreeLoopbackPort();
    using var process = StartCodexWebSocketAppServer(port, codexHome);
    _ = Task.Run(async () =>
    {
        try
        {
            while (await process.StandardOutput.ReadLineAsync() is not null)
            {
                // Drain stdout so app-server cannot block on diagnostics.
            }
        }
        catch
        {
            // Best-effort diagnostics drain.
        }
    });
    _ = Task.Run(async () =>
    {
        try
        {
            while (await process.StandardError.ReadLineAsync() is not null)
            {
                // Drain stderr so app-server cannot block on diagnostics.
            }
        }
        catch
        {
            // Best-effort diagnostics drain.
        }
    });

    using var socket = await ConnectWebSocketAsync(port);

    var nextId = 1;
    await SendJsonWebSocketAsync(socket, new
    {
        id = nextId,
        method = "initialize",
        @params = new
        {
            clientInfo = new
            {
                name = "taskbar-widgets",
                title = "TaskbarWidgets",
                version = "0.3.9"
            },
            capabilities = new
            {
                experimentalApi = true,
                optOutNotificationMethods = Array.Empty<string>()
            }
        }
    });
    await ReadResultWebSocketAsync(socket, nextId++, TimeSpan.FromSeconds(15));

    await SendJsonWebSocketAsync(socket, new { method = "initialized" });
    await Task.Delay(1000);

    var rateLimitId = nextId;
    await SendJsonWebSocketAsync(socket, new
    {
        id = rateLimitId,
        method = "account/rateLimits/read"
    });

    var limits = await ReadResultWebSocketAsync(socket, rateLimitId, TimeSpan.FromSeconds(30));

    TryKill(process);

    var snapshot = limits["rateLimitsByLimitId"]?["codex"] ?? limits["rateLimits"];
    if (snapshot is null)
    {
        throw new InvalidOperationException("Codex rate limit snapshot was empty");
    }

    return new AppServerStatus
    {
        PlanType = snapshot["planType"]?.GetValue<string>(),
        PrimaryUsedPercent = snapshot["primary"]?["usedPercent"]?.GetValue<double>(),
        PrimaryWindowMins = snapshot["primary"]?["windowDurationMins"]?.GetValue<long?>(),
        PrimaryResetsAtUnix = snapshot["primary"]?["resetsAt"]?.GetValue<long?>(),
        SecondaryUsedPercent = snapshot["secondary"]?["usedPercent"]?.GetValue<double>(),
        SecondaryWindowMins = snapshot["secondary"]?["windowDurationMins"]?.GetValue<long?>(),
        SecondaryResetsAtUnix = snapshot["secondary"]?["resetsAt"]?.GetValue<long?>(),
        CreditsBalance = snapshot["credits"]?["balance"]?.GetValue<string>(),
        CreditsUnlimited = snapshot["credits"]?["unlimited"]?.GetValue<bool?>()
    };
}

static int GetFreeLoopbackPort()
{
    var listener = new TcpListener(IPAddress.Loopback, 0);
    listener.Start();
    var port = ((IPEndPoint)listener.LocalEndpoint).Port;
    listener.Stop();
    return port;
}

static Process StartCodexWebSocketAppServer(int port, string codexHome)
{
    var codex = FindCodexExecutable();
    Directory.CreateDirectory(codexHome);

    var info = new ProcessStartInfo
    {
        FileName = codex.Path,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true,
        StandardOutputEncoding = Encoding.UTF8,
        StandardErrorEncoding = Encoding.UTF8
    };
    info.Environment["CODEX_HOME"] = codexHome;
    info.Environment["CODEX_SQLITE_HOME"] = codexHome;

    foreach (var argument in codex.Arguments)
    {
        info.ArgumentList.Add(argument);
    }

    info.ArgumentList.Add("app-server");
    info.ArgumentList.Add("--listen");
    info.ArgumentList.Add($"ws://127.0.0.1:{port}");

    return Process.Start(info) ??
           throw new InvalidOperationException("Failed to start Codex app-server");
}

static async Task<ClientWebSocket> ConnectWebSocketAsync(int port)
{
    var socket = new ClientWebSocket();
    var uri = new Uri($"ws://127.0.0.1:{port}");
    Exception? lastError = null;

    for (var i = 0; i < 50; i++)
    {
        try
        {
            using var cts = new CancellationTokenSource(TimeSpan.FromSeconds(2));
            await socket.ConnectAsync(uri, cts.Token);
            return socket;
        }
        catch (Exception ex)
        {
            lastError = ex;
            await Task.Delay(100);
        }
    }

    socket.Dispose();
    throw new InvalidOperationException($"Codex app-server websocket did not open: {lastError?.Message}");
}

static CodexCommand FindCodexExecutable()
{
    var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
    var native = Path.Combine(appData, "npm", "node_modules", "@openai", "codex",
        "node_modules", "@openai", "codex-win32-x64", "vendor",
        "x86_64-pc-windows-msvc", "codex", "codex.exe");
    if (File.Exists(native))
    {
        return new CodexCommand(native, []);
    }

    var npmCodexPs1 = Path.Combine(appData, "npm", "codex.ps1");
    if (File.Exists(npmCodexPs1))
    {
        return new CodexCommand("pwsh", ["-NoProfile", "-ExecutionPolicy", "Bypass", "-File", npmCodexPs1]);
    }

    return new CodexCommand("codex", []);
}

static async Task SendJsonWebSocketAsync(ClientWebSocket socket, object payload)
{
    var json = JsonSerializer.Serialize(payload, CreateJsonOptions());
    var bytes = Encoding.UTF8.GetBytes(json);
    await socket.SendAsync(bytes, WebSocketMessageType.Text, true,
                           CancellationToken.None);
}

static async Task<JsonNode> ReadResultWebSocketAsync(ClientWebSocket socket,
                                                     int id,
                                                     TimeSpan timeout)
{
    using var cts = new CancellationTokenSource(timeout);

    while (!cts.IsCancellationRequested)
    {
        var text = await ReceiveTextAsync(socket, cts.Token);
        if (string.IsNullOrWhiteSpace(text))
        {
            continue;
        }

        JsonNode? node;
        try
        {
            node = JsonNode.Parse(text);
        }
        catch
        {
            continue;
        }

        if (node?["id"]?.GetValue<int?>() != id)
        {
            continue;
        }

        if (node["error"] is not null)
        {
            throw new InvalidOperationException(node["error"]!.ToJsonString());
        }

        return node["result"] ??
               throw new InvalidOperationException($"Response {id} had no result");
    }

    throw new TimeoutException($"Codex app-server response {id} timed out");
}

static async Task<string> ReceiveTextAsync(ClientWebSocket socket,
                                           CancellationToken cancellationToken)
{
    var buffer = new byte[64 * 1024];
    using var stream = new MemoryStream();

    while (true)
    {
        var result = await socket.ReceiveAsync(buffer, cancellationToken);
        if (result.MessageType == WebSocketMessageType.Close)
        {
            throw new EndOfStreamException("Codex app-server websocket closed");
        }

        stream.Write(buffer, 0, result.Count);
        if (result.EndOfMessage)
        {
            return Encoding.UTF8.GetString(stream.ToArray());
        }
    }
}

static Usage30d ReadLocalUsage30d(string codexHome)
{
    var stateDb = Path.Combine(codexHome, "state_5.sqlite");
    if (!File.Exists(stateDb))
    {
        return new Usage30d(0, 0);
    }

    var sqlite = FindSqliteExecutable();
    if (sqlite is null)
    {
        return new Usage30d(0, 0);
    }

    const string query = """
        select coalesce(sum(tokens_used), 0) || '|' || count(*)
        from threads
        where updated_at >= unixepoch('now', '-30 days');
        """;

    using var process = new Process();
    process.StartInfo = new ProcessStartInfo
    {
        FileName = sqlite,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        UseShellExecute = false,
        CreateNoWindow = true,
        StandardOutputEncoding = Encoding.UTF8,
        StandardErrorEncoding = Encoding.UTF8
    };
    process.StartInfo.ArgumentList.Add(stateDb);
    process.StartInfo.ArgumentList.Add(query);

    process.Start();
    var output = process.StandardOutput.ReadToEnd();
    process.WaitForExit(5000);
    if (!process.HasExited)
    {
        TryKill(process);
        return new Usage30d(0, 0);
    }

    var parts = output.Trim().Split('|');
    if (parts.Length != 2 ||
        !long.TryParse(parts[0], out var tokens) ||
        !long.TryParse(parts[1], out var threads))
    {
        return new Usage30d(0, 0);
    }

    return new Usage30d(tokens, threads);
}

static TaskStateSnapshot ReadLatestTaskState(string codexHome)
{
    var sessionsRoot = Path.Combine(codexHome, "sessions");
    if (!Directory.Exists(sessionsRoot))
    {
        return TaskStateSnapshot.Idle();
    }

    var latestSession = Directory.EnumerateFiles(sessionsRoot, "*.jsonl",
            SearchOption.AllDirectories)
        .Select(path => new FileInfo(path))
        .OrderByDescending(file => file.LastWriteTimeUtc)
        .FirstOrDefault();
    if (latestSession is null)
    {
        return TaskStateSnapshot.Idle();
    }

    string state = "IDLE";
    string label = "IDLE";
    string? title = null;
    long updatedAtUnix = new DateTimeOffset(latestSession.LastWriteTimeUtc)
        .ToUnixTimeSeconds();
    var openToolCalls = new HashSet<string>(StringComparer.Ordinal);

    foreach (var line in ReadRecentLines(latestSession.FullName, 500))
    {
        JsonNode? node;
        try
        {
            node = JsonNode.Parse(line);
        }
        catch
        {
            continue;
        }

        var timestamp = node?["timestamp"]?.GetValue<DateTimeOffset?>();
        if (timestamp is not null)
        {
            updatedAtUnix = timestamp.Value.ToUnixTimeSeconds();
        }

        var type = node?["type"]?.GetValue<string>();
        var payload = node?["payload"];
        var payloadType = payload?["type"]?.GetValue<string>();

        if (payloadType == "user_message")
        {
            title = FirstNonEmptyString(payload?["message"], payload?["text"],
                payload?["content"]) ?? title;
            state = "RUN";
            label = "RUN";
            continue;
        }

        if (type == "event_msg" && payloadType == "task_started")
        {
            state = "RUN";
            label = "RUN";
            continue;
        }

        if (type == "response_item" && payloadType == "function_call")
        {
            var callId = payload?["call_id"]?.GetValue<string>() ??
                         payload?["id"]?.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(callId))
            {
                openToolCalls.Add(callId);
            }

            state = "TOOL";
            label = "TOOL";
            continue;
        }

        if (type == "response_item" && payloadType == "function_call_output")
        {
            var callId = payload?["call_id"]?.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(callId))
            {
                openToolCalls.Remove(callId);
            }

            state = openToolCalls.Count > 0 ? "TOOL" : "RUN";
            label = state;
            continue;
        }

        if (type == "response_item" && payloadType == "reasoning")
        {
            state = "RUN";
            label = "RUN";
            continue;
        }

        if (type == "event_msg" && payloadType == "agent_message")
        {
            var phase = payload?["phase"]?.GetValue<string>();
            state = string.Equals(phase, "final_answer",
                StringComparison.OrdinalIgnoreCase) ? "IDLE" : "RUN";
            label = state;
            continue;
        }

        if (type == "response_item" && payloadType == "message")
        {
            var phase = payload?["phase"]?.GetValue<string>();
            state = string.Equals(phase, "final_answer",
                StringComparison.OrdinalIgnoreCase) ? "IDLE" : "RUN";
            label = state;
            continue;
        }

        if (type == "event_msg" && payloadType == "task_complete")
        {
            state = "IDLE";
            label = "DONE";
            openToolCalls.Clear();
            continue;
        }

        if (type == "event_msg" && payloadType == "turn_aborted")
        {
            state = "STOP";
            label = "STOP";
            openToolCalls.Clear();
        }
    }

    if (openToolCalls.Count > 0)
    {
        state = "TOOL";
        label = "TOOL";
    }

    return new TaskStateSnapshot(state, label, ShortenTitle(title), updatedAtUnix);
}

static IEnumerable<string> ReadRecentLines(string path, int maxLines)
{
    var queue = new Queue<string>(maxLines);
    using var stream = new FileStream(path, FileMode.Open, FileAccess.Read,
        FileShare.ReadWrite | FileShare.Delete);
    using var reader = new StreamReader(stream, Encoding.UTF8);
    while (reader.ReadLine() is { } line)
    {
        if (queue.Count == maxLines)
        {
            queue.Dequeue();
        }

        queue.Enqueue(line);
    }

    return queue;
}

static string? FirstNonEmptyString(params JsonNode?[] nodes)
{
    foreach (var node in nodes)
    {
        if (node is null)
        {
            continue;
        }

        var value = node.GetValueKind() == JsonValueKind.String
            ? node.GetValue<string>()
            : node.ToJsonString();
        if (!string.IsNullOrWhiteSpace(value))
        {
            return value.Trim();
        }
    }

    return null;
}

static string? ShortenTitle(string? value)
{
    if (string.IsNullOrWhiteSpace(value))
    {
        return null;
    }

    value = value.Replace("\r", " ").Replace("\n", " ").Trim();
    while (value.Contains("  ", StringComparison.Ordinal))
    {
        value = value.Replace("  ", " ");
    }

    return value.Length <= 48 ? value : $"{value[..45]}...";
}

static string? FindSqliteExecutable()
{
    var path = Environment.GetEnvironmentVariable("PATH") ?? "";
    foreach (var dir in path.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
    {
        var candidate = Path.Combine(dir.Trim(), "sqlite3.exe");
        if (File.Exists(candidate))
        {
            return candidate;
        }
    }

    var userProfile = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
    var androidSdk = Path.Combine(userProfile, "AppData", "Local", "Android",
        "Sdk", "platform-tools", "sqlite3.exe");
    return File.Exists(androidSdk) ? androidSdk : null;
}

static void WriteStatus(CodexStatus status)
{
    new WidgetStateStore().Write("codex-status", status,
        string.Equals(status.Status, "ok", StringComparison.OrdinalIgnoreCase) ? "ok" : "error",
        status.Error);
}

static string GetStatusDirectory()
{
    return AppPaths.AppDirectory;
}

static string GetStatusPath() => WidgetStateStore.PathFor("codex-status");

static void TryKill(Process process)
{
    try
    {
        if (!process.HasExited)
        {
            process.Kill(entireProcessTree: true);
        }
    }
    catch
    {
        // Best effort.
    }
}

static JsonSerializerOptions CreateJsonOptions() => new()
{
    PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    WriteIndented = true
};

internal sealed record CodexCommand(string Path, string[] Arguments);
internal sealed record Usage30d(long Tokens, long Threads);
internal sealed record TaskStateSnapshot(
    string State,
    string Label,
    string? Title,
    long UpdatedAtUnix)
{
    public static TaskStateSnapshot Idle() =>
        new("IDLE", "IDLE", null, DateTimeOffset.UtcNow.ToUnixTimeSeconds());
}

internal sealed class AppServerStatus
{
    public string? PlanType { get; init; }
    public double? PrimaryUsedPercent { get; init; }
    public long? PrimaryWindowMins { get; init; }
    public long? PrimaryResetsAtUnix { get; init; }
    public double? SecondaryUsedPercent { get; init; }
    public long? SecondaryWindowMins { get; init; }
    public long? SecondaryResetsAtUnix { get; init; }
    public string? CreditsBalance { get; init; }
    public bool? CreditsUnlimited { get; init; }
}

internal sealed class CodexStatus
{
    public int Version { get; init; }
    public long UpdatedAtUnix { get; init; }
    public string Status { get; set; } = "ok";
    public string? Error { get; set; }
    public string? ActiveAccountId { get; set; }
    public string? ActiveAccountLabel { get; set; }
    public string? PlanType { get; set; }
    public double? PrimaryUsedPercent { get; set; }
    public long? PrimaryWindowMins { get; set; }
    public long? PrimaryResetsAtUnix { get; set; }
    public double? SecondaryUsedPercent { get; set; }
    public long? SecondaryWindowMins { get; set; }
    public long? SecondaryResetsAtUnix { get; set; }
    public string? CreditsBalance { get; set; }
    public bool? CreditsUnlimited { get; set; }
    public long Tokens30d { get; set; }
    public long ThreadCount30d { get; set; }
    public string TaskState { get; set; } = "IDLE";
    public string TaskLabel { get; set; } = "IDLE";
    public string? TaskTitle { get; set; }
    public long TaskUpdatedAtUnix { get; set; }
}

}

