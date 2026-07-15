using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class GitHubUpdater
{
    private const string Owner = "pfcdev";
    private const string Repo = "TaskbarWidgets";
    private const string SetupAssetName = ReleaseAssetPolicy.SetupName;
    private const string SetupShaAssetName = ReleaseAssetPolicy.SetupSha256Name;
    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string InstallDirectory = AppPaths.InstallDirectory;
    private static readonly string UpdatesDirectory = Path.Combine(AppDirectory, "Updates");
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string UpdateStatusPath = Path.Combine(AppDirectory, "update-status.json");

    public static async Task CheckAndInstallIfAvailableAsync(
        CancellationToken cancellationToken,
        bool silentSetup = false)
    {
        try
        {
            WriteStatus("checking", CurrentVersion().ToString(), "", false,
                "Checking GitHub latest release...");
            var release = await GetLatestReleaseAsync(cancellationToken);
            if (release is null || !release.IsNewerThan(CurrentVersion()))
            {
                Log("No GitHub update available");
                WriteStatus("current", CurrentVersion().ToString(), release?.TagName ?? "", false,
                    "TaskbarWidgets is up to date.");
                return;
            }

            Log($"GitHub update available: {release.TagName}");
            WriteStatus("downloading", CurrentVersion().ToString(), release.TagName, true,
                $"Downloading {release.TagName}...");
            var downloaded = await DownloadReleaseAsync(
                release,
                cancellationToken,
                (downloadedBytes, totalBytes) => WriteDownloadStatus(release, downloadedBytes, totalBytes));
            WriteStatus("installing", CurrentVersion().ToString(), release.TagName, true,
                silentSetup ? "Applying update..." : "Launching installer...");
            StartUpdateScriptAndExit(downloaded, silentSetup);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log($"Update check failed: {ex.Message}");
            WriteStatus("error", CurrentVersion().ToString(), "", false, ex.Message);
        }
    }

    public static async Task DownloadUpdateIfAvailableAsync(CancellationToken cancellationToken)
    {
        try
        {
            WriteStatus("checking", CurrentVersion().ToString(), "", false,
                "Checking GitHub latest release...");
            var release = await GetLatestReleaseAsync(cancellationToken);
            if (release is null || !release.IsNewerThan(CurrentVersion()))
            {
                Log("No GitHub update available");
                WriteStatus("current", CurrentVersion().ToString(), release?.TagName ?? "", false,
                    "TaskbarWidgets is up to date.");
                return;
            }

            Log($"GitHub update available for download: {release.TagName}");
            WriteStatus("downloading", CurrentVersion().ToString(), release.TagName, true,
                $"Downloading {release.TagName}...");
            var downloaded = await DownloadReleaseAsync(
                release,
                cancellationToken,
                (downloadedBytes, totalBytes) => WriteDownloadStatus(release, downloadedBytes, totalBytes));
            WriteStatus(
                "ready",
                CurrentVersion().ToString(),
                release.TagName,
                true,
                "Installer downloaded. Starting setup...",
                downloaded.Path);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log($"Update download failed: {ex.Message}");
            WriteStatus("error", CurrentVersion().ToString(), "", false, ex.Message);
        }
    }

    public static async Task CheckOnlyAsync(CancellationToken cancellationToken)
    {
        try
        {
            WriteStatus("checking", CurrentVersion().ToString(), "", false,
                "Checking GitHub latest release...");
            var release = await GetLatestReleaseAsync(cancellationToken);
            if (release is null)
            {
                Log("No GitHub release found");
                WriteStatus("no-release", CurrentVersion().ToString(), "", false,
                    "No GitHub release was found.");
                return;
            }

            var current = CurrentVersion();
            var available = release.IsNewerThan(current);
            Log(release.IsNewerThan(current)
                ? $"Update available: current={current}, latest={release.TagName}"
                : $"Already current: current={current}, latest={release.TagName}");
            WriteStatus(
                available ? "available" : "current",
                current.ToString(),
                release.TagName,
                available,
                available
                    ? $"Update available: {release.TagName}"
                    : "TaskbarWidgets is up to date.");
        }
        catch (Exception ex)
        {
            Log($"Update check failed: {ex.Message}");
            WriteStatus("error", CurrentVersion().ToString(), "", false, ex.Message);
        }
    }

    public static async Task CheckOnlyIfDueAsync(
        TimeSpan minimumInterval,
        CancellationToken cancellationToken)
    {
        if (!IsCheckDue(minimumInterval))
        {
            Log("Skipping GitHub update check; cached status is still fresh");
            return;
        }

        await CheckOnlyAsync(cancellationToken);
    }

    private static async Task<ReleaseInfo?> GetLatestReleaseAsync(CancellationToken cancellationToken)
    {
        try
        {
            return await GetLatestReleaseFromGitHubApiAsync(cancellationToken);
        }
        catch (Exception ex)
        {
            Log($"GitHub API latest release lookup failed, using redirect fallback: {ex.Message}");
            return await GetLatestReleaseFromRedirectAsync(cancellationToken);
        }
    }

    private static async Task<ReleaseInfo?> GetLatestReleaseFromGitHubApiAsync(CancellationToken cancellationToken)
    {
        using var client = CreateHttpClient();
        var url = $"https://api.github.com/repos/{Owner}/{Repo}/releases/latest";
        using var response = await client.GetAsync(url, cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException(
                $"GitHub latest release request failed: {(int)response.StatusCode} {response.ReasonPhrase}");
        }

        var json = JsonNode.Parse(await response.Content.ReadAsStringAsync(cancellationToken)) ??
                   throw new InvalidOperationException("GitHub latest release response was empty");
        var tag = json["tag_name"]?.GetValue<string>();
        if (string.IsNullOrWhiteSpace(tag))
        {
            return null;
        }

        var assets = new List<(string Name, string Url)>();
        foreach (var asset in json["assets"]?.AsArray() ?? [])
        {
            var name = asset?["name"]?.GetValue<string>();
            var downloadUrl = asset?["browser_download_url"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(downloadUrl))
            {
                continue;
            }

            assets.Add((name, downloadUrl));
        }

        var selected = ReleaseAssetPolicy.Select(assets);
        if (selected is not null)
        {
            return new ReleaseInfo(
                tag,
                SetupAssetName,
                selected.Value.DownloadUrl,
                selected.Value.Sha256Url,
                UpdatePackageKind.Setup);
        }

        return null;
    }

    private static async Task<ReleaseInfo?> GetLatestReleaseFromRedirectAsync(CancellationToken cancellationToken)
    {
        using var handler = new HttpClientHandler { AllowAutoRedirect = false };
        using var client = CreateHttpClient(handler);
        using var response = await client.GetAsync(
            $"https://github.com/{Owner}/{Repo}/releases/latest",
            cancellationToken);
        var location = response.Headers.Location?.ToString();
        if (string.IsNullOrWhiteSpace(location))
        {
            throw new InvalidOperationException(
                $"GitHub latest release redirect failed: {(int)response.StatusCode} {response.ReasonPhrase}");
        }

        var tag = location.TrimEnd('/').Split('/').LastOrDefault();
        if (string.IsNullOrWhiteSpace(tag))
        {
            return null;
        }

        var baseUrl = $"https://github.com/{Owner}/{Repo}/releases/download/{tag}";
        return new ReleaseInfo(
            tag,
            SetupAssetName,
            $"{baseUrl}/{SetupAssetName}",
            $"{baseUrl}/{SetupShaAssetName}",
            UpdatePackageKind.Setup);
    }

    private static async Task<DownloadedUpdate> DownloadReleaseAsync(
        ReleaseInfo release,
        CancellationToken cancellationToken,
        Action<long, long?>? progress = null)
    {
        Directory.CreateDirectory(UpdatesDirectory);
        var directory = Path.Combine(UpdatesDirectory, release.TagName);
        Directory.CreateDirectory(directory);
        var filePath = Path.Combine(directory, release.AssetName);

        using var client = CreateHttpClient();
        using var response = await client.GetAsync(
            release.DownloadUrl,
            HttpCompletionOption.ResponseHeadersRead,
            cancellationToken);
        response.EnsureSuccessStatusCode();

        var totalBytes = response.Content.Headers.ContentLength;
        await using (var input = await response.Content.ReadAsStreamAsync(cancellationToken))
        await using (var output = File.Create(filePath))
        {
            var buffer = new byte[128 * 1024];
            long downloadedBytes = 0;
            var lastProgress = DateTimeOffset.MinValue;
            progress?.Invoke(downloadedBytes, totalBytes);

            while (true)
            {
                var read = await input.ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken);
                if (read <= 0)
                {
                    break;
                }

                await output.WriteAsync(buffer.AsMemory(0, read), cancellationToken);
                downloadedBytes += read;

                var now = DateTimeOffset.UtcNow;
                if (now - lastProgress >= TimeSpan.FromMilliseconds(400) ||
                    (totalBytes.HasValue && downloadedBytes >= totalBytes.Value))
                {
                    lastProgress = now;
                    progress?.Invoke(downloadedBytes, totalBytes);
                }
            }

            progress?.Invoke(downloadedBytes, totalBytes);
        }

        var fileInfo = new FileInfo(filePath);
        if (!fileInfo.Exists || fileInfo.Length < 1024 * 1024)
        {
            throw new InvalidOperationException("Downloaded update is unexpectedly small");
        }

        if (!string.IsNullOrWhiteSpace(release.Sha256Url))
        {
            var shaText = await client.GetStringAsync(release.Sha256Url, cancellationToken);
            var expected = shaText.Split([' ', '\t', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
                .FirstOrDefault();
            if (!string.IsNullOrWhiteSpace(expected))
            {
                var downloadedBytes = await File.ReadAllBytesAsync(filePath, cancellationToken);
                var actual = Convert.ToHexString(SHA256.HashData(downloadedBytes)).ToLowerInvariant();
                if (!string.Equals(expected.Trim().ToLowerInvariant(), actual, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException("Downloaded update SHA256 verification failed");
                }
            }
        }

        Log($"Downloaded update {release.TagName} to {filePath}");
        return new DownloadedUpdate(filePath, release.Kind);
    }

    private static void StartUpdateScriptAndExit(DownloadedUpdate update, bool silentSetup)
    {
        var scriptPath = Path.Combine(Path.GetDirectoryName(update.Path)!, "apply-update.cmd");
        var currentPid = Environment.ProcessId;
        var setupArguments = silentSetup ? " /S" : "";
        var script = update.Kind switch
        {
            UpdatePackageKind.Setup => $"""
@echo off
setlocal
set "SRC={update.Path}"
set "DIR={InstallDirectory}"
set "DATA={AppDirectory}"
set "PID={currentPid}"
set "LOG=%DATA%\Logs\loader.log"
:wait
tasklist /FI "PID eq %PID%" | find "%PID%" >nul
if not errorlevel 1 (
  timeout /t 1 /nobreak >nul
  goto wait
)
if not exist "%DATA%\Logs" mkdir "%DATA%\Logs" >nul 2>nul
>>"%LOG%" echo %DATE% %TIME% [updater] Starting setup package: "%SRC%"{setupArguments}
start /wait "" "%SRC%"{setupArguments}
>>"%LOG%" echo %DATE% %TIME% [updater] Setup package exited with code %ERRORLEVEL%.
if exist "%DIR%\TaskbarWidgets.exe" start "" "%DIR%\TaskbarWidgets.exe"
del "%~f0"
""",
            _ => throw new InvalidOperationException("Unsupported update package kind")
        };
        File.WriteAllText(scriptPath, script);

        Process.Start(new ProcessStartInfo
        {
            FileName = "cmd.exe",
            UseShellExecute = false,
            CreateNoWindow = true,
            ArgumentList = { "/c", scriptPath }
        });

        Log(silentSetup
            ? "Silent update apply script started; exiting current process"
            : "Interactive update installer script started; exiting current process");
        Environment.Exit(0);
    }

    private static HttpClient CreateHttpClient(HttpMessageHandler? handler = null)
    {
        var client = handler is null ? new HttpClient() : new HttpClient(handler);
        client.Timeout = TimeSpan.FromMinutes(10);
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TaskbarWidgets/0.1");
        client.DefaultRequestHeaders.Accept.ParseAdd("application/vnd.github+json");
        return client;
    }

    private static bool IsCheckDue(TimeSpan minimumInterval)
    {
        try
        {
            if (!File.Exists(UpdateStatusPath))
            {
                return true;
            }

            using var document = JsonDocument.Parse(File.ReadAllText(UpdateStatusPath));
            if (!document.RootElement.TryGetProperty("updatedAtUnix", out var updatedAt) ||
                !updatedAt.TryGetInt64(out var updatedAtUnix) ||
                updatedAtUnix <= 0)
            {
                return true;
            }

            var age = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - updatedAtUnix;
            return age < 0 || age >= minimumInterval.TotalSeconds;
        }
        catch
        {
            return true;
        }
    }

    private static Version CurrentVersion()
    {
        var processPath = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(processPath))
        {
            var fileVersion = FileVersionInfo.GetVersionInfo(processPath).FileVersion;
            if (Version.TryParse(fileVersion, out var version))
            {
                return version;
            }
        }

        return Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0, 0);
    }

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(LogsDirectory);
            File.AppendAllText(
                LogPath,
                $"{DateTimeOffset.Now:O} [updater] {message}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never break update checks.
        }
    }

    private static void WriteStatus(
        string state,
        string currentVersion,
        string latestVersion,
        bool updateAvailable,
        string message,
        string? installerPath = null,
        double? progressPercent = null,
        long? downloadedBytes = null,
        long? totalBytes = null)
    {
        try
        {
            Directory.CreateDirectory(AppDirectory);
            var payload = new
            {
                state,
                currentVersion,
                latestVersion,
                updateAvailable,
                message,
                installerPath,
                progressPercent,
                downloadedBytes,
                totalBytes,
                updatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
            };
            var json = JsonSerializer.Serialize(
                payload,
                new JsonSerializerOptions { WriteIndented = true });
            File.WriteAllText(UpdateStatusPath, json + Environment.NewLine);
        }
        catch
        {
            // Status writes are best-effort and must not break updates.
        }
    }

    private static void WriteDownloadStatus(
        ReleaseInfo release,
        long downloadedBytes,
        long? totalBytes)
    {
        double? progressPercent = totalBytes is > 0
            ? Math.Round(downloadedBytes * 100d / totalBytes.Value, 1)
            : null;
        var message = progressPercent.HasValue
            ? $"Downloading {release.TagName}... {progressPercent.Value:0.#}%"
            : $"Downloading {release.TagName}... {FormatBytes(downloadedBytes)}";

        WriteStatus(
            "downloading",
            CurrentVersion().ToString(),
            release.TagName,
            true,
            message,
            progressPercent: progressPercent,
            downloadedBytes: downloadedBytes,
            totalBytes: totalBytes);
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["B", "KB", "MB", "GB"];
        double value = bytes;
        var index = 0;
        while (value >= 1024 && index < units.Length - 1)
        {
            value /= 1024;
            index++;
        }

        return $"{value:0.#} {units[index]}";
    }

    private enum UpdatePackageKind
    {
        Setup
    }

    private sealed record DownloadedUpdate(string Path, UpdatePackageKind Kind);

    private sealed record ReleaseInfo(
        string TagName,
        string AssetName,
        string DownloadUrl,
        string? Sha256Url,
        UpdatePackageKind Kind)
    {
        public bool IsNewerThan(Version current)
        {
            var normalized = TagName.TrimStart('v', 'V');
            return Version.TryParse(normalized, out var latest) && latest > current;
        }
    }
}
