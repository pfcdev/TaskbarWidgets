$ErrorActionPreference = "Stop"
[Console]::InputEncoding = [Text.Encoding]::UTF8
[Console]::OutputEncoding = [Text.Encoding]::UTF8

# This provider deliberately avoids WMI/CIM, Get-Counter and per-process CPU
# inspection. The original technology demo sampled all of them every two
# seconds, which could overwhelm some Windows installations.
Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.IO;
using System.Net.NetworkInformation;
using System.Runtime.InteropServices;
using System.Threading;

public sealed class NeonSystemSample
{
    public double CpuPercent { get; set; }
    public ulong MemoryTotalBytes { get; set; }
    public ulong MemoryAvailableBytes { get; set; }
    public long DiskTotalBytes { get; set; }
    public long DiskFreeBytes { get; set; }
    public long NetworkReceivedBytes { get; set; }
    public long NetworkSentBytes { get; set; }
    public int ActiveAdapters { get; set; }
    public int ProcessCount { get; set; }
    public long UptimeMilliseconds { get; set; }
    public bool Healthy { get; set; }
}

public static class NeonInputReader
{
    private static readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    private static int started;
    private static volatile bool ended;

    public static bool Ended
    {
        get { return ended; }
    }

    public static void Start()
    {
        if (Interlocked.Exchange(ref started, 1) != 0)
        {
            return;
        }
        Thread reader = new Thread(ReadLoop);
        reader.IsBackground = true;
        reader.Name = "Neon Sentinel protocol reader";
        reader.Start();
    }

    public static bool TryRead(out string line)
    {
        return Lines.TryDequeue(out line);
    }

    private static void ReadLoop()
    {
        try
        {
            string line;
            while ((line = Console.In.ReadLine()) != null)
            {
                Lines.Enqueue(line);
            }
        }
        finally
        {
            ended = true;
        }
    }
}

