using System.Diagnostics;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Text.Json;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.Loader;

internal static class SystemMetricsWorker
{
    private static readonly WidgetStateStore StateStore = new();
    private static readonly SystemMetricsSampler Sampler = new();

    public static async Task RunAsync(string widgetId, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var settings = ReadSettings(widgetId);
            try
            {
                var sample = Sampler.GetSample(settings.RefreshSeconds);
                object data = widgetId switch
                {
                    "system-cpu" => sample.Cpu,
                    "system-storage" => SelectStorage(sample.Storage, settings.SourceId),
                    "system-network" => SelectNetwork(sample.Network, settings.SourceId),
                    "system-memory" => sample.Memory,
                    _ => throw new InvalidOperationException($"Unsupported system widget: {widgetId}")
                };
                StateStore.Write(widgetId, data);
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                throw;
            }
            catch (Exception ex)
            {
                StateStore.Write(widgetId, new { loaded = false }, "error", ex.Message);
            }

            await Task.Delay(TimeSpan.FromSeconds(settings.RefreshSeconds), cancellationToken);
        }
    }

    private static StorageMetric SelectStorage(StorageMetric source, string? requested)
    {
        var selected = string.IsNullOrWhiteSpace(requested) ? "_Total" : requested;
        var disk = source.Disks.FirstOrDefault(item => string.Equals(item.Id, selected, StringComparison.OrdinalIgnoreCase))
                   ?? source.Disks.FirstOrDefault(item => item.Id == "_Total")
                   ?? new StorageDeviceMetric("_Total", "All disks", 0, 0, 0, 0);
        return source with
        {
            DiskId = disk.Id,
            ReadBytesPerSecond = disk.ReadBytesPerSecond,
            WriteBytesPerSecond = disk.WriteBytesPerSecond,
            ReadPercent = disk.ReadPercent,
            WritePercent = disk.WritePercent
        };
    }

    private static NetworkMetric SelectNetwork(NetworkMetric source, string? requested)
    {
        var requestedId = string.IsNullOrWhiteSpace(requested) ||
                          string.Equals(requested, "auto", StringComparison.OrdinalIgnoreCase)
            ? "all"
            : requested;
        if (string.Equals(requestedId, "all", StringComparison.OrdinalIgnoreCase))
        {
            return source with
            {
                InterfaceId = "all",
                InterfaceName = "All interfaces",
                ReceiveBytesPerSecond = source.Interfaces.Sum(item => item.ReceiveBytesPerSecond),
                SendBytesPerSecond = source.Interfaces.Sum(item => item.SendBytesPerSecond),
                ReceiveLinkSpeedBitsPerSecond = SumSaturating(source.Interfaces.Select(item => item.ReceiveLinkSpeedBitsPerSecond)),
                SendLinkSpeedBitsPerSecond = SumSaturating(source.Interfaces.Select(item => item.SendLinkSpeedBitsPerSecond))
            };
        }

        var selected = source.Interfaces.FirstOrDefault(item =>
            string.Equals(item.Id, requestedId, StringComparison.OrdinalIgnoreCase));
        if (selected is null)
        {
            return SelectNetwork(source, "all");
        }
        return source with
        {
            InterfaceId = selected.Id,
            InterfaceName = selected.Name,
            ReceiveBytesPerSecond = selected.ReceiveBytesPerSecond,
            SendBytesPerSecond = selected.SendBytesPerSecond,
            ReceiveLinkSpeedBitsPerSecond = selected.ReceiveLinkSpeedBitsPerSecond,
            SendLinkSpeedBitsPerSecond = selected.SendLinkSpeedBitsPerSecond
        };
    }

    private static ulong SumSaturating(IEnumerable<ulong> values)
    {
        ulong total = 0;
        foreach (var value in values)
        {
            total = ulong.MaxValue - total < value ? ulong.MaxValue : total + value;
        }
        return total;
    }

    private static SystemWidgetSettings ReadSettings(string widgetId)
    {
        try
        {
            var path = Path.Combine(AppPaths.AppDirectory, "config.json");
            if (!File.Exists(path)) return new SystemWidgetSettings(3, null);
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            if (!document.RootElement.TryGetProperty("widgets", out var widgets)) return new SystemWidgetSettings(1, null);
            foreach (var widget in widgets.EnumerateArray())
            {
                if (widget.TryGetProperty("id", out var id) && id.GetString() == widgetId &&
                    widget.TryGetProperty("settings", out var settings))
                {
                    var refresh = 3d;
                    if (settings.TryGetProperty("refreshSeconds", out var refreshValue))
                    {
                        if (refreshValue.ValueKind == JsonValueKind.Number) refreshValue.TryGetDouble(out refresh);
                        else if (refreshValue.ValueKind == JsonValueKind.String) double.TryParse(refreshValue.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out refresh);
                    }
                    var sourceKey = widgetId == "system-storage" ? "diskId" : "interfaceId";
                    var source = settings.TryGetProperty(sourceKey, out var sourceValue) ? sourceValue.GetString() : null;
                    return new SystemWidgetSettings(SystemMetricMath.NormalizeRefreshSeconds(refresh), source);
                }
            }
        }
        catch
        {
            // A partial settings write must not stop system monitoring.
        }
        return new SystemWidgetSettings(3, null);
    }

    private sealed record SystemWidgetSettings(double RefreshSeconds, string? SourceId);
}

