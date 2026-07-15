namespace TaskbarWidgets.Loader;

internal static class AppPaths
{
    public static string InstallDirectory { get; } = ResolveInstallDirectory();
    public static string DataDirectory { get; } = Path.Combine(InstallDirectory, "Data");
    public static string StateDirectory { get; } = Path.Combine(DataDirectory, "State");
    public static string CommandsDirectory { get; } = Path.Combine(DataDirectory, "Commands");
    public static string LogsDirectory { get; } = Path.Combine(DataDirectory, "Logs");
    public static string RuntimeDirectory { get; } = Path.Combine(DataDirectory, "Runtime");

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
