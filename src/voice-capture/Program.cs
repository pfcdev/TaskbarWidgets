using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO.Pipes;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text.Json;
using System.Threading.Channels;

namespace TaskbarWidgets.VoiceCapture;

internal static class Program
{
    internal const string ServiceName = "TaskbarWidgetsVoiceCapture";
    internal const string PipeName = "TaskbarWidgets.VoiceCapture.v1";
    private const string InstallFolderName = "Taskbar Widgets Voice Helper";

    [STAThread]
    private static int Main(string[] args)
    {
        try
        {
            if (args.Contains("--install", StringComparer.OrdinalIgnoreCase))
            {
                Install();
                return 0;
            }
            if (args.Contains("--uninstall", StringComparer.OrdinalIgnoreCase))
            {
                Uninstall();
                return 0;
            }
            if (args.Contains("--service", StringComparer.OrdinalIgnoreCase))
            {
                NativeService.Run();
                return 0;
            }
            return 2;
        }
        catch (Exception ex)
        {
            TryWriteInstallLog(ex.ToString());
            return 1;
        }
    }

    private static void Install()
    {
        EnsureAdministrator();
        RunSc("stop", ServiceName, allowFailure: true);
        Thread.Sleep(500);

        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            InstallFolderName);
        Directory.CreateDirectory(directory);
        var destination = Path.Combine(directory, "TaskbarWidgets.VoiceCapture.exe");
        var source = Environment.ProcessPath ?? throw new InvalidOperationException("Helper path unavailable.");
        if (!string.Equals(source, destination, StringComparison.OrdinalIgnoreCase))
        {
            File.Copy(source, destination, overwrite: true);
        }

        RunSc("delete", ServiceName, allowFailure: true);
        RunSc(
            "create",
            ServiceName,
            "binPath=", $"\"{destination}\" --service",
            "start=", "demand",
            "obj=", "LocalSystem",
            "DisplayName=", "Taskbar Widgets Voice Capture");
        RunSc(
            "description",
            ServiceName,
            "Provides on-demand Discord RTP speaking timing without reading audio content.");

        // Interactive users may only query/start/stop the already configured service.
        // They cannot replace its protected Program Files binary or change the service.
        const string serviceSddl =
            "D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)" +
            "(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)" +
            "(A;;CCLCSWRPWPDTLOCRRC;;;IU)";
        RunSc("sdset", ServiceName, serviceSddl);
        RunSc("start", ServiceName, allowFailure: true);
    }

    private static void Uninstall()
    {
        EnsureAdministrator();
        RunSc("stop", ServiceName, allowFailure: true);
        Thread.Sleep(400);
        RunSc("delete", ServiceName, allowFailure: true);
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
            InstallFolderName);
        var installed = Path.Combine(directory, "TaskbarWidgets.VoiceCapture.exe");
        var current = Environment.ProcessPath ?? "";
        if (!string.Equals(current, installed, StringComparison.OrdinalIgnoreCase))
        {
            File.Delete(installed);
            if (Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
            {
                Directory.Delete(directory);
            }
        }
    }

    private static void EnsureAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        var principal = new WindowsPrincipal(identity);
        if (!principal.IsInRole(WindowsBuiltInRole.Administrator))
        {
            throw new UnauthorizedAccessException("Administrator permission is required.");
        }
    }

    private static void RunSc(params string[] args) => RunSc(args, allowFailure: false);

    private static void RunSc(string first, string second, bool allowFailure) =>
        RunSc([first, second], allowFailure);

    private static void RunSc(string[] args, bool allowFailure)
    {
        var start = new ProcessStartInfo
        {
            FileName = Path.Combine(Environment.SystemDirectory, "sc.exe"),
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var arg in args)
        {
            start.ArgumentList.Add(arg);
        }
        using var process = Process.Start(start) ?? throw new InvalidOperationException("sc.exe could not start.");
        var output = process.StandardOutput.ReadToEnd();
        var error = process.StandardError.ReadToEnd();
        if (!process.WaitForExit(15000))
        {
            process.Kill(entireProcessTree: true);
            throw new TimeoutException($"sc.exe {string.Join(' ', args)} timed out.");
        }
        if (!allowFailure && process.ExitCode != 0)
        {
            throw new InvalidOperationException($"sc.exe {string.Join(' ', args)} failed: {output} {error}");
        }
    }

    private static void RunSc(
        string first,
        string second,
        string third,
        string fourth,
        string fifth,
        string sixth,
        string seventh,
        string eighth,
        string ninth,
        string tenth) =>
        RunSc([first, second, third, fourth, fifth, sixth, seventh, eighth, ninth, tenth], false);

    private static void RunSc(string first, string second, string third) =>
        RunSc([first, second, third], false);

    private static void TryWriteInstallLog(string message)
    {
        try
        {
            var root = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.CommonApplicationData),
                "TaskbarWidgets");
            Directory.CreateDirectory(root);
            File.AppendAllText(
                Path.Combine(root, "voice-helper-install.log"),
                $"{DateTimeOffset.Now:O} {message}{Environment.NewLine}");
        }
        catch
        {
        }
    }
}

