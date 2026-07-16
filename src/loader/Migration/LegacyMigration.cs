using System.Text;
using System.Diagnostics;
using Microsoft.Win32;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader.Migration;

internal static class LegacyMigration
{
    private const string LegacyProductName = "TaskbarStats";
    private static readonly string MarkerPath = Path.Combine(AppPaths.DataDirectory, ".migration-v1-complete");

    public static void Run()
    {
        if (File.Exists(MarkerPath))
        {
            MigrateSystemMeterSettings();
            return;
        }

        Directory.CreateDirectory(AppPaths.DataDirectory);
        var source = CandidateDirectories().FirstOrDefault(ContainsUserData);
        if (source is not null)
        {
            CopyFile(source, "settings.json");
            CopyFile(source, "active-codex-account.txt");
            CopyDirectory(source, "Accounts");
            CopyDirectory(source, "IdeProfiles");

            var legacyLibraries = Path.Combine(source, "WidgetLibraries");
            if (Directory.Exists(legacyLibraries))
            {
                CopyDirectory(source, "WidgetLibraries", Path.Combine("Legacy", "WidgetLibraries"));
            }

            MigrateWidgetConfiguration(source);
            RewriteLegacyAccountPaths();
        }

        var configurationPath = Path.Combine(AppPaths.DataDirectory, "config.json");
        if (!File.Exists(configurationPath))
        {
            new WidgetConfiguration().Save(configurationPath);
        }

        StopLegacyProcesses();
        RemoveLegacyStartupEntry();
        File.WriteAllText(
            MarkerPath,
            $"MigratedAt={DateTimeOffset.UtcNow:O}{Environment.NewLine}Source={source ?? "none"}{Environment.NewLine}",
            Encoding.UTF8);
        MigrateSystemMeterSettings();
    }

    private static void MigrateSystemMeterSettings()
    {
        var path = Path.Combine(AppPaths.DataDirectory, "config.json");
        if (!File.Exists(path))
        {
            return;
        }

        try
        {
            var configuration = WidgetConfiguration.LoadOrCreate(path, out var normalized);
            if (normalized)
            {
                configuration.Save(path);
            }
        }
        catch
        {
            // A malformed user config is left untouched; Settings can report it.
        }
    }

    private static IEnumerable<string> CandidateDirectories()
    {
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var candidates = new[]
        {
            Path.Combine(localAppData, "Programs", LegacyProductName),
            Path.Combine(localAppData, LegacyProductName),
            Path.Combine(Path.GetDirectoryName(AppPaths.InstallDirectory) ?? localAppData, LegacyProductName),
            Path.Combine(AppPaths.InstallDirectory, "..", LegacyProductName)
        };

        return candidates
            .Select(Path.GetFullPath)
            .Where(path => !string.Equals(path, AppPaths.InstallDirectory, StringComparison.OrdinalIgnoreCase))
            .Distinct(StringComparer.OrdinalIgnoreCase);
    }

    private static bool ContainsUserData(string path) =>
        File.Exists(Path.Combine(path, "widget-settings.json")) ||
        File.Exists(Path.Combine(path, "settings.json")) ||
        Directory.Exists(Path.Combine(path, "Accounts"));

    private static void MigrateWidgetConfiguration(string source)
    {
        var target = Path.Combine(AppPaths.DataDirectory, "config.json");
        if (File.Exists(target))
        {
            return;
        }

        var legacy = Path.Combine(source, "widget-settings.json");
        var configuration = WidgetConfiguration.LoadOrCreate(legacy);
        configuration.Save(target);
    }

    private static void CopyFile(string sourceRoot, string relativePath)
    {
        var source = Path.Combine(sourceRoot, relativePath);
        var target = Path.Combine(AppPaths.DataDirectory, relativePath);
        if (!File.Exists(source) || File.Exists(target))
        {
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Copy(source, target);
    }

    private static void CopyDirectory(string sourceRoot, string sourceName, string? targetName = null)
    {
        var source = Path.Combine(sourceRoot, sourceName);
        if (!Directory.Exists(source))
        {
            return;
        }

        var target = Path.Combine(AppPaths.DataDirectory, targetName ?? sourceName);
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(source, file);
            var destination = Path.Combine(target, relative);
            if (File.Exists(destination))
            {
                continue;
            }

            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(file, destination);
        }
    }

    private static void RewriteLegacyAccountPaths()
    {
        var settingsPath = Path.Combine(AppPaths.DataDirectory, "settings.json");
        if (!File.Exists(settingsPath))
        {
            return;
        }

        var text = File.ReadAllText(settingsPath);
        var localAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var legacyRoots = new[]
        {
            Path.Combine(localAppData, "Programs", LegacyProductName),
            Path.Combine(localAppData, LegacyProductName)
        };
        foreach (var legacyRoot in legacyRoots)
        {
            text = text.Replace(
                legacyRoot.Replace("\\", "\\\\"),
                AppPaths.DataDirectory.Replace("\\", "\\\\"),
                StringComparison.OrdinalIgnoreCase);
            text = text.Replace(legacyRoot, AppPaths.DataDirectory, StringComparison.OrdinalIgnoreCase);
        }

        File.WriteAllText(settingsPath, text, Encoding.UTF8);
    }

    private static void RemoveLegacyStartupEntry()
    {
        using var runKey = Registry.CurrentUser.OpenSubKey(
            @"Software\Microsoft\Windows\CurrentVersion\Run", writable: true);
        runKey?.DeleteValue(LegacyProductName, throwOnMissingValue: false);
    }

    private static void StopLegacyProcesses()
    {
        foreach (var process in Process.GetProcessesByName(LegacyProductName))
        {
            try
            {
                process.Kill(entireProcessTree: true);
            }
            catch
            {
                // Migration continues even if another session owns the legacy process.
            }
            finally
            {
                process.Dispose();
            }
        }
    }
}
