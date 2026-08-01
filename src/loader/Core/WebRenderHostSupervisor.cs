using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.Json.Nodes;
using Microsoft.Win32.SafeHandles;

namespace TaskbarWidgets.Loader.Core;

internal static class WebRenderHostSupervisor
{
    public static async Task RunAsync(CancellationToken cancellationToken)
    {
        Process? process = null;
        RenderHostJob? job = null;
        DateTimeOffset? startedAt = null;
        var rapidFailures = 0;
        var quarantined = false;
        Directory.CreateDirectory(AppPaths.WebWidgetStorageDirectory);
        Directory.CreateDirectory(AppPaths.RuntimeDirectory);
        using var wakeSignal = new SemaphoreSlim(0, 1);
        var configurationDirty = 1;
        void Wake(object? _, FileSystemEventArgs __)
        {
            Interlocked.Exchange(ref configurationDirty, 1);
            try
            {
                wakeSignal.Release();
            }
            catch (SemaphoreFullException)
            {
            }
        }
        using var configurationWatcher = CreateWatcher(
            AppPaths.DataDirectory, "config.json", Wake);
        using var catalogWatcher = CreateWatcher(
            AppPaths.RuntimeDirectory,
            Path.GetFileName(AppPaths.RuntimeWidgetCatalogPath),
            Wake);
        var required = false;
        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                if (Interlocked.Exchange(ref configurationDirty, 0) != 0)
                {
                    required = IsWebRendererRequired();
                }
                if (process is { HasExited: true })
                {
                    if (startedAt is { } started &&
                        DateTimeOffset.UtcNow - started < TimeSpan.FromSeconds(30))
                    {
                        rapidFailures++;
                    }
                    else
                    {
                        rapidFailures = 0;
                    }
                    process?.Dispose();
                    job?.Dispose();
                    job = null;
                    process = null;
                    startedAt = null;
                    if (rapidFailures >= 3)
                    {
                        quarantined = true;
                        WriteHealth("quarantined",
                            "RenderHost repeatedly exited and was disabled for this session.");
                    }
                }
                if (required && process is null && !quarantined)
                {
                    process = Start();
                    if (process is not null)
                    {
                        job = RenderHostJob.TryAttach(process);
                        startedAt = DateTimeOffset.UtcNow;
                        _ = RequestHookLayoutRefreshAsync(process, cancellationToken);
                    }
                }
                else if (!required && process is { HasExited: false })
                {
                    Stop(process);
                    job?.Dispose();
                    job = null;
                    process.Dispose();
                    process = null;
                }
                await wakeSignal.WaitAsync(TimeSpan.FromSeconds(5), cancellationToken);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        finally
        {
            if (process is not null)
            {
                Stop(process);
                job?.Dispose();
                process.Dispose();
            }
        }
    }

