using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class SystemNetworkProvider : IWidgetProvider
{
    public string Id => "system-network";
    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        SystemMetricsWorker.RunAsync(Id, cancellationToken);
}
