using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class SteamDownloadProvider : IWidgetProvider
{
    public string Id => "steam-download";

    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        SteamDownloadWorker.RunAsync(cancellationToken);
}