internal sealed class SystemMetricsSampler
{
    private readonly object _gate = new();
    private readonly PdhSampler _pdh = new();
    private readonly Dictionary<string, NetworkBaseline> _networkBaselines = new(StringComparer.OrdinalIgnoreCase);
    private readonly Stopwatch _clock = Stopwatch.StartNew();
    private double _lastNetworkSeconds;
    private long _lastSampleTicks;
    private SystemMetricsSample? _cached;

    public SystemMetricsSample GetSample(double requestedRefreshSeconds)
    {
        lock (_gate)
        {
            var nowTicks = Stopwatch.GetTimestamp();
            var cacheMilliseconds = Math.Clamp(requestedRefreshSeconds * 500d, 50d, 250d);
            if (_cached is not null && Stopwatch.GetElapsedTime(_lastSampleTicks, nowTicks) < TimeSpan.FromMilliseconds(cacheMilliseconds))
            {
                return _cached;
            }

            var counters = _pdh.ReadSystemCounters();
            _cached = new SystemMetricsSample(counters.Cpu, counters.Storage, ReadNetwork(), ReadMemory());
            _lastSampleTicks = nowTicks;
            return _cached;
        }
    }

    private NetworkMetric ReadNetwork()
    {
        var now = _clock.Elapsed.TotalSeconds;
        var elapsed = now - _lastNetworkSeconds;
        _lastNetworkSeconds = now;
        var currentIds = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var interfaces = new List<NetworkInterfaceMetric>();

        foreach (var adapter in WindowsNetworkTable.Read())
        {
            var id = adapter.Id;
            currentIds.Add(id);
            var receiveRate = 0d;
            var sendRate = 0d;
            if (_networkBaselines.TryGetValue(id, out var previous))
            {
                receiveRate = SystemMetricMath.ComputeRate(previous.Received, adapter.Received, elapsed);
                sendRate = SystemMetricMath.ComputeRate(previous.Sent, adapter.Sent, elapsed);
            }
            _networkBaselines[id] = new NetworkBaseline(adapter.Received, adapter.Sent);
            interfaces.Add(new NetworkInterfaceMetric(
                id, adapter.Name, receiveRate, sendRate,
                adapter.ReceiveLinkSpeedBitsPerSecond,
                adapter.SendLinkSpeedBitsPerSecond));
        }

        foreach (var stale in _networkBaselines.Keys.Where(id => !currentIds.Contains(id)).ToList()) _networkBaselines.Remove(stale);
        return new NetworkMetric("all", "All interfaces", 0, 0, 0, 0, interfaces);
    }

    private static MemoryMetric ReadMemory()
    {
        var status = new MemoryStatus { Length = (uint)Marshal.SizeOf<MemoryStatus>() };
        if (!GlobalMemoryStatusEx(ref status)) throw new InvalidOperationException("GlobalMemoryStatusEx failed.");
        var used = status.TotalPhysical >= status.AvailablePhysical ? status.TotalPhysical - status.AvailablePhysical : 0;
        return new MemoryMetric(used, status.AvailablePhysical, status.TotalPhysical,
            status.TotalPhysical == 0 ? 0 : SystemMetricMath.ClampPercent(used * 100d / status.TotalPhysical));
    }

