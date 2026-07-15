using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class WeatherProvider : IWidgetProvider
{
    public string Id => "weather-static";

    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        WeatherWorker.RunAsync(cancellationToken);
}
