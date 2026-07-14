using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace TaskbarStatsProduct;

internal static class GitHubUpdater
{
    private const string Owner = "pfcdev";
    private const string Repo = "TaskWidgets";
    private const string ExeAssetName = "TaskbarStats.exe";
    private const string ExeShaAssetName = "TaskbarStats.exe.sha256";
    private const string MsiAssetName = "TaskbarStats.msi";
    private const string MsiShaAssetName = "TaskbarStats.msi.sha256";
    private const string SetupAssetName = "TaskbarStatsSetup.exe";
    private const string SetupShaAssetName = "TaskbarStatsSetup.exe.sha256";
    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string UpdatesDirectory = Path.Combine(AppDirectory, "Updates");
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string UpdateStatusPath = Path.Combine(AppDirectory, "update-status.json");

    public static async Task CheckAndInstallIfAvailableAsync(CancellationToken cancellationToken)
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
                    "TaskbarStats is up to date.");
                return;
            }

            Log($"GitHub update available: {release.TagName}");
            WriteStatus("downloading", CurrentVersion().ToString(), release.TagName, true,
                $"Downloading {release.TagName}...");
            var downloaded = await DownloadReleaseAsync(release, cancellationToken);
            WriteStatus("installing", CurrentVersion().ToString(), release.TagName, true,
                "Applying update...");
            StartUpdateScriptAndExit(downloaded);
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
                    : "TaskbarStats is up to date.");
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

        string? exeUrl = null;
        string? exeShaUrl = null;
        string? msiUrl = null;
        string? msiShaUrl = null;
        string? setupUrl = null;
        string? setupShaUrl = null;
        foreach (var asset in json["assets"]?.AsArray() ?? [])
        {
            var name = asset?["name"]?.GetValue<string>();
            var downloadUrl = asset?["browser_download_url"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(name) || string.IsNullOrWhiteSpace(downloadUrl))
            {
                continue;
            }

            if (string.Equals(name, MsiAssetName, StringComparison.OrdinalIgnoreCase))
            {
                msiUrl = downloadUrl;
            }
            else if (string.Equals(name, MsiShaAssetName, StringComparison.OrdinalIgnoreCase))
            {
                msiShaUrl = downloadUrl;
            }
            else if (string.Equals(name, SetupAssetName, StringComparison.OrdinalIgnoreCase))
            {
                setupUrl = downloadUrl;
            }
            else if (string.Equals(name, SetupShaAssetName, StringComparison.OrdinalIgnoreCase))
            {
                setupShaUrl = downloadUrl;
            }
            else if (string.Equals(name, ExeAssetName, StringComparison.OrdinalIgnoreCase))
            {
                exeUrl = downloadUrl;
            }
            else if (string.Equals(name, ExeShaAssetName, StringComparison.OrdinalIgnoreCase))
            {
                exeShaUrl = downloadUrl;
            }
        }

        if (!string.IsNullOrWhiteSpace(setupUrl))
        {
            return new ReleaseInfo(tag, SetupAssetName, setupUrl, setupShaUrl, UpdatePackageKind.Setup);
        }

        if (!string.IsNullOrWhiteSpace(msiUrl))
        {
            return new ReleaseInfo(tag, MsiAssetName, msiUrl, msiShaUrl, UpdatePackageKind.Msi);
        }

        return string.IsNullOrWhiteSpace(exeUrl)
            ? null
            : new ReleaseInfo(tag, ExeAssetName, exeUrl, exeShaUrl, UpdatePackageKind.Exe);
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
        CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(UpdatesDirectory);
        var directory = Path.Combine(UpdatesDirectory, release.TagName);
        Directory.CreateDirectory(directory);
        var filePath = Path.Combine(directory, release.AssetName);

        using var client = CreateHttpClient();
        var bytes = await client.GetByteArrayAsync(release.DownloadUrl, cancellationToken);
        if (bytes.Length < 1024 * 1024)
        {
            throw new InvalidOperationException("Downloaded update is unexpectedly small");
        }

        await File.WriteAllBytesAsync(filePath, bytes, cancellationToken);

        if (!string.IsNullOrWhiteSpace(release.Sha256Url))
        {
            var shaText = await client.GetStringAsync(release.Sha256Url, cancellationToken);
            var expected = shaText.Split([' ', '\t', '\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
                .FirstOrDefault();
            if (!string.IsNullOrWhiteSpace(expected))
            {
                var actual = Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
                if (!string.Equals(expected.Trim().ToLowerInvariant(), actual, StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException("Downloaded update SHA256 verification failed");
                }
            }
        }

        Log($"Downloaded update {release.TagName} to {filePath}");
        return new DownloadedUpdate(filePath, release.Kind);
    }

    private static void StartUpdateScriptAndExit(DownloadedUpdate update)
    {
        var currentExe = Environment.ProcessPath ??
                         Process.GetCurrentProcess().MainModule?.FileName ??
                         throw new InvalidOperationException("Current executable path could not be resolved");
        var scriptPath = Path.Combine(Path.GetDirectoryName(update.Path)!, "apply-update.cmd");
        var currentPid = Environment.ProcessId;
        var script = update.Kind switch
        {
            UpdatePackageKind.Msi => $"""
@echo off
setlocal
set "SRC={update.Path}"
set "DIR={AppDirectory}"
set "PID={currentPid}"
:wait
tasklist /FI "PID eq %PID%" | find "%PID%" >nul
if not errorlevel 1 (
  timeout /t 1 /nobreak >nul
  goto wait
)
taskkill /IM TaskbarStatsMediaHelper.exe /F >nul 2>nul
taskkill /IM TaskbarStatsSettings.exe /F >nul 2>nul
msiexec.exe /i "%SRC%" /qn /norestart
if exist "%DIR%\TaskbarStats.exe" start "" "%DIR%\TaskbarStats.exe"
del "%~f0"
""",
            UpdatePackageKind.Setup => $"""
@echo off
setlocal
set "SRC={update.Path}"
set "DIR={AppDirectory}"
set "PID={currentPid}"
:wait
tasklist /FI "PID eq %PID%" | find "%PID%" >nul
if not errorlevel 1 (
  timeout /t 1 /nobreak >nul
  goto wait
)
start /wait "" "%SRC%" /S
if exist "%DIR%\TaskbarStats.exe" start "" "%DIR%\TaskbarStats.exe"
del "%~f0"
""",
            _ => $"""
@echo off
setlocal
set "SRC={update.Path}"
set "DST={currentExe}"
set "PID={currentPid}"
:wait
tasklist /FI "PID eq %PID%" | find "%PID%" >nul
if not errorlevel 1 (
  timeout /t 1 /nobreak >nul
  goto wait
)
copy /Y "%SRC%" "%DST%" >nul
start "" "%DST%"
del "%~f0"
"""
        };
        File.WriteAllText(scriptPath, script);

        Process.Start(new ProcessStartInfo
        {
            FileName = "cmd.exe",
            UseShellExecute = false,
            CreateNoWindow = true,
            ArgumentList = { "/c", scriptPath }
        });

        Log("Update apply script started; exiting current process");
        Environment.Exit(0);
    }

    private static HttpClient CreateHttpClient(HttpMessageHandler? handler = null)
    {
        var client = handler is null ? new HttpClient() : new HttpClient(handler);
        client.Timeout = TimeSpan.FromSeconds(12);
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TaskbarStats/0.1");
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
        string message)
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

    private enum UpdatePackageKind
    {
        Exe,
        Setup,
        Msi
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
