using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using Microsoft.Win32;
using TaskbarWidgets.Loader.Migration;

namespace TaskbarWidgets.Loader;

internal static class Program
{
    private const string AppName = "TaskbarWidgets";
    private static readonly string AppDirectory = AppPaths.DataDirectory;
    private static readonly string RuntimeRoot = AppPaths.RuntimeDirectory;
    private static readonly string LogsDirectory = AppPaths.LogsDirectory;
    private static readonly string LoaderLogPath = Path.Combine(LogsDirectory, "loader.log");
    private const string MutexName = @"Local\TaskbarWidgetsProductLoader";
    private const string LoaderShutdownEventName = @"Local\TaskbarWidgetsLoaderShutdown";
    private static bool s_consoleEnabled;

    private static async Task<int> Main(string[] args)
    {
        LegacyMigration.Run();
        Directory.CreateDirectory(AppDirectory);
        Directory.CreateDirectory(RuntimeRoot);
        Directory.CreateDirectory(LogsDirectory);
        Directory.CreateDirectory(AppPaths.StateDirectory);
        Directory.CreateDirectory(AppPaths.CommandsDirectory);

        s_consoleEnabled = args.Any(arg => string.Equals(arg, "--console", StringComparison.OrdinalIgnoreCase));
        if (s_consoleEnabled)
        {
            if (!AttachConsole(unchecked((uint)-1)))
            {
                AllocConsole();
            }
        }

        if (args.Any(arg => string.Equals(arg, "--install-startup", StringComparison.OrdinalIgnoreCase)))
        {
            InstallStartup();
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--settings", StringComparison.OrdinalIgnoreCase)))
        {
            LaunchSettings();
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--uninstall-startup", StringComparison.OrdinalIgnoreCase)))
        {
            UninstallStartup();
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--detach", StringComparison.OrdinalIgnoreCase)))
        {
            SignalLoaderShutdown();
            DetachFromAllExplorers();
            WaitForProcessesToExit("TaskbarWidgets", TimeSpan.FromSeconds(6));
            WaitForProcessesToExit("TaskbarWidgets.MediaHelper", TimeSpan.FromSeconds(4));
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--check-updates", StringComparison.OrdinalIgnoreCase)))
        {
            await GitHubUpdater.CheckOnlyAsync(CancellationToken.None);
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--update", StringComparison.OrdinalIgnoreCase)))
        {
            await GitHubUpdater.CheckAndInstallIfAvailableAsync(CancellationToken.None);
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--download-update", StringComparison.OrdinalIgnoreCase)))
        {
            await GitHubUpdater.DownloadUpdateIfAvailableAsync(CancellationToken.None);
            return 0;
        }

        if (args.Any(arg => string.Equals(arg, "--update-silent", StringComparison.OrdinalIgnoreCase)))
        {
            await GitHubUpdater.CheckAndInstallIfAvailableAsync(CancellationToken.None, silentSetup: true);
            return 0;
        }

        using var mutex = new Mutex(initiallyOwned: true, MutexName, out var createdNew);
        if (!createdNew)
        {
            Log("Another TaskbarWidgets loader instance is already running");
            return 0;
        }

        using var cancellation = new CancellationTokenSource();
        using var shutdownEvent = new EventWaitHandle(
            initialState: false,
            mode: EventResetMode.ManualReset,
            name: LoaderShutdownEventName);
        shutdownEvent.Reset();
        _ = Task.Run(() =>
        {
            shutdownEvent.WaitOne();
            Log("Loader shutdown event signaled");
            cancellation.Cancel();
        });

        Console.CancelKeyPress += (_, e) =>
        {
            e.Cancel = true;
            cancellation.Cancel();
        };
        AppDomain.CurrentDomain.ProcessExit += (_, _) => cancellation.Cancel();

        var hookPath = ExtractHookDll();
        Log($"Runtime hook path: {hookPath}");
        ExtractSettingsApp();
        AccountManager.Initialize();

        var agentTask = RunProviderIsolatedAsync(
            "codex-status", () => CodexStatusWorker.RunAsync(args, cancellation.Token), cancellation.Token);
        var weatherTask = RunProviderIsolatedAsync(
            "weather-static", () => WeatherWorker.RunAsync(cancellation.Token), cancellation.Token);
        var discordTask = RunProviderIsolatedAsync(
            "discord-voice", () => DiscordVoiceWorker.RunAsync(cancellation.Token), cancellation.Token);
        var mediaTask = RunProviderIsolatedAsync(
            "media-player", () => MediaWorker.RunAsync(cancellation.Token), cancellation.Token);
        var steamDownloadTask = RunProviderIsolatedAsync(
            "steam-download", () => SteamDownloadWorker.RunAsync(cancellation.Token), cancellation.Token);
        var watchdogTask = Task.Run(
            () => RunExplorerWatchdogAsync(hookPath, cancellation.Token),
            cancellation.Token);
        var accountCommandsTask = Task.Run(
            () => AccountManager.RunCommandWatcherAsync(cancellation.Token),
            cancellation.Token);
        if (!args.Any(arg => string.Equals(arg, "--no-update-check", StringComparison.OrdinalIgnoreCase)))
        {
            _ = Task.Run(async () =>
            {
                await Task.Delay(TimeSpan.FromSeconds(10), cancellation.Token);
                await GitHubUpdater.CheckOnlyIfDueAsync(
                    TimeSpan.FromHours(6),
                    cancellation.Token);
            }, cancellation.Token);
        }

        try
        {
            await Task.WhenAll(
                agentTask,
                weatherTask,
                discordTask,
                mediaTask,
                steamDownloadTask,
                watchdogTask,
                accountCommandsTask);
        }
        catch (OperationCanceledException)
        {
            // Normal shutdown.
        }
        catch (Exception ex)
        {
            Log($"Fatal loader error: {ex}");
            return 1;
        }

        return 0;
    }

    private static async Task RunProviderIsolatedAsync(
        string widgetId,
        Func<Task> run,
        CancellationToken cancellationToken)
    {
        var failureCount = 0;
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                await run();
                failureCount = 0;
                if (!cancellationToken.IsCancellationRequested)
                {
                    throw new InvalidOperationException("Provider stopped unexpectedly.");
                }
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                return;
            }
            catch (Exception ex)
            {
                failureCount = Math.Min(failureCount + 1, 8);
                var delay = TimeSpan.FromSeconds(Math.Min(300, 1 << failureCount));
                Log($"Provider {widgetId} failed; retrying in {delay.TotalSeconds:0}s: {ex.Message}");
                await Task.Delay(delay, cancellationToken);
            }
        }
    }

    private static async Task RunExplorerWatchdogAsync(
        string hookPath,
        CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            foreach (var process in GetCurrentSessionExplorers())
            {
                using (process)
                {
                    try
                    {
                        if (!IsHookLoaded(process, hookPath))
                        {
                            InjectHook(process, hookPath);
                        }
                    }
                    catch (Exception ex)
                    {
                        Log($"Watchdog skipped explorer {process.Id}: {ex.Message}");
                    }
                }
            }

            await Task.Delay(TimeSpan.FromSeconds(4), cancellationToken);
        }
    }

    private static IEnumerable<Process> GetCurrentSessionExplorers()
    {
        var sessionId = Process.GetCurrentProcess().SessionId;
        foreach (var process in Process.GetProcessesByName("explorer"))
        {
            if (process.SessionId == sessionId)
            {
                yield return process;
            }
            else
            {
                process.Dispose();
            }
        }
    }

    private static string ExtractHookDll()
    {
        var assembly = Assembly.GetExecutingAssembly();
        var resourceName = assembly.GetManifestResourceNames()
            .FirstOrDefault(name => name.EndsWith(
                "TaskbarWidgets.Hook.dll",
                StringComparison.OrdinalIgnoreCase));
        if (resourceName is null)
        {
            throw new InvalidOperationException(
                "Embedded TaskbarWidgets.Hook.dll resource was not found");
        }

        using var stream = assembly.GetManifestResourceStream(resourceName) ??
                           throw new InvalidOperationException(
                               "Embedded hook DLL resource could not be opened");
        using var memory = new MemoryStream();
        stream.CopyTo(memory);
        var bytes = memory.ToArray();
        var hash = Convert.ToHexString(SHA256.HashData(bytes))[..16].ToLowerInvariant();
        var directory = Path.Combine(RuntimeRoot, hash);
        Directory.CreateDirectory(directory);
        var path = Path.Combine(directory, "TaskbarWidgets.Hook.dll");

        if (!File.Exists(path) || new FileInfo(path).Length != bytes.Length)
        {
            File.WriteAllBytes(path, bytes);
        }

        CleanupOldRuntimeDirectories(hash);
        return path;
    }

    private static void CleanupOldRuntimeDirectories(string activeHash)
    {
        try
        {
            foreach (var directory in Directory.EnumerateDirectories(RuntimeRoot))
            {
                if (!string.Equals(
                    Path.GetFileName(directory),
                    activeHash,
                    StringComparison.OrdinalIgnoreCase))
                {
                    Directory.Delete(directory, recursive: true);
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Runtime cleanup skipped: {ex.Message}");
        }
    }

    private static void ExtractSettingsApp()
    {
        try
        {
            var assembly = Assembly.GetExecutingAssembly();
            var resourceName = assembly.GetManifestResourceNames()
                .FirstOrDefault(name => name.EndsWith(
                    "TaskbarWidgets.Settings.exe",
                    StringComparison.OrdinalIgnoreCase));
            if (resourceName is null)
            {
                Log("Embedded TaskbarWidgets.Settings.exe resource was not found");
                return;
            }

            using var stream = assembly.GetManifestResourceStream(resourceName);
            if (stream is null)
            {
                Log("Embedded TaskbarWidgets.Settings.exe resource could not be opened");
                return;
            }

            var path = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.Settings.exe");
            using var memory = new MemoryStream();
            stream.CopyTo(memory);
            var bytes = memory.ToArray();

            if (File.Exists(path) && new FileInfo(path).Length == bytes.Length)
            {
                return;
            }

            File.WriteAllBytes(path, bytes);
            Log($"Settings app extracted: {path}");
        }
        catch (Exception ex)
        {
            Log($"Settings app extraction skipped: {ex.Message}");
        }
    }

    private static void LaunchSettings()
    {
        var settingsPath = Path.Combine(AppPaths.InstallDirectory, "TaskbarWidgets.Settings.exe");
        if (!File.Exists(settingsPath))
        {
            throw new FileNotFoundException("Taskbar Widgets Settings was not found.", settingsPath);
        }

        Process.Start(new ProcessStartInfo
        {
            FileName = settingsPath,
            WorkingDirectory = AppPaths.InstallDirectory,
            UseShellExecute = true
        });
    }

    private static bool IsHookLoaded(Process process, string hookPath)
    {
        try
        {
            foreach (ProcessModule module in process.Modules)
            {
                using (module)
                {
                    if (string.Equals(module.FileName, hookPath, StringComparison.OrdinalIgnoreCase))
                    {
                        return true;
                    }
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Module check failed for explorer {process.Id}: {ex.Message}");
        }

        return false;
    }

    private static void InjectHook(Process process, string hookPath)
    {
        Log($"Injecting hook into explorer {process.Id}");
        var processHandle = OpenProcess(
            ProcessAccessFlags.CreateThread |
            ProcessAccessFlags.QueryInformation |
            ProcessAccessFlags.VirtualMemoryOperation |
            ProcessAccessFlags.VirtualMemoryWrite |
            ProcessAccessFlags.VirtualMemoryRead,
            false,
            process.Id);
        if (processHandle == IntPtr.Zero)
        {
            throw new InvalidOperationException($"OpenProcess failed: {Marshal.GetLastWin32Error()}");
        }

        try
        {
            var bytes = System.Text.Encoding.Unicode.GetBytes(hookPath + '\0');
            var remoteMemory = VirtualAllocEx(
                processHandle,
                IntPtr.Zero,
                (nuint)bytes.Length,
                AllocationType.Commit | AllocationType.Reserve,
                MemoryProtection.ReadWrite);
            if (remoteMemory == IntPtr.Zero)
            {
                throw new InvalidOperationException($"VirtualAllocEx failed: {Marshal.GetLastWin32Error()}");
            }

            try
            {
                if (!WriteProcessMemory(processHandle, remoteMemory, bytes, bytes.Length, out _))
                {
                    throw new InvalidOperationException(
                        $"WriteProcessMemory failed: {Marshal.GetLastWin32Error()}");
                }

                var loadLibrary = GetRemoteKernel32ProcAddress(process, "LoadLibraryW");
                if (loadLibrary == IntPtr.Zero)
                {
                    throw new InvalidOperationException(
                        $"GetRemoteKernel32ProcAddress(LoadLibraryW) failed");
                }

                var thread = CreateRemoteThread(
                    processHandle,
                    IntPtr.Zero,
                    0,
                    loadLibrary,
                    remoteMemory,
                    0,
                    IntPtr.Zero);
                if (thread == IntPtr.Zero)
                {
                    throw new InvalidOperationException(
                        $"CreateRemoteThread failed: {Marshal.GetLastWin32Error()}");
                }

                try
                {
                    var waitResult = WaitForSingleObject(thread, 10000);
                    if (waitResult != 0)
                    {
                        Log($"LoadLibraryW remote thread wait returned {waitResult} for explorer {process.Id}");
                    }
                    else if (GetExitCodeThread(thread, out var exitCode))
                    {
                        if (exitCode == 0)
                        {
                            Log($"LoadLibraryW failed in explorer {process.Id}; remote thread exit code was 0");
                        }
                    }
                    else
                    {
                        Log($"GetExitCodeThread failed for explorer {process.Id}: {Marshal.GetLastWin32Error()}");
                    }
                }
                finally
                {
                    CloseHandle(thread);
                }
            }
            finally
            {
                VirtualFreeEx(processHandle, remoteMemory, 0, FreeType.Release);
            }
        }
        finally
        {
            CloseHandle(processHandle);
        }
    }

    private static void DetachFromAllExplorers()
    {
        foreach (var process in GetCurrentSessionExplorers())
        {
            using (process)
            {
                var eventName = GetShutdownEventName(process.Id);
                var handle = OpenEvent(EventAccess.ModifyState, false, eventName);
                if (handle == IntPtr.Zero)
                {
                    Log($"Shutdown event not found for explorer {process.Id}: {eventName}");
                    continue;
                }

                try
                {
                    if (!SetEvent(handle))
                    {
                        Log($"SetEvent failed for explorer {process.Id}: {Marshal.GetLastWin32Error()}");
                    }
                    else
                    {
                        Log($"Detach signaled for explorer {process.Id}");
                    }
                }
                finally
                {
                    CloseHandle(handle);
                }
            }
        }
    }

    private static string GetShutdownEventName(int processId) =>
        $@"Local\TaskbarWidgetsHookShutdown_{processId}";

    private static IntPtr GetRemoteKernel32ProcAddress(Process process, string procName)
    {
        var localKernel32 = GetModuleHandle("kernel32.dll");
        var localProc = GetProcAddress(localKernel32, procName);
        if (localKernel32 == IntPtr.Zero || localProc == IntPtr.Zero)
        {
            return IntPtr.Zero;
        }

        var offset = localProc.ToInt64() - localKernel32.ToInt64();
        foreach (ProcessModule module in process.Modules)
        {
            using (module)
            {
                if (string.Equals(module.ModuleName, "kernel32.dll", StringComparison.OrdinalIgnoreCase))
                {
                    return new IntPtr(module.BaseAddress.ToInt64() + offset);
                }
            }
        }

        return IntPtr.Zero;
    }

    private static void InstallStartup()
    {
        using var key = Registry.CurrentUser.OpenSubKey(
                            @"Software\Microsoft\Windows\CurrentVersion\Run",
                            writable: true) ??
                        Registry.CurrentUser.CreateSubKey(
                            @"Software\Microsoft\Windows\CurrentVersion\Run",
                            writable: true);
        var exe = Environment.ProcessPath ??
                  Process.GetCurrentProcess().MainModule?.FileName ??
                  AppContext.BaseDirectory;
        key.SetValue(AppName, $"\"{exe}\"");
        Log("Startup entry installed");
    }

    private static void UninstallStartup()
    {
        using var key = Registry.CurrentUser.OpenSubKey(
            @"Software\Microsoft\Windows\CurrentVersion\Run",
            writable: true);
        key?.DeleteValue(AppName, throwOnMissingValue: false);
        Log("Startup entry removed");
    }

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(LogsDirectory);
            File.AppendAllText(
                LoaderLogPath,
                $"{DateTimeOffset.Now:O} [loader] {message}{Environment.NewLine}");
            if (s_consoleEnabled)
            {
                Console.Error.WriteLine(message);
            }
        }
        catch
        {
            // Logging must never break the loader.
        }
    }

    private static void SignalLoaderShutdown()
    {
        try
        {
            using var shutdownEvent = EventWaitHandle.OpenExisting(LoaderShutdownEventName);
            shutdownEvent.Set();
            Log("Existing loader shutdown signaled");
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            Log("No running loader shutdown event found");
        }
        catch (Exception ex)
        {
            Log($"Failed to signal loader shutdown: {ex.Message}");
        }
    }

    private static void WaitForProcessesToExit(string processName, TimeSpan timeout)
    {
        var deadline = DateTime.UtcNow + timeout;
        int currentId = Environment.ProcessId;

        while (DateTime.UtcNow < deadline)
        {
            var processes = Process.GetProcessesByName(processName)
                .Where(process => process.Id != currentId)
                .ToArray();
            if (processes.Length == 0)
            {
                return;
            }

            try
            {
                foreach (var process in processes)
                {
                    using (process)
                    {
                        int remaining = Math.Max(
                            100,
                            (int)(deadline - DateTime.UtcNow).TotalMilliseconds);
                        process.WaitForExit(Math.Min(500, remaining));
                    }
                }
            }
            catch (Exception ex)
            {
                Log($"Wait for {processName} shutdown skipped: {ex.Message}");
                return;
            }
        }

        Log($"Timed out waiting for {processName} shutdown");
    }

    [Flags]
    private enum ProcessAccessFlags : uint
    {
        CreateThread = 0x0002,
        QueryInformation = 0x0400,
        VirtualMemoryOperation = 0x0008,
        VirtualMemoryRead = 0x0010,
        VirtualMemoryWrite = 0x0020
    }

    [Flags]
    private enum AllocationType : uint
    {
        Commit = 0x1000,
        Reserve = 0x2000
    }

    private enum MemoryProtection : uint
    {
        ReadWrite = 0x04
    }

    private enum FreeType : uint
    {
        Release = 0x8000
    }

    private enum EventAccess : uint
    {
        ModifyState = 0x0002
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr OpenProcess(
        ProcessAccessFlags processAccess,
        bool inheritHandle,
        int processId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAllocEx(
        IntPtr process,
        IntPtr address,
        nuint size,
        AllocationType allocationType,
        MemoryProtection protect);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFreeEx(
        IntPtr process,
        IntPtr address,
        nuint size,
        FreeType freeType);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteProcessMemory(
        IntPtr process,
        IntPtr baseAddress,
        byte[] buffer,
        int size,
        out nuint written);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr CreateRemoteThread(
        IntPtr process,
        IntPtr threadAttributes,
        uint stackSize,
        IntPtr startAddress,
        IntPtr parameter,
        uint creationFlags,
        IntPtr threadId);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr handle);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string moduleName);

    [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procName);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern IntPtr OpenEvent(
        EventAccess desiredAccess,
        bool inheritHandle,
        string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool SetEvent(IntPtr handle);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AllocConsole();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AttachConsole(uint processId);
}