internal static class NativeService
{
    private const uint ServiceWin32OwnProcess = 0x10;
    private const uint ServiceStartPending = 2;
    private const uint ServiceStopPending = 3;
    private const uint ServiceRunning = 4;
    private const uint ServiceStopped = 1;
    private const uint ServiceAcceptStop = 1;
    private const uint ServiceControlStop = 1;
    private static readonly CancellationTokenSource Cancellation = new();
    private static readonly ServiceMainDelegate ServiceMainHandler = ServiceMain;
    private static readonly HandlerExDelegate ControlHandler = HandleControl;
    private static IntPtr _statusHandle;

    public static void Run()
    {
        var table = new[]
        {
            new ServiceTableEntry
            {
                ServiceName = Program.ServiceName,
                ServiceProc = Marshal.GetFunctionPointerForDelegate(ServiceMainHandler)
            },
            new ServiceTableEntry()
        };
        if (!StartServiceCtrlDispatcher(table))
        {
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
    }

    private static void ServiceMain(int argumentCount, IntPtr arguments)
    {
        _statusHandle = RegisterServiceCtrlHandlerEx(Program.ServiceName, ControlHandler, IntPtr.Zero);
        if (_statusHandle == IntPtr.Zero)
        {
            return;
        }
        SetStatus(ServiceStartPending, 0, 3000);
        SetStatus(ServiceRunning, ServiceAcceptStop, 0);
        uint exitCode = 0;
        try
        {
            VoiceCaptureRuntime.RunAsync(Cancellation.Token).GetAwaiter().GetResult();
        }
        catch (OperationCanceledException)
        {
        }
        catch
        {
            exitCode = 1;
        }
        SetStatus(ServiceStopped, 0, 0, exitCode);
    }

    private static uint HandleControl(uint control, uint eventType, IntPtr eventData, IntPtr context)
    {
        if (control == ServiceControlStop)
        {
            SetStatus(ServiceStopPending, 0, 3000);
            Cancellation.Cancel();
        }
        return 0;
    }

    private static void SetStatus(uint state, uint accepted, uint waitHint, uint exitCode = 0)
    {
        var status = new ServiceStatus
        {
            ServiceType = ServiceWin32OwnProcess,
            CurrentState = state,
            ControlsAccepted = accepted,
            Win32ExitCode = exitCode,
            WaitHint = waitHint
        };
        SetServiceStatus(_statusHandle, ref status);
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct ServiceTableEntry
    {
        [MarshalAs(UnmanagedType.LPWStr)] public string? ServiceName;
        public IntPtr ServiceProc;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct ServiceStatus
    {
        public uint ServiceType;
        public uint CurrentState;
        public uint ControlsAccepted;
        public uint Win32ExitCode;
        public uint ServiceSpecificExitCode;
        public uint CheckPoint;
        public uint WaitHint;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void ServiceMainDelegate(int argumentCount, IntPtr arguments);

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate uint HandlerExDelegate(uint control, uint eventType, IntPtr eventData, IntPtr context);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool StartServiceCtrlDispatcher([In] ServiceTableEntry[] serviceTable);

    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr RegisterServiceCtrlHandlerEx(
        string serviceName,
        HandlerExDelegate callback,
        IntPtr context);

    [DllImport("advapi32.dll", SetLastError = true)]
    private static extern bool SetServiceStatus(IntPtr statusHandle, ref ServiceStatus serviceStatus);
}

internal static class VoiceCaptureRuntime
{
    private const int AfInet = 2;
    private const int UdpTableOwnerPid = 1;
    private static readonly Channel<string> Events = Channel.CreateUnbounded<string>(new()
    {
        SingleReader = true,
        SingleWriter = false
    });
    private static readonly ConcurrentDictionary<VoiceKey, VoiceState> States = new();
    private static HashSet<int> _discordPorts = [];

    public static async Task RunAsync(CancellationToken serviceToken)
    {
        using var session = CancellationTokenSource.CreateLinkedTokenSource(serviceToken);
        await using var pipe = new NamedPipeClientStream(
            ".",
            Program.PipeName,
            PipeDirection.Out,
            PipeOptions.Asynchronous);
        await pipe.ConnectAsync(60000, session.Token);
        await using var writer = new StreamWriter(pipe) { AutoFlush = true };

        RefreshDiscordPorts();
        var sockets = CreateCaptureSockets();
        if (sockets.Count == 0)
        {
            throw new InvalidOperationException("No active IPv4 interface supports packet capture.");
        }

        var captureTasks = sockets
            .Select(socket => Task.Run(() => CaptureLoop(socket, session.Token), session.Token))
            .ToList();
        var monitorTask = Task.Run(() => MonitorLoopAsync(session.Token), session.Token);
        var metadataTask = Task.Run(() => MetadataLoopAsync(session.Token), session.Token);
        try
        {
            while (!session.IsCancellationRequested)
            {
                var line = await Events.Reader.ReadAsync(session.Token);
                await writer.WriteLineAsync(line);
            }
        }
        catch (IOException)
        {
            session.Cancel();
        }
        finally
        {
            session.Cancel();
            foreach (var socket in sockets)
            {
                try
                {
                    socket.IOControl(IOControlCode.ReceiveAll, BitConverter.GetBytes(0), null);
                }
                catch
                {
                }
                socket.Dispose();
            }
            try
            {
                await Task.WhenAll(captureTasks.Append(monitorTask).Append(metadataTask));
            }
            catch
            {
            }
        }
    }

    private static List<Socket> CreateCaptureSockets()
    {
        var sockets = new List<Socket>();
        var addresses = NetworkInterface.GetAllNetworkInterfaces()
            .Where(item => item.OperationalStatus == OperationalStatus.Up)
            .SelectMany(item => item.GetIPProperties().UnicastAddresses)
            .Select(item => item.Address)
            .Where(address => address.AddressFamily == AddressFamily.InterNetwork &&
                              !IPAddress.IsLoopback(address))
            .Distinct();
        foreach (var address in addresses)
        {
            try
            {
                var socket = new Socket(AddressFamily.InterNetwork, SocketType.Raw, ProtocolType.IP);
                socket.Bind(new IPEndPoint(address, 0));
                socket.ReceiveTimeout = 100;
                socket.IOControl(IOControlCode.ReceiveAll, BitConverter.GetBytes(1), null);
                sockets.Add(socket);
            }
            catch
            {
                // VPN and virtual adapters can reject raw capture independently.
            }
        }
        return sockets;
    }

    private static void CaptureLoop(Socket socket, CancellationToken cancellationToken)
    {
        var buffer = new byte[65535];
        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var length = socket.Receive(buffer, 0, buffer.Length, SocketFlags.None);
                if (TryParse(buffer, length, out var key))
                {
                    var state = States.GetOrAdd(key, _ => new VoiceState());
                    var start = false;
                    lock (state)
                    {
                        state.LastPacketTick = Environment.TickCount64;
                        state.Packets++;
                        if (!state.Active)
                        {
                            state.Active = true;
                            start = true;
                        }
                    }
                    if (start)
                    {
                        Events.Writer.TryWrite(SerializeEvent(key, true));
                    }
                }
            }
            catch (SocketException ex) when (ex.SocketErrorCode == SocketError.TimedOut)
            {
            }
            catch (ObjectDisposedException)
            {
                return;
            }
        }
    }

    private static async Task MonitorLoopAsync(CancellationToken cancellationToken)
    {
        var nextHeartbeat = 0L;
        while (!cancellationToken.IsCancellationRequested)
        {
            var now = Environment.TickCount64;
            foreach (var item in States)
            {
                var stop = false;
                lock (item.Value)
                {
                    if (item.Value.Active && now - item.Value.LastPacketTick >= 350)
                    {
                        item.Value.Active = false;
                        stop = true;
                    }
                }
                if (stop)
                {
                    Events.Writer.TryWrite(SerializeEvent(item.Key, false));
                }
            }
            if (now >= nextHeartbeat)
            {
                nextHeartbeat = now + 2000;
                Events.Writer.TryWrite(JsonSerializer.Serialize(new
                {
                    type = "heartbeat",
                    unixMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
                }));
            }
            await Task.Delay(100, cancellationToken);
        }
    }

    private static async Task MetadataLoopAsync(CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            RefreshDiscordPorts();
            await Task.Delay(1000, cancellationToken);
        }
    }

    private static bool TryParse(byte[] buffer, int length, out VoiceKey key)
    {
        key = default;
        if (length < 40 || buffer[0] >> 4 != 4 || buffer[9] != 17)
        {
            return false;
        }
        var ipLength = (buffer[0] & 0x0F) * 4;
        if (ipLength < 20 || length < ipLength + 20)
        {
            return false;
        }
        var sourcePort = ReadUInt16(buffer, ipLength);
        var destinationPort = ReadUInt16(buffer, ipLength + 2);
        var ports = Volatile.Read(ref _discordPorts);
        var direction = ports.Contains(destinationPort)
            ? "in"
            : ports.Contains(sourcePort)
                ? "out"
                : null;
        if (direction is null)
        {
            return false;
        }
        var rtp = ipLength + 8;
        if (buffer[rtp] >> 6 != 2 || (buffer[rtp + 1] & 0x7F) != 120)
        {
            return false;
        }
        key = new VoiceKey(ReadUInt32(buffer, rtp + 8), direction);
        return key.Ssrc != 0;
    }

    private static ushort ReadUInt16(byte[] buffer, int offset) =>
        (ushort)((buffer[offset] << 8) | buffer[offset + 1]);

    private static uint ReadUInt32(byte[] buffer, int offset) =>
        ((uint)buffer[offset] << 24) |
        ((uint)buffer[offset + 1] << 16) |
        ((uint)buffer[offset + 2] << 8) |
        buffer[offset + 3];

    private static string SerializeEvent(VoiceKey key, bool active) => JsonSerializer.Serialize(new
    {
        type = "speaking",
        ssrc = key.Ssrc,
        direction = key.Direction,
        active,
        unixMs = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()
    });

    private static void RefreshDiscordPorts()
    {
        var processIds = new HashSet<int>();
        foreach (var process in Process.GetProcesses())
        {
            using (process)
            {
                if (process.ProcessName.StartsWith("Discord", StringComparison.OrdinalIgnoreCase))
                {
                    processIds.Add(process.Id);
                }
            }
        }
        var ports = ReadUdpPorts(processIds);
        Volatile.Write(ref _discordPorts, ports);
    }

    private static HashSet<int> ReadUdpPorts(HashSet<int> processIds)
    {
        var size = 0;
        GetExtendedUdpTable(IntPtr.Zero, ref size, false, AfInet, UdpTableOwnerPid, 0);
        if (size <= 0)
        {
            return [];
        }
        var memory = Marshal.AllocHGlobal(size);
        try
        {
            if (GetExtendedUdpTable(memory, ref size, false, AfInet, UdpTableOwnerPid, 0) != 0)
            {
                return [];
            }
            var count = Marshal.ReadInt32(memory);
            var rowSize = Marshal.SizeOf<UdpRowOwnerPid>();
            var ports = new HashSet<int>();
            for (var index = 0; index < count; index++)
            {
                var row = Marshal.PtrToStructure<UdpRowOwnerPid>(
                    memory + sizeof(int) + index * rowSize);
                if (!processIds.Contains(checked((int)row.OwningPid)))
                {
                    continue;
                }
                var bytes = BitConverter.GetBytes(row.LocalPort);
                var port = (bytes[0] << 8) | bytes[1];
                if (port > 0)
                {
                    ports.Add(port);
                }
            }
            return ports;
        }
        finally
        {
            Marshal.FreeHGlobal(memory);
        }
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct UdpRowOwnerPid
    {
        public uint LocalAddress;
        public uint LocalPort;
        public uint OwningPid;
    }

    private readonly record struct VoiceKey(uint Ssrc, string Direction);

    private sealed class VoiceState
    {
        public bool Active;
        public long LastPacketTick;
        public long Packets;
    }

    [DllImport("iphlpapi.dll", SetLastError = true)]
    private static extern uint GetExtendedUdpTable(
        IntPtr table,
        ref int size,
        bool order,
        int addressFamily,
        int tableClass,
        uint reserved);
}
