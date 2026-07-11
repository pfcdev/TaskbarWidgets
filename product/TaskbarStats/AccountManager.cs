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
    private static readonly string CommandsDirectory = Path.Combine(AppDirectory, "Commands");
    private static readonly string LogsDirectory = Path.Combine(AppDirectory, "Logs");
    private static readonly string SettingsPath = Path.Combine(AppDirectory, "settings.json");
    private static readonly string LogPath = Path.Combine(LogsDirectory, "loader.log");

    public static void Initialize()
    {
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(AccountsDirectory);
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
            return new AccountSnapshot(
                account.Id,
                account.Label,
                ExpandPath(account.CodexHome));
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
