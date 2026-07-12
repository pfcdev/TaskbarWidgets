using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace TaskbarStatsProduct;

internal static class AccountManager
{
    private const string DefaultAccountId = "default";
    private static readonly string[] AntigravityProcessNames =
    [
        "Antigravity",
        "Antigravity IDE"
    ];
    private static readonly object SyncRoot = new();
    private static long s_changeVersion;
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

            RefreshAccountEmailsUnlocked(settings);
            WriteSettingsUnlocked(settings);
        }

        SanitizeCodexConfig(RealCodexHome);
    }

    public static long GetChangeVersion() => Interlocked.Read(ref s_changeVersion);

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
            var codexHome = ResolveCodexHome(account, ReadMaterializedAccountId());

            return new AccountSnapshot(
                account.Id,
                account.Label,
                codexHome);
        }
    }

    public static IReadOnlyList<AccountSnapshot> GetAccounts()
    {
        Initialize();

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            var materializedAccountId = ReadMaterializedAccountId();
            return settings.Accounts
                .Select(account =>
                {
                    return new AccountSnapshot(
                        account.Id,
                        account.Label,
                        ResolveCodexHome(account, materializedAccountId));
                })
                .ToArray();
        }
    }

    public static void SetAccountRateLimitText(string accountId, string rateLimitText)
    {
        Directory.CreateDirectory(AppDirectory);

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            var account = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, accountId, StringComparison.OrdinalIgnoreCase));
            if (account is null)
            {
                return;
            }

            rateLimitText = rateLimitText.Trim();
            if (string.Equals(account.RateLimitText, rateLimitText,
                    StringComparison.Ordinal))
            {
                return;
            }

            account.RateLimitText = rateLimitText;
            WriteSettingsUnlocked(settings);
        }
    }

    public static async Task RunCommandWatcherAsync(CancellationToken cancellationToken)
    {
        Initialize();

        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                RefreshAccountEmails();
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
            else if (string.Equals(command, "loginActiveAccount", StringComparison.OrdinalIgnoreCase))
            {
                LoginActiveAccount();
            }
            else if (string.Equals(command, "deleteActiveAccount", StringComparison.OrdinalIgnoreCase))
            {
                DeleteActiveAccount();
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

        NotifyChanged();
        StartCodexLogin(account);
        Log($"Added Codex account profile: {account.Id}");
    }

    private static void DeleteActiveAccount()
    {
        AccountSettings.Account? accountToDelete = null;
        AccountSettings.Account? fallbackAccount = null;

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            accountToDelete = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, settings.ActiveAccountId,
                    StringComparison.OrdinalIgnoreCase));

            if (accountToDelete is null)
            {
                Log("Delete active Codex account ignored: no active account found");
                return;
            }

            if (string.Equals(accountToDelete.Id, DefaultAccountId,
                    StringComparison.OrdinalIgnoreCase))
            {
                Log("Delete active Codex account ignored: default account cannot be deleted");
                return;
            }

            fallbackAccount = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, DefaultAccountId,
                    StringComparison.OrdinalIgnoreCase)) ?? CreateDefaultAccount();

            settings.Accounts.RemoveAll(item =>
                string.Equals(item.Id, accountToDelete.Id,
                    StringComparison.OrdinalIgnoreCase));
            if (settings.Accounts.Count == 0)
            {
                settings.Accounts.Add(fallbackAccount);
            }

            settings.ActiveAccountId = fallbackAccount.Id;
            RefreshAccountEmailsUnlocked(settings);
            WriteSettingsUnlocked(settings);
        }

        if (string.Equals(ReadMaterializedAccountId(), accountToDelete.Id,
                StringComparison.OrdinalIgnoreCase))
        {
            MaterializeCodexAccount(new AccountSnapshot(
                fallbackAccount.Id,
                fallbackAccount.Label,
                ExpandPath(fallbackAccount.CodexHome)));
        }

        DeleteDirectoryBestEffort(
            Path.Combine(AccountsDirectory, SanitizePathSegment(accountToDelete.Id)));
        DeleteDirectoryBestEffort(GetIdeProfilePath(new AccountSnapshot(
            accountToDelete.Id,
            accountToDelete.Label,
            ExpandPath(accountToDelete.CodexHome))));
        NotifyChanged();
        Log($"Deleted Codex account profile: {accountToDelete.Id}");
    }

    private static void LoginActiveAccount()
    {
        AccountSettings.Account? account;
        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            account = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, settings.ActiveAccountId,
                    StringComparison.OrdinalIgnoreCase));
        }

        if (account is null)
        {
            Log("Login active Codex account ignored: no active account found");
            return;
        }

        StartCodexLogin(account);
        Log($"Started login for active Codex account profile: {account.Id}");
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

        NotifyChanged();
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
            Arguments = $"-NoExit -ExecutionPolicy Bypass -Command \"$env:CODEX_HOME='{escapedHome}'; $env:CODEX_SQLITE_HOME='{escapedHome}'; Write-Host 'TaskbarStats Codex account login: choose the target account in the browser flow.'; codex login\"",
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

        if (!MaterializeCodexAccount(activeAccount))
        {
            Log($"Restart IDE aborted: failed to materialize Codex account {activeAccount.Id}");
            return;
        }

        TerminateCodexAppServersForAccountSwitch();
        if (TryReloadRunningIdeWindow(idePath, activeAccount))
        {
            return;
        }

        var mainWindows = GetAntigravityProcesses()
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

            if (!WaitForAntigravityMainWindowsToClose(TimeSpan.FromSeconds(45)))
            {
                Log("Antigravity main window did not close cleanly; terminating remaining processes before relaunch");
                TerminateAllAntigravityProcesses("main-window-timeout");
            }
        }
        else
        {
            DisposeProcesses(mainWindows);
        }

        TerminateRemainingAntigravityHelpers();
        TerminateCodexAppServersForAccountSwitch();

        Thread.Sleep(1500);
        StartAntigravity(idePath, activeAccount);
    }

    private static bool TryReloadRunningIdeWindow(string idePath, AccountSnapshot account)
    {
        if (!GetAntigravityProcesses().Any(process =>
            {
                using (process)
                {
                    try
                    {
                        return !process.HasExited && process.MainWindowHandle != IntPtr.Zero;
                    }
                    catch
                    {
                        return false;
                    }
                }
            }))
        {
            return false;
        }

        var ideProfile = GetIdeProfilePath(account);
        if (LaunchIdeCommand(idePath, account, ideProfile,
                "workbench.action.reloadWindow", "reload-window"))
        {
            Log($"Requested Antigravity/VS Code window reload for Codex account {account.Id}");
            Thread.Sleep(2500);
            return true;
        }

        return false;
    }

    private static bool WaitForAntigravityMainWindowsToClose(TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow.Add(timeout);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var mainWindows = GetAntigravityProcesses()
                .Where(process => process.MainWindowHandle != IntPtr.Zero)
                .ToArray();
            if (mainWindows.Length == 0)
            {
                return true;
            }

            DisposeProcesses(mainWindows);
            Thread.Sleep(500);
        }

        var remainingMainWindows = GetAntigravityProcesses()
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

    private static void TerminateRemainingAntigravityHelpers()
    {
        var helpers = GetAntigravityProcesses()
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

    private static void TerminateAllAntigravityProcesses(string reason)
    {
        var processes = GetAntigravityProcesses().ToArray();
        if (processes.Length == 0)
        {
            return;
        }

        foreach (var process in processes)
        {
            try
            {
                if (process.HasExited)
                {
                    continue;
                }

                Log($"Terminating Antigravity process before relaunch ({reason}): pid={process.Id}");
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // Process already exited.
            }
            catch (Exception ex)
            {
                Log($"Failed to terminate Antigravity process pid={process.Id}: {ex.Message}");
            }
        }

        var deadline = DateTimeOffset.UtcNow.AddSeconds(15);
        foreach (var process in processes)
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
                // Best effort. The direct launch fallback below is still attempted.
            }
        }

        DisposeProcesses(processes);
    }

    private static void TerminateCodexAppServersForAccountSwitch()
    {
        var processes = Process.GetProcessesByName("codex");
        if (processes.Length == 0)
        {
            return;
        }

        foreach (var process in processes)
        {
            try
            {
                if (process.HasExited)
                {
                    continue;
                }

                var path = TryGetProcessPath(process);
                if (string.IsNullOrWhiteSpace(path) ||
                    !IsAccountScopedCodexAppServer(path))
                {
                    continue;
                }

                Log($"Terminating Codex app-server before account restart: pid={process.Id}; path={path}");
                process.Kill(entireProcessTree: true);
            }
            catch (InvalidOperationException)
            {
                // Process already exited.
            }
            catch (Exception ex)
            {
                Log($"Failed to terminate Codex process pid={process.Id}: {ex.Message}");
            }
        }

        var deadline = DateTimeOffset.UtcNow.AddSeconds(10);
        foreach (var process in processes)
        {
            try
            {
                var path = TryGetProcessPath(process);
                if (string.IsNullOrWhiteSpace(path) ||
                    !IsAccountScopedCodexAppServer(path))
                {
                    continue;
                }

                var remaining = deadline - DateTimeOffset.UtcNow;
                if (remaining > TimeSpan.Zero && !process.HasExited)
                {
                    process.WaitForExit((int)Math.Min(remaining.TotalMilliseconds, int.MaxValue));
                }
            }
            catch
            {
                // Best effort. A newly launched IDE will start a fresh Codex
                // app-server against the materialized account.
            }
        }

        DisposeProcesses(processes);
    }

    private static string? TryGetProcessPath(Process process)
    {
        try
        {
            return process.MainModule?.FileName;
        }
        catch
        {
            return null;
        }
    }

    private static bool IsAccountScopedCodexAppServer(string path)
    {
        var normalized = path.Replace('/', '\\');
        return normalized.Contains("\\.antigravity-ide\\extensions\\openai.chatgpt-",
                   StringComparison.OrdinalIgnoreCase) ||
               normalized.Contains("\\npm\\node_modules\\@openai\\codex\\",
                   StringComparison.OrdinalIgnoreCase);
    }

    private static void DisposeProcesses(IEnumerable<Process> processes)
    {
        foreach (var process in processes)
        {
            process.Dispose();
        }
    }

    private static IEnumerable<Process> GetAntigravityProcesses()
    {
        foreach (var processName in AntigravityProcessNames)
        {
            foreach (var process in Process.GetProcessesByName(processName))
            {
                yield return process;
            }
        }
    }

    private static string? FindAntigravityExecutable()
    {
        foreach (var path in GetKnownAntigravityExecutablePaths())
        {
            if (File.Exists(path))
            {
                return path;
            }
        }

        foreach (var process in GetAntigravityProcesses())
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

        return null;
    }

    private static void StartAntigravity(string idePath, AccountSnapshot account)
    {
        var ideProfile = GetIdeProfilePath(account);
        Directory.CreateDirectory(RealCodexHome);
        Directory.CreateDirectory(ideProfile);

        if (LaunchAntigravityWithProfile(idePath, account, ideProfile, "primary") &&
            WaitForAntigravityMainWindowToOpen(idePath, TimeSpan.FromSeconds(20)))
        {
            return;
        }

        Log($"Antigravity did not show a main window after primary launch for account {account.Id}; retrying");
        Thread.Sleep(2500);
        if (LaunchAntigravityFromShell(idePath, account, ideProfile, "retry") &&
            WaitForAntigravityMainWindowToOpen(idePath, TimeSpan.FromSeconds(20)))
        {
            return;
        }

        Log($"Antigravity launch did not produce a visible main window for account {account.Id}");
    }

    private static bool LaunchAntigravityWithProfile(string idePath,
                                                     AccountSnapshot account,
                                                     string ideProfile,
                                                     string attempt)
    {
        var info = new ProcessStartInfo
        {
            FileName = idePath,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(idePath) ?? AppDirectory,
            WindowStyle = ProcessWindowStyle.Normal
        };
        info.ArgumentList.Add("--user-data-dir");
        info.ArgumentList.Add(ideProfile);
        info.Environment["CODEX_HOME"] = RealCodexHome;
        info.Environment["CODEX_SQLITE_HOME"] = RealCodexHome;

        try
        {
            var process = Process.Start(info);
            Log($"Started Antigravity ({attempt}) with Codex account {account.Id}: pid={process?.Id.ToString() ?? "unknown"}; codexHome={RealCodexHome}; ideProfile={ideProfile}");
            return process != null;
        }
        catch (Exception ex)
        {
            Log($"Failed to start Antigravity ({attempt}) with profile for account {account.Id}: {ex.Message}");
            return false;
        }
    }

    private static bool LaunchAntigravityFromShell(string idePath,
                                                   AccountSnapshot account,
                                                   string ideProfile,
                                                   string attempt)
    {
        var info = new ProcessStartInfo
        {
            FileName = idePath,
            Arguments = $"--user-data-dir \"{ideProfile}\"",
            UseShellExecute = true,
            WorkingDirectory = Path.GetDirectoryName(idePath) ?? AppDirectory,
            WindowStyle = ProcessWindowStyle.Normal
        };

        try
        {
            var process = Process.Start(info);
            Log($"Started Antigravity ({attempt}) with Codex account {account.Id}: pid={process?.Id.ToString() ?? "unknown"}; codexHome={RealCodexHome}; ideProfile={ideProfile}");
            return process != null;
        }
        catch (Exception ex)
        {
            Log($"Failed to start Antigravity ({attempt}) with account {account.Id}: {ex.Message}");
            return false;
        }
    }

    private static bool LaunchIdeCommand(string idePath,
                                         AccountSnapshot account,
                                         string ideProfile,
                                         string command,
                                         string attempt)
    {
        var info = new ProcessStartInfo
        {
            FileName = idePath,
            UseShellExecute = false,
            WorkingDirectory = Path.GetDirectoryName(idePath) ?? AppDirectory,
            WindowStyle = ProcessWindowStyle.Hidden,
            CreateNoWindow = true
        };
        info.ArgumentList.Add("--user-data-dir");
        info.ArgumentList.Add(ideProfile);
        info.ArgumentList.Add("--command");
        info.ArgumentList.Add(command);
        info.Environment["CODEX_HOME"] = RealCodexHome;
        info.Environment["CODEX_SQLITE_HOME"] = RealCodexHome;

        try
        {
            var process = Process.Start(info);
            Log($"Started Antigravity command ({attempt}) for Codex account {account.Id}: pid={process?.Id.ToString() ?? "unknown"}; command={command}; ideProfile={ideProfile}");
            return process != null;
        }
        catch (Exception ex)
        {
            Log($"Failed to start Antigravity command ({attempt}) for account {account.Id}: {ex.Message}");
            return false;
        }
    }

    private static bool WaitForAntigravityMainWindowToOpen(string idePath,
                                                           TimeSpan timeout)
    {
        var deadline = DateTimeOffset.UtcNow.Add(timeout);
        while (DateTimeOffset.UtcNow < deadline)
        {
            var mainWindows = GetAntigravityProcesses()
                .Where(process => process.MainWindowHandle != IntPtr.Zero)
                .ToArray();
            if (mainWindows.Length > 0)
            {
                foreach (var process in mainWindows)
                {
                    Log($"Antigravity main window opened: pid={process.Id}");
                }

                DisposeProcesses(mainWindows);
                return true;
            }

            DisposeProcesses(mainWindows);
            Thread.Sleep(500);
        }

        return false;
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
            SanitizeCodexConfig(RealCodexHome);

            File.WriteAllText(MaterializedAccountPath, targetAccount.Id,
                Encoding.UTF8);
            RefreshAccountEmails();
            NotifyChanged();
            Log($"Codex account auth materialized: {targetAccount.Id} -> {currentAuth}");
            return true;
        }
        catch (Exception ex)
        {
            Log($"Failed to materialize Codex account {targetAccount.Id}: {ex.Message}");
            return false;
        }
    }

    private static void SanitizeCodexConfig(string codexHome)
    {
        try
        {
            var configPath = Path.Combine(codexHome, "config.toml");
            if (!File.Exists(configPath))
            {
                return;
            }

            var lines = File.ReadAllLines(configPath, Encoding.UTF8);
            var filtered = lines
                .Where(line => !line.TrimStart().StartsWith("skills =",
                    StringComparison.OrdinalIgnoreCase))
                .ToArray();
            if (filtered.Length == lines.Length)
            {
                return;
            }

            var backupPath = $"{configPath}.bak-taskbarstats";
            File.Copy(configPath, backupPath, overwrite: true);
            File.WriteAllLines(configPath, filtered, Encoding.UTF8);
            Log($"Removed unsupported Codex config feature from {configPath}");
        }
        catch (Exception ex)
        {
            Log($"Failed to sanitize Codex config in {codexHome}: {ex.Message}");
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

    private static string ResolveCodexHome(
        AccountSettings.Account account,
        string materializedAccountId)
    {
        if (string.Equals(materializedAccountId, account.Id,
                StringComparison.OrdinalIgnoreCase))
        {
            return RealCodexHome;
        }

        var storedCodexHome = GetStoredCodexHome(account.Id, account.CodexHome);
        if (Directory.Exists(storedCodexHome) ||
            File.Exists(Path.Combine(storedCodexHome, "auth.json")))
        {
            return storedCodexHome;
        }

        return ExpandPath(account.CodexHome);
    }

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

    private static IEnumerable<string> GetKnownAntigravityExecutablePaths()
    {
        yield return Path.Combine(LocalAppData, "Programs", "Antigravity",
            "Antigravity.exe");
        yield return Path.Combine(LocalAppData, "Programs", "Antigravity IDE",
            "Antigravity IDE.exe");
    }

    private static void RefreshAccountEmails()
    {
        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            if (RefreshAccountEmailsUnlocked(settings))
            {
                WriteSettingsUnlocked(settings);
            }
        }
    }

    private static bool RefreshAccountEmailsUnlocked(AccountSettings settings)
    {
        var changed = false;
        foreach (var account in settings.Accounts)
        {
            var email = ReadAccountEmail(account) ?? "";
            if (!string.Equals(account.Email, email, StringComparison.Ordinal))
            {
                account.Email = email;
                changed = true;
            }
        }

        return changed;
    }

    private static string? ReadAccountEmail(AccountSettings.Account account)
    {
        var materializedAccountId = ReadMaterializedAccountId();
        if (string.Equals(materializedAccountId, account.Id,
                StringComparison.OrdinalIgnoreCase))
        {
            var materializedEmail = ReadCodexAuthEmail(
                Path.Combine(RealCodexHome, "auth.json"));
            if (!string.IsNullOrWhiteSpace(materializedEmail))
            {
                return materializedEmail;
            }
        }

        var storedEmail = ReadCodexAuthEmail(
            Path.Combine(GetStoredCodexHome(account.Id, account.CodexHome),
                "auth.json"));
        if (!string.IsNullOrWhiteSpace(storedEmail))
        {
            return storedEmail;
        }

        if (string.Equals(account.Id, DefaultAccountId,
                StringComparison.OrdinalIgnoreCase))
        {
            return ReadCodexAuthEmail(Path.Combine(RealCodexHome, "auth.json"));
        }

        return null;
    }

    private static string? ReadCodexAuthEmail(string authPath)
    {
        try
        {
            if (!File.Exists(authPath))
            {
                return null;
            }

            var node = JsonNode.Parse(File.ReadAllText(authPath, Encoding.UTF8));
            var idToken = node?["tokens"]?["id_token"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(idToken))
            {
                return null;
            }

            var parts = idToken.Split('.');
            if (parts.Length < 2)
            {
                return null;
            }

            var payload = Encoding.UTF8.GetString(Base64UrlDecode(parts[1]));
            var payloadNode = JsonNode.Parse(payload);
            var email = payloadNode?["email"]?.GetValue<string>() ??
                        payloadNode?["preferred_username"]?.GetValue<string>();
            return string.IsNullOrWhiteSpace(email) ? null : email.Trim();
        }
        catch
        {
            return null;
        }
    }

    private static byte[] Base64UrlDecode(string value)
    {
        var base64 = value.Replace('-', '+').Replace('_', '/');
        var padding = base64.Length % 4;
        if (padding > 0)
        {
            base64 = base64.PadRight(base64.Length + 4 - padding, '=');
        }

        return Convert.FromBase64String(base64);
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

    private static void DeleteDirectoryBestEffort(string path)
    {
        try
        {
            if (Directory.Exists(path))
            {
                Directory.Delete(path, recursive: true);
            }
        }
        catch (Exception ex)
        {
            Log($"Failed to delete directory {path}: {ex.Message}");
        }
    }

    private static void NotifyChanged()
    {
        Interlocked.Increment(ref s_changeVersion);
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
            public string Email { get; set; } = "";
            public string CodexHome { get; set; } = "";
            public string RateLimitText { get; set; } = "";
        }
    }
}

internal sealed record AccountSnapshot(string Id, string Label, string CodexHome);
