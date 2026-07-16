using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class SystemMemoryProvider : IWidgetProvider
{
    public string Id => "system-memory";
    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        SystemMetricsWorker.RunAsync(Id, cancellationToken);
}
