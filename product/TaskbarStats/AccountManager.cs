using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace TaskbarStatsProduct;

internal static class AccountManager
{
    private const string DefaultAccountId = "default";
    private static readonly object SyncRoot = new();
    private static readonly string LocalAppData =
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
    private static readonly string UserProfile =
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
    private static readonly string AppDirectory = Path.Combine(LocalAppData, "TaskbarStats");
    private static readonly string AccountsDirectory = Path.Combine(AppDirectory, "Accounts");
    private static readonly string IdeProfilesDirectory = Path.Combine(AppDirectory, "IdeProfiles");
    private static readonly string CommandsDirectory = Path.Combine(AppDirectory, "Commands");
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string SettingsPath = Path.Combine(AppDirectory, "settings.json");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string RealCodexHome = Path.Combine(UserProfile, ".codex");
    private static readonly string MaterializedAccountPath =
        Path.Combine(AppDirectory, "active-codex-account.txt");

    public static void Initialize()
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(AccountsDirectory);
        Directory.CreateDirectory(IdeProfilesDirectory);
        Directory.CreateDirectory(CommandsDirectory);
        Directory.CreateDirectory(LogsDirectory);

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            if (settings.Accounts.Count == 0)
            {
                settings.Accounts.Add(CreateDefaultAccount());
            }

            if (string.IsNullOrWhiteSpace(settings.ActiveAccountId) ||
                settings.Accounts.All(account => account.Id != settings.ActiveAccountId))
            {
                settings.ActiveAccountId = settings.Accounts[0].Id;
            }

