using System.Diagnostics;
using System.Globalization;
using System.Net.Http;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using Microsoft.Win32;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class SteamDownloadWorker
{
    private static readonly TimeSpan RefreshInterval = TimeSpan.FromSeconds(2);
    private static readonly string AppDirectory = AppPaths.AppDirectory;
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string WidgetAssetsDirectory = Path.Combine(AppDirectory, "Assets", "widgets");
    private static readonly WidgetStateStore StateStore = new();
    private static readonly HttpClient Http = new()
    {
        Timeout = TimeSpan.FromSeconds(10)
    };

    private static readonly Regex PairRegex = new("\"([^\"]+)\"\\s+\"([^\"]*)\"", RegexOptions.Compiled);
    private static readonly Regex PathRegex = new("\"path\"\\s+\"([^\"]+)\"", RegexOptions.IgnoreCase | RegexOptions.Compiled);
    private static readonly Regex LogRateRegex = new(
        "\\[(?<time>\\d{4}-\\d{2}-\\d{2} \\d{2}:\\d{2}:\\d{2})\\].*?(?:Current download rate:|Download rate).*?(?<value>[\\d.]+)\\s*Mbps",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(LogsDirectory);
        Directory.CreateDirectory(WidgetAssetsDirectory);

        var previousBytes = new Dictionary<string, (long bytes, DateTimeOffset at)>(StringComparer.OrdinalIgnoreCase);

        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var snapshot = await BuildSnapshotAsync(previousBytes, cancellationToken);
                WriteStatus(snapshot);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception ex)
            {
                Log($"Steam download update failed: {ex.Message}");
                WriteStatus(SteamDownloadSnapshot.ErrorSnapshot(ex.Message));
            }

            await Task.Delay(RefreshInterval, cancellationToken);
        }
    }

    private static async Task<SteamDownloadSnapshot> BuildSnapshotAsync(
        Dictionary<string, (long bytes, DateTimeOffset at)> previousBytes,
        CancellationToken cancellationToken)
    {
        var steamRunning = SteamIsRunning();
        var steamRoot = FindSteamRoot();
        if (steamRoot is null)
        {
            return new SteamDownloadSnapshot
            {
                Loaded = true,
                SteamRunning = steamRunning,
                Active = false,
                Status = steamRunning ? "steam_not_found" : "steam_closed",
                Title = steamRunning ? "Steam" : "Steam kapali",
                Subtitle = steamRunning ? "Kurulum klasoru bulunamadi" : "Indirme yok",
                UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
            };
        }

        var folders = FindSteamappsFolders(steamRoot);
        var items = ReadActiveDownloads(folders);
        if (items.Count == 0)
        {
            previousBytes.Clear();
            return new SteamDownloadSnapshot
            {
                Loaded = true,
                SteamRunning = steamRunning,
                Active = false,
                Status = steamRunning ? "idle" : "steam_closed",
                Title = steamRunning ? "Steam" : "Steam kapali",
                Subtitle = steamRunning ? "Indirme yok" : "Steam calismiyor",
                UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
            };
        }

        var item = items[0];
        var now = DateTimeOffset.UtcNow;
        var downloaded = Math.Max(item.BytesDownloaded, item.BytesStaged);
        var total = item.BytesToDownload > 0 ? item.BytesToDownload : item.BytesToStage;
        var speed = EstimateSpeedBytesPerSecond(item, steamRoot, previousBytes, now);
        previousBytes[item.AppId] = (downloaded, now);

        foreach (var appId in previousBytes.Keys.Where(appId => items.All(item => item.AppId != appId)).ToList())
        {
            previousBytes.Remove(appId);
        }

        var remainingBytes = total > 0 ? Math.Max(0, total - downloaded) : 0;
        var remainingSeconds = speed > 0 && remainingBytes > 0
            ? (long)Math.Ceiling(remainingBytes / speed)
            : 0;
        var progress = total > 0
            ? Math.Clamp(downloaded * 100.0 / total, 0, 100)
            : 0;
        var coverPath = await EnsureHeaderImageAsync(item.AppId, cancellationToken);
        var status = speed > 0 ? "downloading" : "paused_or_queued";
        var statusText = speed > 0 ? "Downloading" : "Queued";
        var speedText = speed > 0 ? $"{BytesToMegabytes(speed):0.0} MB/s" : "0 MB/s";
        var remainingText = remainingSeconds > 0 ? FormatDuration(remainingSeconds) : "Bekleniyor";

        return new SteamDownloadSnapshot
        {
            Loaded = true,
            SteamRunning = steamRunning,
            Active = true,
            AppId = item.AppId,
            Name = item.Name,
            Title = item.Name,
            Subtitle = $"{statusText} • {progress:0}% • {speedText}",
            Detail = remainingSeconds > 0
                ? $"{remainingText} kaldi"
                : $"{FormatGigabytes(downloaded)} / {FormatGigabytes(total)}",
            Status = status,
            StoreUrl = $"https://store.steampowered.com/app/{item.AppId}",
            HeaderUrl = $"https://cdn.akamai.steamstatic.com/steam/apps/{item.AppId}/header.jpg",
            CoverPath = coverPath,
            ProgressPercent = Math.Round(progress, 1),
            DownloadedBytes = downloaded,
            DownloadedGb = Math.Round(BytesToGigabytes(downloaded), 3),
            TotalBytes = total,
            TotalGb = Math.Round(BytesToGigabytes(total), 3),
            RemainingBytes = remainingBytes,
            RemainingGb = Math.Round(BytesToGigabytes(remainingBytes), 3),
            SpeedBytesPerSecond = (long)Math.Round(speed),
            SpeedMbS = Math.Round(BytesToMegabytes(speed), 2),
            RemainingSeconds = remainingSeconds,
            RemainingText = remainingText,
            UpdatedAtUnix = now.ToUnixTimeSeconds()
        };
    }

    private static string? FindSteamRoot()
    {
        foreach (var lookup in new[]
                 {
                     (Registry.CurrentUser, @"SOFTWARE\Valve\Steam", "SteamPath"),
                     (Registry.CurrentUser, @"SOFTWARE\Valve\Steam", "SteamExe"),
                     (Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath")
                 })
        {
            try
            {
                using var key = lookup.Item1.OpenSubKey(lookup.Item2);
                var value = key?.GetValue(lookup.Item3) as string;
                if (string.IsNullOrWhiteSpace(value))
                {
                    continue;
                }

                var path = value.Replace('/', '\\');
                if (Path.GetExtension(path).Equals(".exe", StringComparison.OrdinalIgnoreCase))
                {
                    path = Path.GetDirectoryName(path) ?? path;
                }

                if (Directory.Exists(path))
                {
                    return path;
                }
            }
            catch
            {
                // Try the next registry location.
            }
        }

        var fallback = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
            "Steam");
        return Directory.Exists(fallback) ? fallback : null;
    }

    private static List<string> FindSteamappsFolders(string steamRoot)
    {
        var result = new List<string>();
        AddIfSteamapps(Path.Combine(steamRoot, "steamapps"), result);

        var libraryFile = Path.Combine(steamRoot, "steamapps", "libraryfolders.vdf");
        if (File.Exists(libraryFile))
        {
            var text = File.ReadAllText(libraryFile, Encoding.UTF8);
            foreach (Match match in PathRegex.Matches(text))
            {
                AddIfSteamapps(Path.Combine(match.Groups[1].Value.Replace(@"\\", "\\"), "steamapps"), result);
            }
        }

        return result;
    }

    private static void AddIfSteamapps(string path, List<string> result)
    {
        if (!Directory.Exists(path))
        {
            return;
        }

        var full = Path.GetFullPath(path);
        if (!result.Any(existing => string.Equals(existing, full, StringComparison.OrdinalIgnoreCase)))
        {
            result.Add(full);
        }
    }

    private static List<SteamDownloadItem> ReadActiveDownloads(IEnumerable<string> steamappsFolders)
    {
        var result = new List<SteamDownloadItem>();
        foreach (var steamapps in steamappsFolders)
        {
            foreach (var manifest in Directory.EnumerateFiles(steamapps, "appmanifest_*.acf"))
            {
                var data = ParseManifest(manifest);
                var appId = data.TryGetValue("appid", out var explicitAppId)
                    ? explicitAppId
                    : Path.GetFileNameWithoutExtension(manifest).Replace("appmanifest_", "", StringComparison.OrdinalIgnoreCase);
                var item = new SteamDownloadItem
                {
                    AppId = appId,
                    Name = data.TryGetValue("name", out var name) ? name : $"App {appId}",
                    ManifestPath = manifest,
                    SteamappsPath = steamapps,
                    StateFlags = ToLong(data.TryGetValue("StateFlags", out var flags) ? flags : null),
                    BytesToDownload = ToLong(data.TryGetValue("BytesToDownload", out var btd) ? btd : null),
                    BytesDownloaded = ToLong(data.TryGetValue("BytesDownloaded", out var bd) ? bd : null),
                    BytesToStage = ToLong(data.TryGetValue("BytesToStage", out var bts) ? bts : null),
                    BytesStaged = ToLong(data.TryGetValue("BytesStaged", out var bs) ? bs : null)
                };

                if (item.Active)
                {
                    result.Add(item);
                }
            }
        }

        return result.OrderBy(item => item.Name, StringComparer.CurrentCultureIgnoreCase).ToList();
    }

    private static Dictionary<string, string> ParseManifest(string path)
    {
        var text = File.ReadAllText(path, Encoding.UTF8);
        var values = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (Match match in PairRegex.Matches(text))
        {
            values[match.Groups[1].Value] = match.Groups[2].Value;
        }

        return values;
    }

    private static long ToLong(string? value) =>
        long.TryParse(value, NumberStyles.Integer, CultureInfo.InvariantCulture, out var result)
            ? result
            : 0;

    private static double EstimateSpeedBytesPerSecond(
        SteamDownloadItem item,
        string steamRoot,
        IReadOnlyDictionary<string, (long bytes, DateTimeOffset at)> previousBytes,
        DateTimeOffset now)
    {
        var downloaded = Math.Max(item.BytesDownloaded, item.BytesStaged);
        if (previousBytes.TryGetValue(item.AppId, out var previous))
        {
            var elapsed = (now - previous.at).TotalSeconds;
            var delta = downloaded - previous.bytes;
            if (elapsed > 0 && delta > 0)
            {
                return delta / elapsed;
            }
        }

        return ReadRecentLogSpeed(steamRoot) ?? 0;
    }

    private static double? ReadRecentLogSpeed(string steamRoot)
    {
        var logPath = Path.Combine(steamRoot, "logs", "content_log.txt");
        if (!File.Exists(logPath))
        {
            return null;
        }

        var info = new FileInfo(logPath);
        using var stream = File.Open(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        stream.Seek(Math.Max(0, info.Length - 2 * 1024 * 1024), SeekOrigin.Begin);
        using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        var text = reader.ReadToEnd();
        (DateTime at, double bytesPerSecond)? newest = null;
        foreach (Match match in LogRateRegex.Matches(text))
        {
            if (!DateTime.TryParseExact(
                    match.Groups["time"].Value,
                    "yyyy-MM-dd HH:mm:ss",
                    CultureInfo.InvariantCulture,
                    DateTimeStyles.AssumeLocal,
                    out var at) ||
                !double.TryParse(match.Groups["value"].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var mbps))
            {
                continue;
            }

            var value = mbps * 1_000_000 / 8;
            if (newest is null || at > newest.Value.at)
            {
                newest = (at, value);
            }
        }

        if (newest is null || Math.Abs((DateTime.Now - newest.Value.at).TotalSeconds) > 120)
        {
            return null;
        }

        return newest.Value.bytesPerSecond;
    }

    private static async Task<string> EnsureHeaderImageAsync(string appId, CancellationToken cancellationToken)
    {
        var path = Path.Combine(WidgetAssetsDirectory, $"steam_header_{SanitizeFileName(appId)}.jpg");
        if (File.Exists(path) && new FileInfo(path).Length > 4096)
        {
            return path;
        }

        var url = $"https://cdn.akamai.steamstatic.com/steam/apps/{appId}/header.jpg";
        try
        {
            var bytes = await Http.GetByteArrayAsync(url, cancellationToken);
            if (bytes.Length > 4096)
            {
                await File.WriteAllBytesAsync(path, bytes, cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (Exception ex)
        {
            Log($"Steam header download failed for {appId}: {ex.Message}");
        }

        return File.Exists(path) ? path : "";
    }

    private static string SanitizeFileName(string value)
    {
        var invalid = Path.GetInvalidFileNameChars();
        return new string(value.Select(ch => invalid.Contains(ch) ? '_' : ch).ToArray());
    }

    private static bool SteamIsRunning()
    {
        try
        {
            return Process.GetProcessesByName("steam").Length > 0;
        }
        catch
        {
            return false;
        }
    }

    private static string FormatDuration(long seconds)
    {
        seconds = Math.Max(0, seconds);
        var span = TimeSpan.FromSeconds(seconds);
        if (span.TotalHours >= 1)
        {
            return $"{(int)span.TotalHours}sa {span.Minutes}dk";
        }
        if (span.TotalMinutes >= 1)
        {
            return $"{span.Minutes}dk {span.Seconds}sn";
        }
        return $"{span.Seconds}sn";
    }

    private static string FormatGigabytes(long bytes) => $"{BytesToGigabytes(bytes):0.0} GB";

    private static double BytesToGigabytes(double bytes) => bytes / 1024 / 1024 / 1024;

    private static double BytesToMegabytes(double bytes) => bytes / 1024 / 1024;

    private static void WriteStatus(SteamDownloadSnapshot snapshot)
    {
        StateStore.Write("steam-download", snapshot);
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
                $"{DateTimeOffset.Now:O} [steam] {message}{Environment.NewLine}");
        }
        catch
        {
            // Logging must never break the loader.
        }
    }

    private sealed class SteamDownloadItem
    {
        public string AppId { get; init; } = "";
        public string Name { get; init; } = "";
        public string ManifestPath { get; init; } = "";
        public string SteamappsPath { get; init; } = "";
        public long StateFlags { get; init; }
        public long BytesToDownload { get; init; }
        public long BytesDownloaded { get; init; }
        public long BytesToStage { get; init; }
        public long BytesStaged { get; init; }

        public bool Active =>
            BytesToDownload > 0 && BytesDownloaded < BytesToDownload ||
            BytesToStage > 0 && BytesStaged < BytesToStage ||
            Directory.Exists(Path.Combine(SteamappsPath, "downloading", AppId)) ||
            (StateFlags & 0x100000) != 0;
    }

    private sealed class SteamDownloadSnapshot
    {
        public bool Loaded { get; init; }
        public bool SteamRunning { get; init; }
        public bool Active { get; init; }
        public string AppId { get; init; } = "";
        public string Name { get; init; } = "";
        public string Title { get; init; } = "Steam";
        public string Subtitle { get; init; } = "Indirme yok";
        public string Detail { get; init; } = "";
        public string Status { get; init; } = "idle";
        public string StoreUrl { get; init; } = "";
        public string HeaderUrl { get; init; } = "";
        public string CoverPath { get; init; } = "";
        public double ProgressPercent { get; init; }
        public long DownloadedBytes { get; init; }
        public double DownloadedGb { get; init; }
        public long TotalBytes { get; init; }
        public double TotalGb { get; init; }
        public long RemainingBytes { get; init; }
        public double RemainingGb { get; init; }
        public long SpeedBytesPerSecond { get; init; }
        public double SpeedMbS { get; init; }
        public long RemainingSeconds { get; init; }
        public string RemainingText { get; init; } = "";
        public long UpdatedAtUnix { get; init; }
        public string Error { get; init; } = "";

        public static SteamDownloadSnapshot ErrorSnapshot(string message) => new()
        {
            Loaded = true,
            Active = false,
            Status = "error",
            Title = "Steam",
            Subtitle = "Veri okunamadi",
            Error = message,
            UpdatedAtUnix = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
        };
    }
}
