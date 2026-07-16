using System.Text.Json.Nodes;

namespace TaskbarWidgets.Loader.Core;

internal static class CommunityWidgetUpdateChecker
{
    private const string LibraryBaseUrl = "https://pfcsoft.com/twidget_library";
    private static readonly TimeSpan CheckInterval = TimeSpan.FromHours(6);
    private static readonly TimeSpan InitialDelay = TimeSpan.FromSeconds(20);
    private static readonly HttpClient Client = CreateClient();

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        try
        {
            await Task.Delay(InitialDelay, cancellationToken);
            while (!cancellationToken.IsCancellationRequested)
            {
                await CheckAsync(cancellationToken);
                await Task.Delay(CheckInterval, cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            // Normal loader shutdown.
        }
    }

    internal static async Task CheckAsync(CancellationToken cancellationToken)
    {
        try
        {
            var index = await ReadObjectAsync($"{LibraryBaseUrl}/index.json", cancellationToken);
            if (index["schemaVersion"]?.GetValue<int?>() != 1 || index["widgets"] is not JsonArray ids || ids.Count > 200)
            {
                throw new InvalidDataException("The community library index is invalid.");
            }

            var installed = CommunityWidgetRegistry.Entries
                .Where(widget => widget.Valid && !widget.Trusted)
                .ToDictionary(widget => widget.Id, StringComparer.OrdinalIgnoreCase);
            var updates = new JsonArray();
            foreach (var idNode in ids)
            {
                var id = idNode?.GetValue<string>();
                if (string.IsNullOrWhiteSpace(id) || !installed.TryGetValue(id, out var local)) continue;
                var info = await ReadObjectAsync($"{LibraryBaseUrl}/{id}/info.json", cancellationToken);
                var remoteId = info["id"]?.GetValue<string>();
                var availableVersion = info["version"]?.GetValue<string>();
                if (!string.Equals(remoteId, id, StringComparison.Ordinal) ||
                    !IsNewerVersion(availableVersion, local.Version)) continue;
                updates.Add(new JsonObject
                {
                    ["widgetId"] = id,
                    ["installedVersion"] = local.Version,
                    ["availableVersion"] = availableVersion,
                    ["displayName"] = info["displayName"]?.GetValue<string>() ?? local.DisplayName,
                    ["authorName"] = info["author"]?["name"]?.GetValue<string>() ?? local.AuthorName
                });
            }

            WriteState("ok", null, updates);
        }
        catch (Exception ex) when (ex is not OperationCanceledException)
        {
            var previous = ReadPreviousUpdates();
            WriteState("error", ex.Message, previous);
            Log($"Community update check failed: {ex.Message}");
        }
    }

    internal static bool IsNewerVersion(string? available, string installed)
    {
        return Version.TryParse(available, out var remote) && Version.TryParse(installed, out var local) && remote > local;
    }

    private static async Task<JsonObject> ReadObjectAsync(string url, CancellationToken cancellationToken)
    {
        using var response = await Client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is > 262_144)
        {
            throw new InvalidDataException("The community library response is too large.");
        }
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        var node = await JsonNode.ParseAsync(stream, cancellationToken: cancellationToken);
        return node?.AsObject() ?? throw new InvalidDataException("The community library response must be a JSON object.");
    }

    private static void WriteState(string status, string? error, JsonArray updates)
    {
        AtomicJson.Write(
            AppPaths.CommunityWidgetUpdateStatePath,
            new JsonObject
            {
                ["schemaVersion"] = 1,
                ["status"] = status,
                ["checkedAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
                ["error"] = error,
                ["updates"] = updates
            },
            WidgetConfiguration.JsonOptions());
    }

    private static JsonArray ReadPreviousUpdates()
    {
        try
        {
            return JsonNode.Parse(File.ReadAllText(AppPaths.CommunityWidgetUpdateStatePath))?["updates"]?.DeepClone().AsArray()
                   ?? new JsonArray();
        }
        catch
        {
            return new JsonArray();
        }
    }

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(12), MaxResponseContentBufferSize = 262_144 };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TaskbarWidgets-CommunityUpdateChecker/1");
        return client;
    }

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.LogsDirectory);
            File.AppendAllText(Path.Combine(AppPaths.LogsDirectory, "loader.log"),
                $"[{DateTimeOffset.Now:O}] {message}{Environment.NewLine}");
        }
        catch
        {
            // Diagnostics must never stop the loader.
        }
    }
}
