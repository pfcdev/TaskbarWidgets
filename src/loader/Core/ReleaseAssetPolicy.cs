namespace TaskbarWidgets.Loader.Core;

internal static class ReleaseAssetPolicy
{
    public const string SetupName = "TaskbarWidgetsSetup-x64.exe";
    public const string SetupSha256Name = "TaskbarWidgetsSetup-x64.exe.sha256";

    public static (string DownloadUrl, string? Sha256Url)? Select(
        IEnumerable<(string Name, string Url)> assets)
    {
        string? setup = null;
        string? checksum = null;
        foreach (var (name, url) in assets)
        {
            if (string.Equals(name, SetupName, StringComparison.OrdinalIgnoreCase))
            {
                setup = url;
            }
            else if (string.Equals(name, SetupSha256Name, StringComparison.OrdinalIgnoreCase))
            {
                checksum = url;
            }
        }

        return string.IsNullOrWhiteSpace(setup) ? null : (setup, checksum);
    }
}