    private sealed record NetworkBaseline(ulong Received, ulong Sent);

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MemoryStatus
    {
        public uint Length;
        public uint MemoryLoad;
        public ulong TotalPhysical;
        public ulong AvailablePhysical;
        public ulong TotalPageFile;
        public ulong AvailablePageFile;
        public ulong TotalVirtual;
        public ulong AvailableVirtual;
        public ulong AvailableExtendedVirtual;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static extern bool GlobalMemoryStatusEx(ref MemoryStatus buffer);
}

internal static class WindowsNetworkTable
{
    public static IReadOnlyList<NetworkCounterRow> Read()
    {
        var status = GetIfTable2(out var table);
        if (status != 0 || table == IntPtr.Zero)
        {
            throw new InvalidOperationException($"GetIfTable2 failed: {status}");
        }

        try
        {
            var count = Marshal.ReadInt32(table);
            var rowOffset = Marshal.OffsetOf<MibIfTable2Header>(nameof(MibIfTable2Header.FirstRow)).ToInt32();
            var rowSize = Marshal.SizeOf<MibIfRow2>();
            var result = new List<NetworkCounterRow>(Math.Max(0, count));
            for (var index = 0; index < count; index++)
            {
                var row = Marshal.PtrToStructure<MibIfRow2>(IntPtr.Add(table, rowOffset + index * rowSize));
                if (row.OperStatus != 1 || row.Type == 24) continue; // Up; exclude software loopback.
                var name = string.IsNullOrWhiteSpace(row.Alias) ? row.Description : row.Alias;
                result.Add(new NetworkCounterRow(
                    row.InterfaceGuid.ToString("D", CultureInfo.InvariantCulture),
                    string.IsNullOrWhiteSpace(name) ? $"Interface {row.InterfaceIndex}" : name,
                    row.InOctets,
                    row.OutOctets,
                    row.ReceiveLinkSpeed,
                    row.TransmitLinkSpeed));
            }
            return result;
        }
        finally
        {
            FreeMibTable(table);
        }
    }

    internal sealed record NetworkCounterRow(
        string Id,
        string Name,
        ulong Received,
        ulong Sent,
        ulong ReceiveLinkSpeedBitsPerSecond,
        ulong SendLinkSpeedBitsPerSecond);

    [StructLayout(LayoutKind.Sequential)]
    private struct MibIfTable2Header
    {
        public uint NumEntries;
        public MibIfRow2 FirstRow;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct MibIfRow2
    {
        public ulong InterfaceLuid;
        public uint InterfaceIndex;
        public Guid InterfaceGuid;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 257)] public string Alias;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 257)] public string Description;
        public uint PhysicalAddressLength;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] PhysicalAddress;
        [MarshalAs(UnmanagedType.ByValArray, SizeConst = 32)] public byte[] PermanentPhysicalAddress;
        public uint Mtu;
        public uint Type;
        public uint TunnelType;
        public uint MediaType;
        public uint PhysicalMediumType;
        public uint AccessType;
        public uint DirectionType;
        public byte InterfaceAndOperStatusFlags;
        public uint OperStatus;
        public uint AdminStatus;
        public uint MediaConnectState;
        public Guid NetworkGuid;
        public uint ConnectionType;
        public ulong TransmitLinkSpeed;
        public ulong ReceiveLinkSpeed;
        public ulong InOctets;
        public ulong InUcastPkts;
        public ulong InNUcastPkts;
        public ulong InDiscards;
        public ulong InErrors;
        public ulong InUnknownProtos;
        public ulong InUcastOctets;
        public ulong InMulticastOctets;
        public ulong InBroadcastOctets;
        public ulong OutOctets;
        public ulong OutUcastPkts;
        public ulong OutNUcastPkts;
        public ulong OutDiscards;
        public ulong OutErrors;
        public ulong OutUcastOctets;
        public ulong OutMulticastOctets;
        public ulong OutBroadcastOctets;
        public ulong OutQLen;
    }

    [DllImport("iphlpapi.dll")]
    private static extern uint GetIfTable2(out IntPtr table);

    [DllImport("iphlpapi.dll")]
    private static extern void FreeMibTable(IntPtr memory);
}

internal sealed class PdhSampler : IDisposable
{
    private const uint PdhFormatDouble = 0x00000200;
    private const uint PdhMoreData = 0x800007D2;
    private readonly IntPtr _query;
    private readonly IntPtr _cpuTotal;
    private readonly IntPtr _cpuUser;
    private readonly IntPtr _cpuKernel;
    private readonly IntPtr _cores;
    private readonly IntPtr _coresUser;
    private readonly IntPtr _coresKernel;
    private readonly IntPtr _diskReads;
    private readonly IntPtr _diskWrites;
    private readonly IntPtr _diskReadPercent;
    private readonly IntPtr _diskWritePercent;

