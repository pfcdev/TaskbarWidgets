using System.Collections.Concurrent;
using System.Diagnostics;
using System.Text;
using System.Text.Json.Nodes;

namespace TaskbarWidgets.Loader.Core;

internal static class CommunityFullTrustSupervisor
{
    private static readonly ConcurrentDictionary<string, RunningRuntime> Running =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, DateTimeOffset> RetryAfter =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly ConcurrentDictionary<string, long> Sequences =
        new(StringComparer.OrdinalIgnoreCase);

    internal static async Task RunAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var configuration = WidgetConfiguration.LoadOrCreate(
                    Path.Combine(AppPaths.DataDirectory, "config.json"));
                var desired = configuration.Widgets
                    .Where(instance => instance.Enabled)
                    .Select(instance => (
                        instance,
                        definition: CommunityWidgetRegistry.Find(instance.WidgetId)))
                    .Where(item =>
                        item.definition is
                        {
                            Valid: true,
                            ManifestSchemaVersion: >= 4,
                            ProcessRuntime: not null
                        } &&
                        CommunityPermissions.IsApproved(item.definition))
                    .GroupBy(item => item.definition!.Id, StringComparer.OrdinalIgnoreCase)
                    .ToDictionary(
                        group => group.Key,
                        group => (
                            definition: group.First().definition!,
                            instances: group.Select(item => item.instance).ToArray()),
                        StringComparer.OrdinalIgnoreCase);

                foreach (var pair in Running.ToArray())
                {
                    if (!desired.ContainsKey(pair.Key) || pair.Value.Process.HasExited)
                    {
                        if (Running.TryRemove(pair.Key, out var stopped))
                        {
                            stopped.Dispose();
                            if (desired.ContainsKey(pair.Key))
                            {
                                RetryAfter[pair.Key] = DateTimeOffset.UtcNow.AddSeconds(2);
                            }
                        }
                    }
                }