public static class NeonSystemSampler
{
    [StructLayout(LayoutKind.Sequential)]
    private struct FileTime
    {
        public uint Low;
        public uint High;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    private sealed class MemoryStatus
    {
        public uint Length = (uint)Marshal.SizeOf(typeof(MemoryStatus));
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
    private static extern bool GetSystemTimes(
        out FileTime idle,
        out FileTime kernel,
        out FileTime user);

    [DllImport("kernel32.dll", CharSet = CharSet.Auto, SetLastError = true)]
    private static extern bool GlobalMemoryStatusEx([In, Out] MemoryStatus status);

    [DllImport("kernel32.dll")]
    private static extern ulong GetTickCount64();

    private static readonly object Sync = new object();
    private static ulong previousIdle;
    private static ulong previousTotal;

    private static ulong ToUInt64(FileTime value)
    {
        return ((ulong)value.High << 32) | value.Low;
    }

    private static double ReadCpu()
    {
        FileTime idle;
        FileTime kernel;
        FileTime user;
        if (!GetSystemTimes(out idle, out kernel, out user))
        {
            return 0;
        }

        ulong idleNow = ToUInt64(idle);
        ulong totalNow = ToUInt64(kernel) + ToUInt64(user);
        lock (Sync)
        {
            double result = 0;
            if (previousTotal != 0 && totalNow > previousTotal)
            {
                ulong totalDelta = totalNow - previousTotal;
                ulong idleDelta = idleNow >= previousIdle ? idleNow - previousIdle : 0;
                result = totalDelta == 0
                    ? 0
                    : Math.Max(0, Math.Min(100, 100.0 * (totalDelta - Math.Min(idleDelta, totalDelta)) / totalDelta));
            }
            previousIdle = idleNow;
            previousTotal = totalNow;
            return result;
        }
    }

    public static NeonSystemSample Capture()
    {
        NeonSystemSample sample = new NeonSystemSample();
        sample.Healthy = true;
        sample.CpuPercent = ReadCpu();
        sample.UptimeMilliseconds = (long)Math.Min(GetTickCount64(), (ulong)long.MaxValue);

        MemoryStatus memory = new MemoryStatus();
        if (GlobalMemoryStatusEx(memory))
        {
            sample.MemoryTotalBytes = memory.TotalPhysical;
            sample.MemoryAvailableBytes = memory.AvailablePhysical;
        }
        else
        {
            sample.Healthy = false;
        }

        try
        {
            string systemDrive = Environment.GetEnvironmentVariable("SystemDrive") ?? "C:";
            DriveInfo drive = new DriveInfo(systemDrive + Path.DirectorySeparatorChar);
            if (drive.IsReady)
            {
                sample.DiskTotalBytes = drive.TotalSize;
                sample.DiskFreeBytes = drive.AvailableFreeSpace;
            }
        }
        catch
        {
            sample.Healthy = false;
        }

        try
        {
            foreach (NetworkInterface adapter in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (adapter.OperationalStatus != OperationalStatus.Up ||
                    adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                {
                    continue;
                }
                IPv4InterfaceStatistics statistics = adapter.GetIPv4Statistics();
                sample.NetworkReceivedBytes += statistics.BytesReceived;
                sample.NetworkSentBytes += statistics.BytesSent;
                sample.ActiveAdapters++;
            }
        }
        catch
        {
            sample.Healthy = false;
        }

        try
        {
            Process[] processes = Process.GetProcesses();
            sample.ProcessCount = processes.Length;
            foreach (Process process in processes)
            {
                process.Dispose();
            }
        }
        catch
        {
            sample.Healthy = false;
        }

        return sample;
    }
}
'@

$instances = @{}
$nextSample = [DateTimeOffset]::MinValue
$lastReport = $null
$shutdown = $false

function Get-SentinelSnapshot {
    $sample = [NeonSystemSampler]::Capture()
    $memoryUsed = [Math]::Max(
        [int64]0,
        [int64]($sample.MemoryTotalBytes - $sample.MemoryAvailableBytes))
    $memoryPercent = if ($sample.MemoryTotalBytes -gt 0) {
        ($memoryUsed / $sample.MemoryTotalBytes) * 100
    } else {
        0
    }
    $diskUsedPercent = if ($sample.DiskTotalBytes -gt 0) {
        (($sample.DiskTotalBytes - $sample.DiskFreeBytes) / $sample.DiskTotalBytes) * 100
    } else {
        0
    }
    $uptime = [TimeSpan]::FromMilliseconds($sample.UptimeMilliseconds)

    return [ordered]@{
        health = if ($sample.Healthy) { "nominal" } else { "degraded" }
        machineName = [Environment]::MachineName
        userName = [Environment]::UserName
        sampleTime = [DateTimeOffset]::Now.ToString("HH:mm:ss")
        samplingMode = "Win32 API + .NET / 5s"
        cpuPercent = [Math]::Round($sample.CpuPercent, 1)
        logicalProcessors = [Environment]::ProcessorCount
        memoryPercent = [Math]::Round($memoryPercent, 1)
        memoryUsedBytes = $memoryUsed
        memoryTotalBytes = [int64]$sample.MemoryTotalBytes
        diskUsedPercent = [Math]::Round($diskUsedPercent, 1)
        diskFreeBytes = $sample.DiskFreeBytes
        diskTotalBytes = $sample.DiskTotalBytes
        processCount = $sample.ProcessCount
        providerProcessName = [Diagnostics.Process]::GetCurrentProcess().ProcessName
        providerProcessId = $PID
        networkReceivedBytes = $sample.NetworkReceivedBytes
        networkSentBytes = $sample.NetworkSentBytes
        activeAdapters = $sample.ActiveAdapters
        uptimeText = "{0}d {1}h {2}m" -f [Math]::Floor($uptime.TotalDays), $uptime.Hours, $uptime.Minutes
    }
}

function Publish-Snapshot {
    $script:lastReport = Get-SentinelSnapshot
    foreach ($instanceId in @($script:instances.Keys)) {
        $json = [ordered]@{
            type = "snapshot"
            instanceId = $instanceId
            status = $script:lastReport.health
            data = $script:lastReport
        } | ConvertTo-Json -Compress -Depth 8
        [Console]::Out.WriteLine($json)
    }
    [Console]::Out.Flush()
}

function Update-Instances {
    param([object[]]$Values)
    $script:instances = @{}
    foreach ($instance in @($Values)) {
        if ($null -ne $instance.instanceId) {
            $script:instances[[string]$instance.instanceId] = $instance.settings
        }
    }
    $script:nextSample = [DateTimeOffset]::MinValue
}

function Invoke-SentinelAction {
    param([object]$Message)
    switch ([string]$Message.action) {
        "openTaskManager" {
            Start-Process taskmgr.exe
        }
        "openResourceMonitor" {
            Start-Process resmon.exe
        }
        "copyReport" {
            if ($null -ne $script:lastReport) {
                $report = $script:lastReport | ConvertTo-Json -Depth 8
                Set-Clipboard -Value $report
            }
        }
        "refresh" {
            $script:nextSample = [DateTimeOffset]::MinValue
        }
    }
}

[NeonInputReader]::Start()
while (-not $shutdown) {
    $line = $null
    while ([NeonInputReader]::TryRead([ref]$line)) {
        try {
            $message = $line | ConvertFrom-Json
            switch ([string]$message.type) {
                "initialize" { Update-Instances @($message.instances) }
                "instancesChanged" { Update-Instances @($message.instances) }
                "action" { Invoke-SentinelAction $message }
                "shutdown" { $shutdown = $true }
            }
        } catch {
            $json = [ordered]@{
                type = "log"
                message = "Input error: $($_.Exception.Message)"
            } | ConvertTo-Json -Compress
            [Console]::Out.WriteLine($json)
            [Console]::Out.Flush()
        }
    }

    if ([NeonInputReader]::Ended -and $null -eq $line) { break }

    if (-not $shutdown -and
        $instances.Count -gt 0 -and
        [DateTimeOffset]::Now -ge $nextSample) {
        try {
            Publish-Snapshot
        } catch {
            $json = [ordered]@{
                type = "log"
                message = "Telemetry error: $($_.Exception.Message)"
            } | ConvertTo-Json -Compress
            [Console]::Out.WriteLine($json)
            [Console]::Out.Flush()
        }
        $nextSample = [DateTimeOffset]::Now.AddSeconds(5)
    }

    Start-Sleep -Milliseconds 100
}