    private static async Task RequestHookLayoutRefreshAsync(
        Process process,
        CancellationToken cancellationToken)
    {
        try
        {
            var configurationPath = Path.Combine(AppPaths.DataDirectory, "config.json");
            foreach (var delay in new[]
                     {
                         TimeSpan.FromMilliseconds(500),
                         TimeSpan.FromSeconds(1)
                     })
            {
                await Task.Delay(delay, cancellationToken);
                if (process.HasExited || !File.Exists(configurationPath))
                {
                    return;
                }
                File.SetLastWriteTimeUtc(configurationPath, DateTime.UtcNow);
            }
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch
        {
            // A later config change still provides another refresh opportunity.
        }
    }

    private static FileSystemWatcher CreateWatcher(
        string directory,
        string filter,
        FileSystemEventHandler handler)
    {
        Directory.CreateDirectory(directory);
        var watcher = new FileSystemWatcher(directory, filter)
        {
            NotifyFilter = NotifyFilters.FileName |
                           NotifyFilters.LastWrite |
                           NotifyFilters.Size
        };
        watcher.Changed += handler;
        watcher.Created += handler;
        watcher.Deleted += handler;
        watcher.Renamed += (sender, args) => handler(sender, args);
        watcher.EnableRaisingEvents = true;
        return watcher;
    }

    internal static bool IsWebRendererRequired()
    {
        try
        {
            var configuration = WidgetConfiguration.LoadOrCreate(
                Path.Combine(AppPaths.DataDirectory, "config.json"));
            return configuration.Widgets.Any(instance =>
                instance.Enabled &&
                CommunityWidgetRegistry.Find(instance.WidgetId) is
                {
                    Valid: true,
                    Renderer: "web"
                } definition &&
                (definition.ManifestSchemaVersion >= 4
                    ? CommunityPermissions.IsApproved(definition)
                    : definition.Permissions.Count == 0 ||
                      instance.Settings["_permissionsApproved"]?.GetValue<bool?>() == true));
        }
        catch
        {
            return false;
        }
    }

    private static Process? Start()
    {
        var executable = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.RenderHost.exe");
        if (!File.Exists(executable))
        {
            WriteHealth("runtime-missing", "TaskbarWidgets.RenderHost.exe is not installed.");
            return null;
        }
        try
        {
            var start = new ProcessStartInfo(executable)
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = AppPaths.InstallDirectory
            };
            start.ArgumentList.Add("--data-dir");
            start.ArgumentList.Add(AppPaths.DataDirectory);
            start.ArgumentList.Add("--community-dir");
            start.ArgumentList.Add(AppPaths.CommunityWidgetsDirectory);
            start.ArgumentList.Add("--storage-dir");
            start.ArgumentList.Add(AppPaths.WebWidgetStorageDirectory);
            var process = Process.Start(start);
            WriteHealth(process is null ? "start-failed" : "starting", null);
            return process;
        }
        catch (Exception ex)
        {
            WriteHealth("start-failed", ex.Message);
            return null;
        }
    }

    private static void Stop(Process process)
    {
        try
        {
            if (process.HasExited) return;
            process.CloseMainWindow();
            if (!process.WaitForExit(1500))
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit(1500);
            }
        }
        catch
        {
        }
    }

    private static void WriteHealth(string status, string? error)
    {
        try
        {
            AtomicJson.Write(
                AppPaths.WebRenderHostHealthPath,
                new JsonObject
                {
                    ["schemaVersion"] = 1,
                    ["status"] = status,
                    ["error"] = error,
                    ["updatedAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
                },
                WidgetConfiguration.JsonOptions());
        }
        catch
        {
        }
    }
}

internal sealed class RenderHostJob : IDisposable
{
    private const uint JobObjectLimitJobMemory = 0x00000200;
    private const uint JobObjectLimitKillOnJobClose = 0x00002000;
    private readonly SafeFileHandle _handle;

    private RenderHostJob(SafeFileHandle handle) => _handle = handle;

    public static RenderHostJob? TryAttach(Process process)
    {
        var handle = new SafeFileHandle(CreateJobObject(IntPtr.Zero, null), ownsHandle: true);
        if (handle.IsInvalid) return null;
        var information = new JobObjectExtendedLimitInformation
        {
            BasicLimitInformation = new JobObjectBasicLimitInformation
            {
                LimitFlags = JobObjectLimitJobMemory | JobObjectLimitKillOnJobClose
            },
            JobMemoryLimit = (nuint)(384L * 1024 * 1024)
        };
        var size = Marshal.SizeOf<JobObjectExtendedLimitInformation>();
        var pointer = Marshal.AllocHGlobal(size);
        try
        {
            Marshal.StructureToPtr(information, pointer, false);
            if (!SetInformationJobObject(handle, 9, pointer, (uint)size) ||
                !AssignProcessToJobObject(handle, process.Handle))
            {
                handle.Dispose();
                return null;
            }
            return new RenderHostJob(handle);
        }
        finally
        {
            Marshal.FreeHGlobal(pointer);
        }
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

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateJobObject(IntPtr securityAttributes, string? name);

    [DllImport("kernel32.dll")]
    private static extern bool SetInformationJobObject(
        SafeFileHandle job, int informationClass, IntPtr information, uint informationLength);

    [DllImport("kernel32.dll")]
    private static extern bool AssignProcessToJobObject(SafeFileHandle job, IntPtr process);
}