                foreach (var pair in desired)
                {
                    if (Running.TryGetValue(pair.Key, out var current))
                    {
                        current.UpdateInstances(pair.Value.instances);
                        continue;
                    }
                    if (RetryAfter.TryGetValue(pair.Key, out var retry) &&
                        retry > DateTimeOffset.UtcNow)
                    {
                        continue;
                    }
                    try
                    {
                        var started = Start(
                            pair.Value.definition,
                            pair.Value.instances,
                            cancellationToken);
                        if (!Running.TryAdd(pair.Key, started))
                        {
                            started.Dispose();
                        }
                        else
                        {
                            RetryAfter.TryRemove(pair.Key, out _);
                        }
                    }
                    catch (Exception ex)
                    {
                        RetryAfter[pair.Key] = DateTimeOffset.UtcNow.AddSeconds(5);
                        Log(pair.Key, "Runtime start failed: " + ex.Message);
                    }
                }
                await Task.Delay(TimeSpan.FromSeconds(1), cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            foreach (var pair in Running.ToArray())
            {
                if (Running.TryRemove(pair.Key, out var runtime))
                {
                    runtime.Dispose();
                }
            }
        }
    }

    internal static bool TryDispatch(
        string? widgetId,
        string? instanceId,
        string action,
        JsonNode? arguments)
    {
        if (string.IsNullOrWhiteSpace(widgetId) ||
            string.IsNullOrWhiteSpace(instanceId) ||
            string.IsNullOrWhiteSpace(action) ||
            action.Length > 120 ||
            !action.All(character =>
                char.IsAsciiLetterOrDigit(character) ||
                character is '.' or '-' or '_' or ':') ||
            !WidgetConfiguration.IsSafeInstanceId(instanceId) ||
            !Running.TryGetValue(widgetId, out var runtime))
        {
            return false;
        }
        return runtime.Send(new JsonObject
        {
            ["type"] = "action",
            ["widgetId"] = widgetId,
            ["instanceId"] = instanceId,
            ["action"] = action,
            ["arguments"] = arguments?.DeepClone() ?? new JsonObject(),
            ["createdAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
        });
    }

    internal static string SerializeProtocolMessageForTool(JsonObject message) =>
        message.ToJsonString();

    private static RunningRuntime Start(
        RuntimeWidgetDefinition definition,
        WidgetInstanceConfiguration[] instances,
        CancellationToken cancellationToken)
    {
        var runtime = definition.ProcessRuntime!;
        var entry = Path.GetFullPath(Path.Combine(definition.SourcePath!, runtime.Entry));
        var root = Path.GetFullPath(definition.SourcePath!)
            .TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!entry.StartsWith(root, StringComparison.OrdinalIgnoreCase) ||
            !File.Exists(entry))
        {
            throw new InvalidDataException("Full-access runtime entry escaped the widget package.");
        }

        var start = BuildStartInfo(definition, entry, runtime);
        var process = Process.Start(start)
                      ?? throw new InvalidOperationException("Full-access runtime did not start.");
        if (runtime.RunAs == "user" && !TryApplyProcessSafetyDefaultsForTool(process))
        {
            Log(definition.Id, "Runtime priority could not be lowered.");
        }
        var running = new RunningRuntime(
            definition,
            runtime,
            process,
            instances,
            cancellationToken);
        Log(definition.Id, $"Runtime started: pid={process.Id}; entry={runtime.Entry}");
        running.StartReaders();
        running.UpdateInstances(instances, force: true);
        return running;
    }

    internal static bool TryApplyProcessSafetyDefaultsForTool(Process process)
    {
        try
        {
            if (process.HasExited)
            {
                return false;
            }
            process.PriorityClass = ProcessPriorityClass.BelowNormal;
            return process.PriorityClass == ProcessPriorityClass.BelowNormal;
        }
        catch
        {
            return false;
        }
    }

    private static ProcessStartInfo BuildStartInfo(
        RuntimeWidgetDefinition definition,
        string entry,
        CommunityProcessRuntimeDefinition runtime)
    {
        var extension = Path.GetExtension(entry).ToLowerInvariant();
        var useProtocol = runtime.Protocol == "json-lines-v1" && runtime.RunAs == "user";
        var start = new ProcessStartInfo
        {
            UseShellExecute = runtime.RunAs == "administrator",
            WorkingDirectory = runtime.WorkingDirectory == "data"
                ? AppPaths.DataDirectory
                : definition.SourcePath!,
            CreateNoWindow = runtime.RunAs == "user",
            RedirectStandardInput = useProtocol,
            RedirectStandardOutput = useProtocol,
            RedirectStandardError = useProtocol,
            StandardOutputEncoding = useProtocol ? Encoding.UTF8 : null,
            StandardErrorEncoding = useProtocol ? Encoding.UTF8 : null
        };
        if (runtime.RunAs == "administrator")
        {
            start.Verb = "runas";
        }

        switch (extension)
        {
            case ".ps1":
                start.FileName = "powershell.exe";
                start.ArgumentList.Add("-NoProfile");
                start.ArgumentList.Add("-ExecutionPolicy");
                start.ArgumentList.Add("Bypass");
                start.ArgumentList.Add("-File");
                start.ArgumentList.Add(entry);
                break;
            case ".cmd":
            case ".bat":
                start.FileName = "cmd.exe";
                start.ArgumentList.Add("/d");
                start.ArgumentList.Add("/c");
                start.ArgumentList.Add(entry);
                break;
            case ".py":
                start.FileName = "python.exe";
                start.ArgumentList.Add(entry);
                break;
            case ".js":
                start.FileName = "node.exe";
                start.ArgumentList.Add(entry);
                break;
            default:
                start.FileName = entry;
                break;
        }
        foreach (var argument in runtime.Arguments)
        {
            start.ArgumentList.Add(
                argument
                    .Replace("{widgetDir}", definition.SourcePath!, StringComparison.Ordinal)
                    .Replace("{dataDir}", AppPaths.DataDirectory, StringComparison.Ordinal));
        }
        return start;
    }

    private static void WriteSnapshot(
        RuntimeWidgetDefinition definition,
        IReadOnlyCollection<WidgetInstanceConfiguration> instances,
        JsonObject message)
    {
        var instanceId = message["instanceId"]?.GetValue<string>() ?? "";
        if (!WidgetConfiguration.IsSafeInstanceId(instanceId))
        {
            return;
        }
        var instance = instances.FirstOrDefault(item =>
            string.Equals(item.InstanceId, instanceId, StringComparison.OrdinalIgnoreCase));
        if (instance is null || message["data"] is not JsonObject data)
        {
            return;
        }
        var sequence = Sequences.AddOrUpdate(instanceId, 1, (_, value) => value + 1);
        var snapshot = new JsonObject
        {
            ["schemaVersion"] = 2,
            ["widgetId"] = definition.Id,
            ["instanceId"] = instanceId,
            ["packageVersion"] = definition.Version,
            ["sequence"] = sequence,
            ["updatedAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            ["status"] = message["status"]?.GetValue<string>() ?? "ok",
            ["data"] = data.DeepClone(),
            ["settings"] = instance.Settings.DeepClone()
        };
        AtomicJson.Write(
            Path.Combine(AppPaths.StateDirectory, instanceId + ".json"),
            snapshot,
            WidgetConfiguration.JsonOptions());
    }

    private static void Log(string widgetId, string message)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.CommunityWidgetLogsDirectory);
            File.AppendAllText(
                Path.Combine(AppPaths.CommunityWidgetLogsDirectory, widgetId + ".log"),
                $"[{DateTimeOffset.Now:O}] {message.ReplaceLineEndings(" ")}{Environment.NewLine}");
        }
        catch
        {
        }
    }

    private sealed class RunningRuntime : IDisposable
    {
        private readonly RuntimeWidgetDefinition _definition;
        private readonly CommunityProcessRuntimeDefinition _runtime;
        private readonly CancellationToken _cancellationToken;
        private readonly object _writeLock = new();
        private WidgetInstanceConfiguration[] _instances;
        private string _instanceSignature = "";
        private bool _disposed;

        internal RunningRuntime(
            RuntimeWidgetDefinition definition,
            CommunityProcessRuntimeDefinition runtime,
            Process process,
            WidgetInstanceConfiguration[] instances,
            CancellationToken cancellationToken)
        {
            _definition = definition;
            _runtime = runtime;
            Process = process;
            _instances = instances;
            _cancellationToken = cancellationToken;
        }

        internal Process Process { get; }

        internal void StartReaders()
        {
            if (_runtime.Protocol != "json-lines-v1" || _runtime.RunAs != "user")
            {
                return;
            }
            _ = Task.Run(ReadOutputAsync, _cancellationToken);
            _ = Task.Run(ReadErrorAsync, _cancellationToken);
        }

        internal void UpdateInstances(
            WidgetInstanceConfiguration[] instances,
            bool force = false)
        {
            _instances = instances;
            var signature = string.Join(
                "|",
                instances.OrderBy(item => item.InstanceId).Select(item =>
                    item.InstanceId + ":" + item.Settings.ToJsonString()));
            if (!force && signature == _instanceSignature)
            {
                return;
            }
            _instanceSignature = signature;
            Send(new JsonObject
            {
                ["type"] = force ? "initialize" : "instancesChanged",
                ["widgetId"] = _definition.Id,
                ["widgetVersion"] = _definition.Version,
                ["widgetDirectory"] = _definition.SourcePath,
                ["dataDirectory"] = AppPaths.DataDirectory,
                ["instances"] = new JsonArray(instances.Select(instance =>
                    (JsonNode)new JsonObject
                    {
                        ["instanceId"] = instance.InstanceId,
                        ["settings"] = instance.Settings.DeepClone()
                    }).ToArray())
            });
        }

        internal bool Send(JsonObject message)
        {
            if (_disposed ||
                Process.HasExited ||
                _runtime.Protocol != "json-lines-v1" ||
                _runtime.RunAs != "user")
            {
                return false;
            }
            try
            {
                var json = SerializeProtocolMessageForTool(message);
                if (json.Length > 1_048_576)
                {
                    return false;
                }
                lock (_writeLock)
                {
                    Process.StandardInput.WriteLine(json);
                    Process.StandardInput.Flush();
                }
                return true;
            }
            catch (Exception ex)
            {
                Log(_definition.Id, "Runtime input failed: " + ex.Message);
                return false;
            }
        }

        private async Task ReadOutputAsync()
        {
            try
            {
                while (!_cancellationToken.IsCancellationRequested && !Process.HasExited)
                {
                    var line = await Process.StandardOutput.ReadLineAsync(_cancellationToken);
                    if (line is null)
                    {
                        break;
                    }
                    if (line.Length > 1_048_576)
                    {
                        Log(_definition.Id, "Runtime output line exceeded 1 MB.");
                        continue;
                    }
                    try
                    {
                        var message = JsonNode.Parse(line)?.AsObject();
                        var type = message?["type"]?.GetValue<string>();
                        if (message is not null && type == "snapshot")
                        {
                            WriteSnapshot(_definition, _instances, message);
                        }
                        else if (message is not null && type == "log")
                        {
                            Log(
                                _definition.Id,
                                message["message"]?.GetValue<string>() ?? "Runtime log");
                        }
                    }
                    catch (Exception ex)
                    {
                        Log(_definition.Id, "Invalid runtime output: " + ex.Message);
                    }
                }
            }
            catch (OperationCanceledException) when (_cancellationToken.IsCancellationRequested)
            {
            }
            catch (Exception ex)
            {
                Log(_definition.Id, "Runtime output reader failed: " + ex.Message);
            }
        }

        private async Task ReadErrorAsync()
        {
            try
            {
                while (!_cancellationToken.IsCancellationRequested && !Process.HasExited)
                {
                    var line = await Process.StandardError.ReadLineAsync(_cancellationToken);
                    if (line is null)
                    {
                        break;
                    }
                    Log(
                        _definition.Id,
                        line.Length > 2000 ? line[..2000] : line);
                }
            }
            catch (OperationCanceledException) when (_cancellationToken.IsCancellationRequested)
            {
            }
            catch
            {
            }
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }
            _disposed = true;
            try
            {
                if (!Process.HasExited)
                {
                    if (_runtime.Protocol == "json-lines-v1" && _runtime.RunAs == "user")
                    {
                        lock (_writeLock)
                        {
                            Process.StandardInput.WriteLine("{\"type\":\"shutdown\"}");
                            Process.StandardInput.Flush();
                        }
                    }
                    if (!Process.WaitForExit(1500))
                    {
                        Process.Kill(entireProcessTree: true);
                        Process.WaitForExit(1500);
                    }
                }
            }
            catch
            {
            }
            Process.Dispose();
            Log(_definition.Id, "Runtime stopped.");
        }
    }
}
