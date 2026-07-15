using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class DiscordVoiceProvider : IWidgetProvider
{
    public string Id => "discord-voice";

    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        DiscordVoiceWorker.RunAsync(cancellationToken);
}
