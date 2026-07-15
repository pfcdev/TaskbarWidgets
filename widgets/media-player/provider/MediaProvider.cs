using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class MediaProvider : IWidgetProvider
{
    public string Id => "media-player";

    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        MediaWorker.RunAsync(cancellationToken);
}