            WriteSettingsUnlocked(settings);
        }
    }

    public static AccountSnapshot GetActiveAccount()
    {
        Initialize();

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            var account = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, settings.ActiveAccountId, StringComparison.OrdinalIgnoreCase)) ??
                          settings.Accounts.FirstOrDefault() ??
                          CreateDefaultAccount();
            var codexHome = ExpandPath(account.CodexHome);
            if (string.Equals(ReadMaterializedAccountId(), account.Id,
                    StringComparison.OrdinalIgnoreCase))
            {
                codexHome = RealCodexHome;
            }

            return new AccountSnapshot(
                account.Id,
                account.Label,
                codexHome);
        }
    }

    public static async Task RunCommandWatcherAsync(CancellationToken cancellationToken)
    {
        Initialize();

        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                foreach (var commandPath in Directory.EnumerateFiles(CommandsDirectory, "*.json")
                             .OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    ProcessCommandFile(commandPath);
                }
            }
            catch (Exception ex)
            {
                Log($"Account command watcher error: {ex.Message}");
            }

            await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
        }
    }

    private static void ProcessCommandFile(string path)
    {
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8));
            var command = node?["command"]?.GetValue<string>();
            if (string.Equals(command, "addAccount", StringComparison.OrdinalIgnoreCase))
            {
                AddAccountAndStartLogin();
            }
            else if (string.Equals(command, "switchAccount", StringComparison.OrdinalIgnoreCase))
            {
                var accountId = node?["accountId"]?.GetValue<string>();
                if (!string.IsNullOrWhiteSpace(accountId))
                {
                    SwitchAccount(accountId);
                }
            }
            else if (string.Equals(command, "restartIde", StringComparison.OrdinalIgnoreCase))
            {
                RestartAntigravityWithActiveAccount();
            }
            else
            {
                Log($"Unknown account command ignored: {command}");
            }
        }
        catch (Exception ex)
        {
            Log($"Failed to process account command {Path.GetFileName(path)}: {ex.Message}");
        }
        finally
        {
            try
            {
                File.Delete(path);
            }
            catch
            {
                // Best effort. A stale command can be retried or overwritten later.
            }
        }
    }

    private static void AddAccountAndStartLogin()
    {
        AccountSettings.Account account;
        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            var number = settings.Accounts.Count(account => account.Id.StartsWith("account", StringComparison.OrdinalIgnoreCase)) + 1;
            string id;
            do
            {
                id = $"account{number++}";
            }
            while (settings.Accounts.Any(item => string.Equals(item.Id, id, StringComparison.OrdinalIgnoreCase)));

            account = new AccountSettings.Account
            {
                Id = id,
                Label = $"Account {settings.Accounts.Count + 1}",
                CodexHome = Path.Combine(AccountsDirectory, id, "codex")
            };
            Directory.CreateDirectory(account.CodexHome);
            settings.Accounts.Add(account);
            settings.ActiveAccountId = account.Id;
            WriteSettingsUnlocked(settings);
        }

        StartCodexLogin(account);
        Log($"Added Codex account profile: {account.Id}");
    }

    private static void SwitchAccount(string accountId)
    {
        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            if (settings.Accounts.All(account =>
                    !string.Equals(account.Id, accountId, StringComparison.OrdinalIgnoreCase)))
            {
                Log($"Switch account ignored, unknown id: {accountId}");
                return;
            }

            settings.ActiveAccountId = accountId;
            WriteSettingsUnlocked(settings);
        }

        Log($"Active Codex account switched: {accountId}");
    }

    private static void StartCodexLogin(AccountSettings.Account account)
    {
        var codexHome = ExpandPath(account.CodexHome);
        Directory.CreateDirectory(codexHome);

        var escapedHome = codexHome.Replace("'", "''");
        var info = new ProcessStartInfo
        {
            FileName = "powershell.exe",
            Arguments = $"-NoExit -ExecutionPolicy Bypass -Command \"$env:CODEX_HOME='{escapedHome}'; $env:CODEX_SQLITE_HOME='{escapedHome}'; codex login\"",
            UseShellExecute = true,
            WindowStyle = ProcessWindowStyle.Normal
        };

        try
        {
            Process.Start(info);
        }
        catch (Exception ex)
        {
            Log($"Failed to start codex login for {account.Id}: {ex.Message}");
        }
    }

    private static void RestartAntigravityWithActiveAccount()
    {
        var activeAccount = GetActiveAccount();
        var idePath = FindAntigravityExecutable();
        if (string.IsNullOrWhiteSpace(idePath) || !File.Exists(idePath))
        {
            Log("Restart IDE skipped: Antigravity executable was not found");
            return;
        }

        var mainWindows = GetAntigravityProcesses(idePath)
            .Where(process => process.MainWindowHandle != IntPtr.Zero)
            .ToArray();

        if (mainWindows.Length > 0)
        {
            foreach (var process in mainWindows)
            {
                try
                {
                    Log($"Requesting Antigravity close: pid={process.Id}");
                    process.CloseMainWindow();
                }
                catch (Exception ex)
                {
                    Log($"Failed to request Antigravity close for pid={process.Id}: {ex.Message}");
                }
            }

            DisposeProcesses(mainWindows);

            if (!WaitForAntigravityMainWindowsToClose(idePath, TimeSpan.FromSeconds(45)))
            {
                Log("Restart IDE aborted: Antigravity main window did not close cleanly");
                return;
            }
        }
        else
        {
            DisposeProcesses(mainWindows);
        }

        TerminateRemainingAntigravityHelpers(idePath);

        if (!MaterializeCodexAccount(activeAccount))
        {
            Log($"Restart IDE aborted: failed to materialize Codex account {activeAccount.Id}");
            return;
        }

        StartAntigravity(idePath, activeAccount);
    }

    private static bool WaitForAntigravityMainWindowsToClose(string idePath, TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow.Add(timeout);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var mainWindows = GetAntigravityProcesses(idePath)
                .Where(process => process.MainWindowHandle != IntPtr.Zero)
                .ToArray();
            if (mainWindows.Length == 0)
            {
                return true;
            }

            DisposeProcesses(mainWindows);
            Thread.Sleep(500);
        }

        var remainingMainWindows = GetAntigravityProcesses(idePath)
            .Where(process => process.MainWindowHandle != IntPtr.Zero)
            .ToArray();
        foreach (var process in remainingMainWindows)
        {
            try
            {
                Log($"Antigravity main window still open: pid={process.Id}");
            }
            catch
            {
                // Ignore logging races during shutdown.
            }
        }

        DisposeProcesses(remainingMainWindows);
        return false;
    }

    private static void TerminateRemainingAntigravityHelpers(string idePath)
    {
        var helpers = GetAntigravityProcesses(idePath)
            .Where(process => process.MainWindowHandle == IntPtr.Zero)
            .ToArray();
        if (helpers.Length == 0)
        {
            return;
        }

        foreach (var process in helpers)
        {
            try
            {
                if (process.HasExited)
                {
                    continue;
                }

                Log($"Terminating Antigravity helper before account restart: pid={process.Id}");
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // Process already exited.
            }
            catch (Exception ex)
            {
                Log($"Failed to terminate Antigravity helper pid={process.Id}: {ex.Message}");
            }
        }

        var deadline = DateTimeOffset.UtcNow.AddSeconds(10);
        foreach (var process in helpers)
        {
            try
            {
                var remaining = deadline - DateTimeOffset.UtcNow;
                if (remaining > TimeSpan.Zero && !process.HasExited)
                {
                    process.WaitForExit((int)Math.Min(remaining.TotalMilliseconds, int.MaxValue));
                }
            }
            catch
            {
                // Best effort. The next launch should still get a clean profile
                // if Electron's singleton helper has exited.
            }
        }

        DisposeProcesses(helpers);
    }

    private static void DisposeProcesses(IEnumerable<Process> processes)
    {
        foreach (var process in processes)
        {
            process.Dispose();
        }
    }

    private static IEnumerable<Process> GetAntigravityProcesses(string idePath)
    {
        foreach (var process in Process.GetProcessesByName("Antigravity IDE"))
        {
            string? path = null;
            try
            {
                path = process.MainModule?.FileName;
            }
            catch
            {
                // Ignore inaccessible helper processes.
            }

            if (string.Equals(path, idePath, StringComparison.OrdinalIgnoreCase))
            {
                yield return process;
            }
            else
            {
                process.Dispose();
            }
        }
    }

    private static string? FindAntigravityExecutable()
    {
        foreach (var process in Process.GetProcessesByName("Antigravity IDE"))
        {
            using (process)
            {
                try
                {
                    var path = process.MainModule?.FileName;
                    if (!string.IsNullOrWhiteSpace(path) && File.Exists(path))
                    {
                        return path;
                    }
                }
                catch
                {
                    // Try the default install location below.
                }
            }
        }

        var defaultPath = Path.Combine(
            LocalAppData,
            "Programs",
            "Antigravity IDE",
            "Antigravity IDE.exe");
        return File.Exists(defaultPath) ? defaultPath : null;
    }

    private static void StartAntigravity(string idePath, AccountSnapshot account)
    {
        Directory.CreateDirectory(RealCodexHome);

        var info = new ProcessStartInfo
        {
            FileName = idePath,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(idePath) ?? AppDirectory
        };
        info.Environment["CODEX_HOME"] = RealCodexHome;
        info.Environment["CODEX_SQLITE_HOME"] = RealCodexHome;

        try
        {
            Process.Start(info);
            Log($"Started Antigravity with Codex account {account.Id}: codexHome={RealCodexHome}");
        }
        catch (Exception ex)
        {
            Log($"Failed to start Antigravity with account {account.Id}: {ex.Message}");
        }
    }

    private static bool MaterializeCodexAccount(AccountSnapshot targetAccount)
    {
        try
        {
            var targetStorage = GetStoredCodexHome(targetAccount);
            Directory.CreateDirectory(targetStorage);
            Directory.CreateDirectory(RealCodexHome);

            var currentAccountId = ReadMaterializedAccountId();
            var currentAuth = Path.Combine(RealCodexHome, "auth.json");
            if (!string.IsNullOrWhiteSpace(currentAccountId))
            {
                var currentStorage = GetStoredCodexHome(currentAccountId);
                var currentStoredAuth = Path.Combine(currentStorage, "auth.json");
                if (File.Exists(currentAuth))
                {
                    Directory.CreateDirectory(currentStorage);
                    File.Copy(currentAuth, currentStoredAuth, overwrite: true);
                }
            }

            var targetAuth = Path.Combine(targetStorage, "auth.json");
            if (!File.Exists(targetAuth))
            {
                Log($"Codex account auth file is missing: {targetAccount.Id} ({targetAuth})");
                return false;
            }

            var tempAuth = $"{currentAuth}.tmp";
            File.Copy(targetAuth, tempAuth, overwrite: true);
            File.Move(tempAuth, currentAuth, overwrite: true);

            File.WriteAllText(MaterializedAccountPath, targetAccount.Id,
                Encoding.UTF8);
            Log($"Codex account auth materialized: {targetAccount.Id} -> {currentAuth}");
            return true;
        }
        catch (Exception ex)
        {
            Log($"Failed to materialize Codex account {targetAccount.Id}: {ex.Message}");
            return false;
        }
    }

    private static string ReadMaterializedAccountId()
    {
        try
        {
            if (File.Exists(MaterializedAccountPath))
            {
                var value = File.ReadAllText(MaterializedAccountPath, Encoding.UTF8)
                    .Trim();
                if (!string.IsNullOrWhiteSpace(value))
                {
                    return value;
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Failed to read materialized Codex account marker: {ex.Message}");
        }

        return DefaultAccountId;
    }

    private static string GetStoredCodexHome(AccountSnapshot account) =>
        GetStoredCodexHome(account.Id, account.CodexHome);

    private static string GetStoredCodexHome(string accountId, string? codexHome = null)
    {
        if (string.Equals(accountId, DefaultAccountId,
                StringComparison.OrdinalIgnoreCase))
        {
            return Path.Combine(AccountsDirectory, DefaultAccountId, "codex");
        }

        return ExpandPath(codexHome ??
                          Path.Combine(AccountsDirectory,
                              SanitizePathSegment(accountId), "codex"));
    }

    private static string GetIdeProfilePath(AccountSnapshot account)
    {
        if (string.Equals(account.Id, DefaultAccountId, StringComparison.OrdinalIgnoreCase))
        {
            var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
            return Path.Combine(appData, "Antigravity IDE");
        }

        return Path.Combine(IdeProfilesDirectory, SanitizePathSegment(account.Id));
    }

    private static string SanitizePathSegment(string value)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var chars = value.Select(ch => invalid.Contains(ch) ? '_' : ch).ToArray();
        var sanitized = new string(chars).Trim();
        return string.IsNullOrWhiteSpace(sanitized) ? "account" : sanitized;
    }

    private static AccountSettings ReadSettingsUnlocked()
    {
        if (!File.Exists(SettingsPath))
        {
            return new AccountSettings
            {
                ActiveAccountId = DefaultAccountId,
                Accounts = [CreateDefaultAccount()]
            };
        }

        try
        {
            var settings = JsonSerializer.Deserialize<AccountSettings>(
                File.ReadAllText(SettingsPath, Encoding.UTF8),
                CreateJsonOptions());
            return settings ?? new AccountSettings();
        }
        catch (Exception ex)
        {
            Log($"Settings file was reset because it could not be parsed: {ex.Message}");
            return new AccountSettings
            {
                ActiveAccountId = DefaultAccountId,
                Accounts = [CreateDefaultAccount()]
            };
        }
    }

    private static void WriteSettingsUnlocked(AccountSettings settings)
    {
        var temp = $"{SettingsPath}.tmp";
        var json = JsonSerializer.Serialize(settings, CreateJsonOptions());
        File.WriteAllText(temp, json, Encoding.UTF8);
        File.Move(temp, SettingsPath, overwrite: true);
    }

    private static AccountSettings.Account CreateDefaultAccount() => new()
    {
        Id = DefaultAccountId,
        Label = "Default",
        CodexHome = Path.Combine(UserProfile, ".codex")
    };

    private static string ExpandPath(string path) =>
        Environment.ExpandEnvironmentVariables(path);

    private static JsonSerializerOptions CreateJsonOptions() => new()
    {
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
            // Logging must never break account handling.
        }
    }

    private sealed class AccountSettings
    {
        public string ActiveAccountId { get; set; } = DefaultAccountId;
        public List<Account> Accounts { get; set; } = [];

        public sealed class Account
        {
            public string Id { get; set; } = "";
            public string Label { get; set; } = "";
            public string CodexHome { get; set; } = "";
        }
    }
}

internal sealed record AccountSnapshot(string Id, string Label, string CodexHome);