    public PdhSampler()
    {
        ThrowIfError(PdhOpenQueryW(null, IntPtr.Zero, out _query), "PdhOpenQuery");
        _cpuTotal = AddCounter(@"\Processor(_Total)\% Processor Time");
        _cpuUser = AddCounter(@"\Processor(_Total)\% User Time");
        _cpuKernel = AddCounter(@"\Processor(_Total)\% Privileged Time");
        _cores = AddCounter(@"\Processor(*)\% Processor Time");
        _coresUser = AddCounter(@"\Processor(*)\% User Time");
        _coresKernel = AddCounter(@"\Processor(*)\% Privileged Time");
        _diskReads = AddCounter(@"\PhysicalDisk(*)\Disk Read Bytes/sec");
        _diskWrites = AddCounter(@"\PhysicalDisk(*)\Disk Write Bytes/sec");
        _diskReadPercent = AddCounter(@"\PhysicalDisk(*)\% Disk Read Time");
        _diskWritePercent = AddCounter(@"\PhysicalDisk(*)\% Disk Write Time");
        PdhCollectQueryData(_query);
    }

    public (CpuMetric Cpu, StorageMetric Storage) ReadSystemCounters()
    {
        Collect();
        var coreTotals = ReadArray(_cores);
        var coreUsers = ReadArray(_coresUser);
        var coreKernels = ReadArray(_coresKernel);
        var cores = coreTotals
            .Where(item => item.Key != "_Total")
            .OrderBy(item => NaturalKey(item.Key))
            .Select(item => new CpuCoreMetric(
                item.Key,
                SystemMetricMath.ClampPercent(item.Value),
                coreUsers.TryGetValue(item.Key, out var user) ? SystemMetricMath.ClampPercent(user) : 0,
                coreKernels.TryGetValue(item.Key, out var kernel) ? SystemMetricMath.ClampPercent(kernel) : 0))
            .ToList();
        var cpu = new CpuMetric(Percent(_cpuTotal), Percent(_cpuUser), Percent(_cpuKernel), cores);
        var diskReads = ReadArray(_diskReads);
        var diskWrites = ReadArray(_diskWrites);
        var diskReadPercent = ReadArray(_diskReadPercent);
        var diskWritePercent = ReadArray(_diskWritePercent);
        var ids = diskReads.Keys.Concat(diskWrites.Keys).Concat(diskReadPercent.Keys).Concat(diskWritePercent.Keys).Distinct(StringComparer.OrdinalIgnoreCase)
            .OrderBy(id => id == "_Total" ? "" : NaturalKey(id));
        var disks = ids.Select(id => new StorageDeviceMetric(
            id,
            id == "_Total" ? "All disks" : id,
            diskReads.TryGetValue(id, out var read) ? NonNegative(read) : 0,
            diskWrites.TryGetValue(id, out var write) ? NonNegative(write) : 0,
            diskReadPercent.TryGetValue(id, out var readActivity) ? SystemMetricMath.ClampPercent(readActivity) : 0,
            diskWritePercent.TryGetValue(id, out var writeActivity) ? SystemMetricMath.ClampPercent(writeActivity) : 0)).ToList();
        return (cpu, new StorageMetric("_Total", 0, 0, 0, 0, disks));
    }

    private void Collect()
    {
        var status = PdhCollectQueryData(_query);
        if (status != 0) throw new InvalidOperationException($"PDH collection failed: 0x{status:X8}");
    }

    private double Percent(IntPtr counter) => SystemMetricMath.ClampPercent(Read(counter));
    private static double NonNegative(double value)
    {
        return double.IsFinite(value) && value > 0 ? Math.Round(value, 1) : 0;
    }

    private double Read(IntPtr counter)
    {
        var status = PdhGetFormattedCounterValue(counter, PdhFormatDouble, out _, out var value);
        return status == 0 && value.CStatus == 0 ? value.DoubleValue : 0;
    }

    private IntPtr AddCounter(string path)
    {
        ThrowIfError(PdhAddEnglishCounterW(_query, path, IntPtr.Zero, out var counter), $"PdhAddEnglishCounter {path}");
        return counter;
    }

