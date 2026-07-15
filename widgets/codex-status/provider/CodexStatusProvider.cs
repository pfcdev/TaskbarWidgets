using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

public sealed class CodexStatusProvider : IWidgetProvider
{
    public string Id => "codex-status";

    public Task RunAsync(WidgetProviderContext context, CancellationToken cancellationToken) =>
        CodexStatusWorker.RunAsync([], cancellationToken);
}
