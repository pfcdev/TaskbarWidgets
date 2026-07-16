using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class SystemStorageProvider : IWidgetProvider
{
    public string Id => "system-storage";
    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        SystemMetricsWorker.RunAsync(Id, cancellationToken);
}
