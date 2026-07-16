using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class SystemCpuProvider : IWidgetProvider
{
    public string Id => "system-cpu";
    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        SystemMetricsWorker.RunAsync(Id, cancellationToken);
}
