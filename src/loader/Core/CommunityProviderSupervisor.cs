using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace TaskbarWidgets.Loader.Core;

internal static class CommunityProviderSupervisor
{
    private static readonly Dictionary<string, DateTimeOffset> Due =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, long> Sequences =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, int> Failures =
        new(StringComparer.OrdinalIgnoreCase);

    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await TickAsync(cancellationToken);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                Log("Community provider supervisor: " + ex.Message);
            }
            await Task.Delay(250, cancellationToken);
        }
    }

    private static async Task TickAsync(CancellationToken cancellationToken)
    {
        var configuration = WidgetConfiguration.LoadOrCreate(
            Path.Combine(AppPaths.DataDirectory, "config.json"));
        foreach (var instance in configuration.Widgets.Where(widget => widget.Enabled))
        {
            var definition = CommunityWidgetRegistry.Find(instance.WidgetId);
            if (definition is not { Valid: true, Renderer: "declarative" }) continue;
            if (!IsSafeInstanceId(instance.InstanceId)) continue;
            if (definition.Permissions.Count > 0 &&
                instance.Settings["_permissionsApproved"]?.GetValue<bool?>() != true)
            {
                continue;
            }

            var now = DateTimeOffset.UtcNow;
            if (Due.TryGetValue(instance.InstanceId, out var due) && now < due) continue;
            var refresh = definition.Provider?.RefreshSeconds ?? 3d;
            try
            {
                var data = await ReadProviderDataAsync(definition, instance, cancellationToken);
                WriteSnapshot(definition, instance, data);
                Failures.Remove(instance.InstanceId);
                Due[instance.InstanceId] = now.AddSeconds(refresh);
            }
            catch (Exception ex)
            {
                var failures = Failures.GetValueOrDefault(instance.InstanceId) + 1;
                Failures[instance.InstanceId] = failures;
                Due[instance.InstanceId] = now.AddSeconds(Math.Min(300, Math.Pow(2, Math.Min(failures, 8))));
                WriteProviderLog(instance.InstanceId, ex.Message);
                if (failures >= 5)
                {
                    WriteSnapshot(definition, instance, new JsonObject
                    {
                        ["error"] = "Provider was paused after repeated failures."
                    });
                }
            }
        }
    }

    private static async Task<JsonObject> ReadProviderDataAsync(
        RuntimeWidgetDefinition definition,
        WidgetInstanceConfiguration instance,
        CancellationToken cancellationToken)
    {
        var provider = definition.Provider;
        if (provider is null || provider.Type == "static")
        {
            return provider?.Configuration["data"]?.DeepClone().AsObject() ?? new JsonObject();
        }
        if (provider.Type == "clock")
        {
            var now = DateTimeOffset.Now;
            return new JsonObject
            {
                ["unix"] = now.ToUnixTimeSeconds(),
                ["time"] = now.ToString("HH:mm:ss"),
                ["date"] = now.ToString("yyyy-MM-dd"),
                ["iso"] = now.ToString("O")
            };
        }
        if (provider.Type == "http-json")
        {
            var url = provider.Configuration["url"]?.GetValue<string>()
                      ?? throw new InvalidDataException("http-json provider requires url.");
            return await CommunityHttpBroker.GetJsonAsync(url, definition.Permissions, cancellationToken);
        }
        if (provider.Type != "javascript" || string.IsNullOrWhiteSpace(provider.Path))
        {
            throw new InvalidDataException("Unsupported provider configuration.");
        }
        var sourcePath = Path.GetFullPath(Path.Combine(definition.SourcePath!, provider.Path));
        var root = Path.GetFullPath(definition.SourcePath!).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if (!sourcePath.StartsWith(root, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("Provider path escaped the package.");
        }
        var source = await File.ReadAllTextAsync(sourcePath, cancellationToken);
        var request = new JsonObject
        {
            ["source"] = source,
            ["settings"] = CommunitySecretStore.Unprotect(instance.Settings),
            ["networkHosts"] = definition.Permissions["network"]?.DeepClone() ?? new JsonArray(),
            ["systemMetrics"] = ReadAllowedSystemMetrics(definition.Permissions),
            ["widgetId"] = definition.Id,
            ["instanceId"] = instance.InstanceId
        };
        return await RunWidgetHostAsync(request, cancellationToken);
    }

    private static JsonObject ReadAllowedSystemMetrics(JsonObject permissions)
    {
        var result = new JsonObject();
        if (permissions["systemMetrics"] is not JsonArray metrics) return result;
        foreach (var node in metrics)
        {
            var metric = node?.GetValue<string>();
            if (metric is not ("cpu" or "storage" or "network" or "memory")) continue;
            try
            {
                var path = Path.Combine(AppPaths.StateDirectory, $"system-{metric}.json");
                var snapshot = JsonNode.Parse(File.ReadAllText(path))?.AsObject();
                if (snapshot?["widgetId"]?.GetValue<string>() == $"system-{metric}" &&
                    snapshot["data"] is JsonObject data)
                {
                    result[metric] = data.DeepClone();
                }
            }
            catch { }
        }
        return result;
    }

    private static async Task<JsonObject> RunWidgetHostAsync(
        JsonObject request,
        CancellationToken cancellationToken)
    {
        var hostPath = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.WidgetHost.exe");
        if (!File.Exists(hostPath)) throw new FileNotFoundException("WidgetHost is not installed.", hostPath);
        var start = new ProcessStartInfo(hostPath)
        {
            UseShellExecute = false,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = AppPaths.CommunityWidgetCacheDirectory
        };
        using var process = Process.Start(start) ?? throw new InvalidOperationException("WidgetHost could not start.");
        using var job = WidgetHostJob.Attach(process);
        await process.StandardInput.WriteAsync(request.ToJsonString(WidgetConfiguration.JsonOptions()));
        process.StandardInput.Close();
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(5));
        try
        {
            await process.WaitForExitAsync(timeout.Token);
        }
        catch
        {
            try { process.Kill(entireProcessTree: true); } catch { }
            throw new TimeoutException("Provider exceeded the 5 second wall-time limit.");
        }
        var output = await process.StandardOutput.ReadToEndAsync(cancellationToken);
        var error = await process.StandardError.ReadToEndAsync(cancellationToken);
        if (process.ExitCode != 0) throw new InvalidOperationException(error.Length > 300 ? error[..300] : error);
        if (output.Length > 65_536) throw new InvalidDataException("Provider result exceeds 64 KB.");
        return JsonNode.Parse(output)?.AsObject()
               ?? throw new InvalidDataException("Provider must return a JSON object.");
    }

    private static void WriteSnapshot(
        RuntimeWidgetDefinition definition,
        WidgetInstanceConfiguration instance,
        JsonObject data)
    {
        var sequence = Sequences.GetValueOrDefault(instance.InstanceId) + 1;
        Sequences[instance.InstanceId] = sequence;
        var snapshot = new JsonObject
        {
            ["schemaVersion"] = 2,
            ["widgetId"] = definition.Id,
            ["instanceId"] = instance.InstanceId,
            ["packageVersion"] = definition.Version,
            ["sequence"] = sequence,
            ["updatedAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            ["data"] = data,
            ["settings"] = instance.Settings.DeepClone()
        };
        AtomicJson.Write(
            Path.Combine(AppPaths.StateDirectory, instance.InstanceId + ".json"),
            snapshot,
            WidgetConfiguration.JsonOptions());
    }

    private static bool IsSafeInstanceId(string id) =>
        id.Length is > 0 and <= 160 && id.All(character =>
            char.IsAsciiLetterOrDigit(character) || character is '.' or '-' or '_');

    private static void WriteProviderLog(string instanceId, string message)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.CommunityWidgetLogsDirectory);
            File.AppendAllText(
                Path.Combine(AppPaths.CommunityWidgetLogsDirectory, instanceId + ".log"),
                $"[{DateTimeOffset.Now:O}] {message.ReplaceLineEndings(" ")}{Environment.NewLine}");
        }
        catch { }
    }

    private static void Log(string message)
    {
        try
        {
            File.AppendAllText(Path.Combine(AppPaths.LogsDirectory, "loader.log"),
                $"[{DateTimeOffset.Now:O}] {message}{Environment.NewLine}");
        }
        catch { }
    }
}

