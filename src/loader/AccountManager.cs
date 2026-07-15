using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

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
    private static readonly string AppDirectory = AppPaths.DataDirectory;
    private static readonly string AccountsDirectory = Path.Combine(AppDirectory, "Accounts");
    private static readonly string IdeProfilesDirectory = Path.Combine(AppDirectory, "IdeProfiles");
    private static readonly string WidgetLibrariesDirectory = Path.Combine(AppDirectory, "Legacy", "WidgetLibraries");
    private static readonly string CommandsDirectory = AppPaths.CommandsDirectory;
    private static readonly string LogsDirectory = AppPaths.LogsDirectory;
    private static readonly string SettingsPath = Path.Combine(AppDirectory, "settings.json");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");
    private static readonly string SettingsAppPath = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.Settings.exe");
    private static readonly string LoaderPath = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.exe");
    private static readonly string RealCodexHome = Path.Combine(UserProfile, ".codex");
    private static readonly string MaterializedAccountPath =
        Path.Combine(AppDirectory, "active-codex-account.txt");
    private const int SwRestore = 9;
    private const int SwShow = 5;

    public static void Initialize()
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(AccountsDirectory);
        Directory.CreateDirectory(IdeProfilesDirectory);
        Directory.CreateDirectory(WidgetLibrariesDirectory);
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

        EnsureReloadKeybindingsForKnownProfiles();
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

            await Task.Delay(TimeSpan.FromMilliseconds(100), cancellationToken);
        }
    }

    private static void ProcessCommandFile(string path)
    {
        try
        {
            var node = JsonNode.Parse(File.ReadAllText(path, Encoding.UTF8));
            var schemaVersion = node?["schemaVersion"]?.GetValue<int?>() ?? 0;
            var commandId = node?["commandId"]?.GetValue<string>() ?? "";
            var createdAtUnix = node?["createdAtUnix"]?.GetValue<long?>() ?? 0;
            var command = node?["action"]?.GetValue<string>() ?? "";
            var envelope = new WidgetCommand(
                schemaVersion,
                commandId,
                node?["widgetId"]?.GetValue<string>(),
                command,
                null,
                createdAtUnix);
            if (!WidgetCommandValidator.IsValid(
                    envelope, DateTimeOffset.UtcNow.ToUnixTimeSeconds()))
            {
                Log($"Invalid or stale command ignored: {Path.GetFileName(path)}");
                return;
            }
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
            else if (string.Equals(command, "deleteAccount", StringComparison.OrdinalIgnoreCase))
            {
                var accountId = node?["accountId"]?.GetValue<string>();
                DeleteActiveAccount(accountId);
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
            else if (string.Equals(command, "openSettings", StringComparison.OrdinalIgnoreCase))
            {
                OpenSettingsApp();
            }
            else if (string.Equals(command, "mediaToggle", StringComparison.OrdinalIgnoreCase))
            {
                MediaWorker.RequestToggle();
            }
            else if (string.Equals(command, "quit", StringComparison.OrdinalIgnoreCase))
            {
                QuitTaskbarWidgets();
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

    private static void OpenWidgetLibrariesFolder()
    {
        Directory.CreateDirectory(WidgetLibrariesDirectory);
        var readmePath = Path.Combine(WidgetLibrariesDirectory, "README.txt");
        if (!File.Exists(readmePath))
        {
            File.WriteAllText(
                readmePath,
                "TaskbarWidgets widget design packs will live in this folder." +
                Environment.NewLine +
                "The current build includes built-in Codex Status and Static Weather designs." +
                Environment.NewLine,
                Encoding.UTF8);
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = WidgetLibrariesDirectory,
            UseShellExecute = true
        });
        Log($"Opened widget libraries folder: {WidgetLibrariesDirectory}");
    }

    private static void OpenSettingsApp()
    {
        if (!File.Exists(SettingsAppPath))
        {
            Log($"Settings app was not found: {SettingsAppPath}");
            return;
        }

        using Process? runningSettings = FindRunningSettingsApp();
        if (runningSettings is not null)
        {
            Log($"Settings app is already running: {SettingsAppPath}");
            BringSettingsProcessToFront(runningSettings);
            return;
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = SettingsAppPath,
            WorkingDirectory = AppDirectory,
            UseShellExecute = true
        });
        Log($"Opened settings app: {SettingsAppPath}");
    }

    private static Process? FindRunningSettingsApp()
    {
        string expectedPath = Path.GetFullPath(SettingsAppPath);
        foreach (Process process in Process.GetProcessesByName("TaskbarWidgets.Settings"))
        {
            try
            {
                string? path = process.MainModule?.FileName;
                if (!string.IsNullOrWhiteSpace(path) &&
                    string.Equals(Path.GetFullPath(path), expectedPath,
                        StringComparison.OrdinalIgnoreCase))
                {
                    return process;
                }
            }
            catch
            {
                // Some processes can deny module path access. Ignore them.
            }

            process.Dispose();
        }

        return null;
    }

    private static void BringSettingsProcessToFront(Process process)
    {
        try
        {
            process.Refresh();
            IntPtr handle = process.MainWindowHandle;
            if (handle == IntPtr.Zero)
            {
                process.WaitForInputIdle(1000);
                process.Refresh();
                handle = process.MainWindowHandle;
            }

            if (handle == IntPtr.Zero)
            {
                Log($"Settings app is running but no main window handle was found: pid={process.Id}");
                return;
            }

            ShowWindow(handle, IsIconic(handle) ? SwRestore : SwShow);
            SetForegroundWindow(handle);
            Log($"Brought settings app to foreground: pid={process.Id}");
        }
        catch (Exception ex)
        {
            Log($"Failed to bring settings app to foreground: {ex.Message}");
        }
    }

    private static void QuitTaskbarWidgets()
    {
        var exe = File.Exists(LoaderPath)
            ? LoaderPath
            : Environment.ProcessPath ?? Process.GetCurrentProcess().MainModule?.FileName;
        if (string.IsNullOrWhiteSpace(exe))
        {
            Log("Quit command ignored: loader path could not be resolved");
            return;
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = exe,
            WorkingDirectory = AppDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            ArgumentList = { "--detach" }
        });
        Log("Quit command requested loader detach");
    }

    private static void DeleteActiveAccount(string? requestedAccountId = null)
    {
        AccountSettings.Account? accountToDelete = null;
        AccountSettings.Account? fallbackAccount = null;

        lock (SyncRoot)
        {
            var settings = ReadSettingsUnlocked();
            var targetAccountId = string.IsNullOrWhiteSpace(requestedAccountId)
                ? settings.ActiveAccountId
                : requestedAccountId;
            accountToDelete = settings.Accounts.FirstOrDefault(item =>
                string.Equals(item.Id, targetAccountId,
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
            else if (settings.Accounts.All(item =>
                         !string.Equals(item.Id, fallbackAccount.Id,
                             StringComparison.OrdinalIgnoreCase)))
            {
                settings.Accounts.Insert(0, fallbackAccount);
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
            Arguments = $"-NoExit -ExecutionPolicy Bypass -Command \"$env:CODEX_HOME='{escapedHome}'; $env:CODEX_SQLITE_HOME='{escapedHome}'; Write-Host 'TaskbarWidgets Codex account login: choose the target account in the browser flow.'; codex login\"",
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

        if (TryReloadRunningIdeWindow(activeAccount))
        {
            return;
        }

        Log($"Restart IDE skipped: no running Antigravity window to reload for account {activeAccount.Id}");
    }

    private static bool TryReloadRunningIdeWindow(AccountSnapshot account)
    {
        var windowProcess = GetAntigravityProcesses()
            .FirstOrDefault(process =>
            {
                try
                {
                    return !process.HasExited &&
                           process.MainWindowHandle != IntPtr.Zero &&
                           Regex.IsMatch(process.MainWindowTitle ?? "",
                               "Antigravity.*IDE|IDE.*Antigravity",
                               RegexOptions.IgnoreCase);
                }
                catch
                {
                    return false;
                }
            }) ??
            GetAntigravityProcesses()
                .FirstOrDefault(process =>
                {
                    try
                    {
                        return !process.HasExited &&
                               process.MainWindowHandle != IntPtr.Zero;
                    }
                    catch
                    {
                        return false;
                    }
                });

        if (windowProcess is null)
        {
            return false;
        }

        var ideProfile = GetIdeProfilePath(account);
        EnsureAntigravityReloadKeybinding(GetDefaultAntigravityProfilePath());
        EnsureAntigravityReloadKeybinding(ideProfile);

        try
        {
            var pid = windowProcess.Id;
            var title = windowProcess.MainWindowTitle;
            windowProcess.Dispose();

            if (!SendReloadShortcutToAntigravityWindow(pid, account))
            {
                return false;
            }

            Log($"Sent Antigravity reload shortcut for Codex account {account.Id}: pid={pid}; title={title}");
            Thread.Sleep(2500);
            return true;
        }
        catch (Exception ex)
        {
            Log($"Failed to send Antigravity reload shortcut for account {account.Id}: {ex.Message}");
            windowProcess.Dispose();
            return false;
        }
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
        EnsureAntigravityReloadKeybinding(ideProfile);

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
            return GetDefaultAntigravityProfilePath();
        }

        return Path.Combine(IdeProfilesDirectory, SanitizePathSegment(account.Id));
    }

    private static string GetDefaultAntigravityProfilePath()
    {
        var appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        return Path.Combine(appData, "Antigravity IDE");
    }

    private static void EnsureReloadKeybindingsForKnownProfiles()
    {
        EnsureAntigravityReloadKeybinding(GetDefaultAntigravityProfilePath());

        try
        {
            if (!Directory.Exists(IdeProfilesDirectory))
            {
                return;
            }

            foreach (var profile in Directory.EnumerateDirectories(IdeProfilesDirectory))
            {
                EnsureAntigravityReloadKeybinding(profile);
            }
        }
        catch (Exception ex)
        {
            Log($"Failed to update Antigravity reload keybindings for known profiles: {ex.Message}");
        }
    }

    private static void EnsureAntigravityReloadKeybinding(string ideProfile)
    {
        try
        {
            var userDirectory = Path.Combine(ideProfile, "User");
            Directory.CreateDirectory(userDirectory);

            var keybindingsPath = Path.Combine(userDirectory, "keybindings.json");
            const string key = "ctrl+alt+shift+r";
            const string command = "workbench.action.reloadWindow";
            var entry =
                $"    {{{Environment.NewLine}" +
                $"        \"key\": \"{key}\",{Environment.NewLine}" +
                $"        \"command\": \"{command}\"{Environment.NewLine}" +
                "    }";

            if (!File.Exists(keybindingsPath))
            {
                File.WriteAllText(keybindingsPath,
                    $"[{Environment.NewLine}{entry}{Environment.NewLine}]{Environment.NewLine}",
                    Encoding.UTF8);
                Log($"Created Antigravity reload keybinding: {keybindingsPath}");
                return;
            }

            var text = File.ReadAllText(keybindingsPath, Encoding.UTF8);
            var escapedKey = Regex.Escape(key);
            var escapedCommand = Regex.Escape(command);
            var existingKeyThenCommand =
                $"\\{{[^{{}}]*\"key\"\\s*:\\s*\"{escapedKey}\"[^{{}}]*\"command\"\\s*:\\s*\"{escapedCommand}\"[^{{}}]*\\}}";
            var existingCommandThenKey =
                $"\\{{[^{{}}]*\"command\"\\s*:\\s*\"{escapedCommand}\"[^{{}}]*\"key\"\\s*:\\s*\"{escapedKey}\"[^{{}}]*\\}}";
            if (Regex.IsMatch(text, existingKeyThenCommand,
                    RegexOptions.IgnoreCase | RegexOptions.Singleline) ||
                Regex.IsMatch(text, existingCommandThenKey,
                    RegexOptions.IgnoreCase | RegexOptions.Singleline))
            {
                return;
            }

            var openIndex = text.IndexOf('[');
            var closeIndex = text.LastIndexOf(']');
            if (openIndex < 0 || closeIndex < openIndex)
            {
                File.Copy(keybindingsPath, $"{keybindingsPath}.bak-taskbarstats",
                    overwrite: true);
                File.WriteAllText(keybindingsPath,
                    $"[{Environment.NewLine}{entry}{Environment.NewLine}]{Environment.NewLine}",
                    Encoding.UTF8);
                Log($"Reset malformed Antigravity keybindings file and added reload shortcut: {keybindingsPath}");
                return;
            }

            var inner = text.Substring(openIndex + 1, closeIndex - openIndex - 1);
            var innerWithoutComments = string.Join(Environment.NewLine,
                inner.Split(["\r\n", "\n"], StringSplitOptions.None)
                    .Where(line => !line.TrimStart().StartsWith("//",
                        StringComparison.Ordinal)));
            var comma = string.IsNullOrWhiteSpace(innerWithoutComments) ? "" : ",";
            var updated =
                text[..closeIndex].TrimEnd() +
                comma +
                Environment.NewLine +
                entry +
                Environment.NewLine +
                text[closeIndex..];
            File.WriteAllText(keybindingsPath, updated, Encoding.UTF8);
            Log($"Added Antigravity reload keybinding: {keybindingsPath}");
        }
        catch (Exception ex)
        {
            Log($"Failed to ensure Antigravity reload keybinding in {ideProfile}: {ex.Message}");
        }
    }

    private static bool SendReloadShortcutToAntigravityWindow(
        int processId,
        AccountSnapshot account)
    {
        var script = string.Join(Environment.NewLine, new[]
        {
            "$ErrorActionPreference = \"Stop\"",
            "$shell = New-Object -ComObject WScript.Shell",
            $"if (-not $shell.AppActivate({processId})) {{",
            "    throw \"Antigravity IDE penceresi one getirilemedi.\"",
            "}",
            "Start-Sleep -Milliseconds 300",
            "$shell.SendKeys(\"^%+r\")"
        });
        var encodedScript = Convert.ToBase64String(Encoding.Unicode.GetBytes(script));

        var info = new ProcessStartInfo
        {
            FileName = "powershell.exe",
            UseShellExecute = false,
            CreateNoWindow = true,
            WindowStyle = ProcessWindowStyle.Hidden,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        info.ArgumentList.Add("-NoProfile");
        info.ArgumentList.Add("-ExecutionPolicy");
        info.ArgumentList.Add("Bypass");
        info.ArgumentList.Add("-EncodedCommand");
        info.ArgumentList.Add(encodedScript);

        try
        {
            using var process = Process.Start(info);
            if (process is null)
            {
                Log($"Failed to start reload shortcut sender for account {account.Id}");
                return false;
            }

            if (!process.WaitForExit(8000))
            {
                process.Kill(entireProcessTree: true);
                Log($"Reload shortcut sender timed out for account {account.Id}");
                return false;
            }

            if (process.ExitCode == 0)
            {
                return true;
            }

            var error = process.StandardError.ReadToEnd().Trim();
            var output = process.StandardOutput.ReadToEnd().Trim();
            Log($"Reload shortcut sender failed for account {account.Id}: exit={process.ExitCode}; error={error}; output={output}");
            return false;
        }
        catch (Exception ex)
        {
            Log($"Reload shortcut sender failed for account {account.Id}: {ex.Message}");
            return false;
        }
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

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool IsIconic(IntPtr hWnd);

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