    private static Dictionary<string, double> ReadArray(IntPtr counter)
    {
        var bufferSize = 0u;
        var itemCount = 0u;
        var status = PdhGetFormattedCounterArrayW(counter, PdhFormatDouble, ref bufferSize, ref itemCount, IntPtr.Zero);
        if (status != PdhMoreData || bufferSize == 0) return new(StringComparer.OrdinalIgnoreCase);
        var buffer = Marshal.AllocHGlobal(checked((int)bufferSize));
        try
        {
            status = PdhGetFormattedCounterArrayW(counter, PdhFormatDouble, ref bufferSize, ref itemCount, buffer);
            if (status != 0) return new(StringComparer.OrdinalIgnoreCase);
            var result = new Dictionary<string, double>(StringComparer.OrdinalIgnoreCase);
            var itemSize = Marshal.SizeOf<PdhFormattedValueItem>();
            for (var index = 0; index < itemCount; index++)
            {
                var item = Marshal.PtrToStructure<PdhFormattedValueItem>(IntPtr.Add(buffer, checked((int)index * itemSize)));
                var name = Marshal.PtrToStringUni(item.Name);
                if (!string.IsNullOrWhiteSpace(name) && item.Value.CStatus == 0)
                {
                    result[name] = item.Value.DoubleValue;
                }
            }
            return result;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    private static string NaturalKey(string value) => value.PadLeft(16, '0');
    private static void ThrowIfError(uint status, string operation)
    {
        if (status != 0) throw new InvalidOperationException($"{operation} failed: 0x{status:X8}");
    }

    public void Dispose()
    {
        if (_query != IntPtr.Zero) PdhCloseQuery(_query);
    }

    [StructLayout(LayoutKind.Explicit)]
    private struct PdhFormattedValue
    {
        [FieldOffset(0)] public uint CStatus;
        [FieldOffset(8)] public double DoubleValue;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PdhFormattedValueItem
    {
        public IntPtr Name;
        public PdhFormattedValue Value;
    }

    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhOpenQueryW(string? dataSource, IntPtr userData, out IntPtr query);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhAddEnglishCounterW(IntPtr query, string path, IntPtr userData, out IntPtr counter);
    [DllImport("pdh.dll")]
    private static extern uint PdhCollectQueryData(IntPtr query);
    [DllImport("pdh.dll")]
    private static extern uint PdhGetFormattedCounterValue(IntPtr counter, uint format, out uint type, out PdhFormattedValue value);
    [DllImport("pdh.dll", CharSet = CharSet.Unicode)]
    private static extern uint PdhGetFormattedCounterArrayW(IntPtr counter, uint format, ref uint bufferSize, ref uint itemCount, IntPtr itemBuffer);
    [DllImport("pdh.dll")]
    private static extern uint PdhCloseQuery(IntPtr query);
}

internal sealed record SystemMetricsSample(CpuMetric Cpu, StorageMetric Storage, NetworkMetric Network, MemoryMetric Memory);
internal sealed record CpuMetric(double TotalPercent, double UserPercent, double KernelPercent, IReadOnlyList<CpuCoreMetric> Cores);
internal sealed record CpuCoreMetric(string Id, double Percent, double UserPercent, double KernelPercent);
internal sealed record StorageMetric(string DiskId, double ReadBytesPerSecond, double WriteBytesPerSecond, double ReadPercent, double WritePercent, IReadOnlyList<StorageDeviceMetric> Disks)
{
    public IReadOnlyList<MetricSourceOption> AvailableDisks => Disks.Select(item => new MetricSourceOption(item.Id, item.Name)).ToList();
}
internal sealed record StorageDeviceMetric(string Id, string Name, double ReadBytesPerSecond, double WriteBytesPerSecond, double ReadPercent, double WritePercent);
internal sealed record NetworkMetric(string InterfaceId, string InterfaceName, double ReceiveBytesPerSecond, double SendBytesPerSecond, ulong ReceiveLinkSpeedBitsPerSecond, ulong SendLinkSpeedBitsPerSecond, IReadOnlyList<NetworkInterfaceMetric> Interfaces)
{
    public IReadOnlyList<MetricSourceOption> AvailableInterfaces => new[] { new MetricSourceOption("all", "All interfaces") }
        .Concat(Interfaces.Select(item => new MetricSourceOption(item.Id, item.Name))).ToList();
}
internal sealed record NetworkInterfaceMetric(string Id, string Name, double ReceiveBytesPerSecond, double SendBytesPerSecond, ulong ReceiveLinkSpeedBitsPerSecond, ulong SendLinkSpeedBitsPerSecond);
internal sealed record MemoryMetric(ulong UsedBytes, ulong AvailableBytes, ulong TotalBytes, double UsedPercent);
internal sealed record MetricSourceOption(string Id, string Name);