internal static class CommunitySecretStore
{
    public static JsonObject Unprotect(JsonObject settings)
    {
        var result = (JsonObject)settings.DeepClone();
        foreach (var item in result.ToArray())
        {
            if (item.Value is not JsonValue value ||
                !value.TryGetValue<string>(out var text) ||
                !text.StartsWith("dpapi:", StringComparison.OrdinalIgnoreCase)) continue;
            try
            {
                var encrypted = Convert.FromHexString(text[6..]);
                result[item.Key] = UnprotectBytes(encrypted);
            }
            catch
            {
                result[item.Key] = "";
            }
        }
        return result;
    }

    private static string UnprotectBytes(byte[] encrypted)
    {
        var inputPointer = Marshal.AllocHGlobal(encrypted.Length);
        Marshal.Copy(encrypted, 0, inputPointer, encrypted.Length);
        var input = new DataBlob { Size = encrypted.Length, Data = inputPointer };
        try
        {
            if (!CryptUnprotectData(ref input, IntPtr.Zero, IntPtr.Zero, IntPtr.Zero,
                    IntPtr.Zero, 0x1, out var output))
            {
                throw new InvalidOperationException("DPAPI decryption failed.");
            }
            try
            {
                var bytes = new byte[output.Size];
                Marshal.Copy(output.Data, bytes, 0, output.Size);
                return System.Text.Encoding.UTF8.GetString(bytes);
            }
            finally
            {
                LocalFree(output.Data);
            }
        }
        finally
        {
            Marshal.FreeHGlobal(inputPointer);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct DataBlob
    {
        public int Size;
        public IntPtr Data;
    }

    [DllImport("crypt32.dll", SetLastError = true)]
    private static extern bool CryptUnprotectData(
        ref DataBlob dataIn, IntPtr description, IntPtr optionalEntropy,
        IntPtr reserved, IntPtr prompt, int flags, out DataBlob dataOut);

    [DllImport("kernel32.dll")]
    private static extern IntPtr LocalFree(IntPtr memory);
}

internal sealed class WidgetHostJob : IDisposable
{
    private const uint JobObjectLimitActiveProcess = 0x00000008;
    private const uint JobObjectLimitProcessMemory = 0x00000100;
    private const uint JobObjectLimitKillOnJobClose = 0x00002000;
    private readonly SafeFileHandle _handle;

    private WidgetHostJob(SafeFileHandle handle) => _handle = handle;

    public static WidgetHostJob Attach(Process process)
    {
        var handle = new SafeFileHandle(CreateJobObject(IntPtr.Zero, null), ownsHandle: true);
        if (handle.IsInvalid) throw new InvalidOperationException("A provider Job Object could not be created.");
        var information = new JobObjectExtendedLimitInformation
        {
            BasicLimitInformation = new JobObjectBasicLimitInformation
            {
                LimitFlags = JobObjectLimitActiveProcess | JobObjectLimitProcessMemory | JobObjectLimitKillOnJobClose,
                ActiveProcessLimit = 1
            },
            ProcessMemoryLimit = (nuint)(64 * 1024 * 1024)
        };
        var size = Marshal.SizeOf<JobObjectExtendedLimitInformation>();
        var pointer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(information, pointer, false);
            if (!SetInformationJobObject(handle, 9, pointer, (uint)size) ||
                !AssignProcessToJobObject(handle, process.Handle))
            {
                throw new InvalidOperationException("WidgetHost could not be placed in its resource Job Object.");
            }
        }
        catch
        {
            handle.Dispose();
            throw;
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }
        return new WidgetHostJob(handle);
    }

    public void Dispose() => _handle.Dispose();

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectBasicLimitInformation
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public nuint MinimumWorkingSetSize;
        public nuint MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public nuint Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct IoCounters
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct JobObjectExtendedLimitInformation
    {
        public JobObjectBasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public nuint ProcessMemoryLimit;
        public nuint JobMemoryLimit;
        public nuint PeakProcessMemoryUsed;
        public nuint PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr CreateJobObject(IntPtr securityAttributes, string? name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetInformationJobObject(
        SafeFileHandle job, int informationClass, IntPtr information, uint informationLength);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
}

internal static class CommunityHttpBroker
{
    private static readonly HttpClient Client = new(new HttpClientHandler { AllowAutoRedirect = false })
    {
        Timeout = TimeSpan.FromSeconds(5)
    };

    public static async Task<JsonObject> GetJsonAsync(
        string address,
        JsonObject permissions,
        CancellationToken cancellationToken)
    {
        var allowed = permissions["network"]?.AsArray()
            .Select(node => node?.GetValue<string>())
            .Where(host => !string.IsNullOrWhiteSpace(host))
            .ToHashSet(StringComparer.OrdinalIgnoreCase) ?? [];
        if (!Uri.TryCreate(address, UriKind.Absolute, out var uri) ||
            uri.Scheme != Uri.UriSchemeHttps || !allowed.Contains(uri.IdnHost))
        {
            throw new InvalidOperationException("HTTP target is not allowed by the widget manifest.");
        }
        using var response = await Client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        response.EnsureSuccessStatusCode();
        if (response.Content.Headers.ContentLength is > 262144)
        {
            throw new InvalidDataException("HTTP response exceeds 256 KB.");
        }
        await using var stream = await response.Content.ReadAsStreamAsync(cancellationToken);
        using var limited = new MemoryStream();
        await stream.CopyToAsync(limited, cancellationToken);
        if (limited.Length > 262144) throw new InvalidDataException("HTTP response exceeds 256 KB.");
        limited.Position = 0;
        return await JsonNode.ParseAsync(limited, cancellationToken: cancellationToken) is JsonObject result
            ? result
            : throw new InvalidDataException("HTTP provider response must be a JSON object.");
    }
}
