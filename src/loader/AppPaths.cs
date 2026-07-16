namespace TaskbarWidgets.Loader;

internal static class AppPaths
{
    public static string InstallDirectory { get; } = ResolveInstallDirectory();
    public static string DataDirectory { get; } = Path.Combine(InstallDirectory, "Data");
    public static string StateDirectory { get; } = Path.Combine(DataDirectory, "State");
    public static string CommandsDirectory { get; } = Path.Combine(DataDirectory, "Commands");
    public static string LogsDirectory { get; } = Path.Combine(DataDirectory, "Logs");
    public static string RuntimeDirectory { get; } = Path.Combine(DataDirectory, "Runtime");
    public static string UserDataDirectory { get; } = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "TaskbarWidgets");
    public static string CommunityWidgetsDirectory { get; } = Path.Combine(
        UserDataDirectory, "CommunityWidgets");
    public static string CommunityWidgetCacheDirectory { get; } = Path.Combine(
        UserDataDirectory, "WidgetCache");
    public static string CommunityWidgetLogsDirectory { get; } = Path.Combine(
        UserDataDirectory, "WidgetLogs");
    public static string RuntimeWidgetCatalogPath { get; } = Path.Combine(
        RuntimeDirectory, "WidgetCatalog.json");
    public static string CommunityWidgetUpdateStatePath { get; } = Path.Combine(
        RuntimeDirectory, "community-widget-updates.json");
    public static string WidgetInstallRequestPath { get; } = Path.Combine(
        RuntimeDirectory, "widget-install-request.json");

    // Kept as a source-compatibility alias while the loader is split into services.
    public static string AppDirectory => DataDirectory;

    private static string ResolveInstallDirectory()
    {
        var processPath = Environment.ProcessPath;
        if (!string.IsNullOrWhiteSpace(processPath))
        {
            var directory = Path.GetDirectoryName(Path.GetFullPath(processPath));
            if (!string.IsNullOrWhiteSpace(directory))
            {
                return directory;
            }
        }

        return AppContext.BaseDirectory.TrimEnd(
            Path.DirectorySeparatorChar,
            Path.AltDirectorySeparatorChar);
    }
}
