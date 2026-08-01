$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = [Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)

Add-Type -AssemblyName System.Runtime.WindowsRuntime
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Concurrent;
using System.Runtime.InteropServices;
using System.Threading;
public static class CommunityMediaKeys {
    [DllImport("user32.dll")]
    private static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);
    public static void Toggle() {
        const byte MediaPlayPause = 0xB3;
        keybd_event(MediaPlayPause, 0, 0, UIntPtr.Zero);
        keybd_event(MediaPlayPause, 0, 0x0002, UIntPtr.Zero);
    }
}
public static class CommunityMediaInputReader {
    private static readonly ConcurrentQueue<string> Lines = new ConcurrentQueue<string>();
    private static int started;
    private static volatile bool ended;
    public static bool Ended { get { return ended; } }
    public static void Start() {
        if (Interlocked.Exchange(ref started, 1) != 0) return;
        Thread reader = new Thread(ReadLoop);
        reader.IsBackground = true;
        reader.Name = "Community Media protocol reader";
        reader.Start();
    }
    public static bool TryRead(out string line) { return Lines.TryDequeue(out line); }
    private static void ReadLoop() {
        try {
            string line;
            while ((line = Console.In.ReadLine()) != null) Lines.Enqueue(line);
        } finally {
            ended = true;
        }
    }
}
'@

$script:ManagerType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager, Windows.Media.Control, ContentType=WindowsRuntime]
$script:PropertiesType = [Windows.Media.Control.GlobalSystemMediaTransportControlsSessionMediaProperties, Windows.Media.Control, ContentType=WindowsRuntime]
$script:RandomAccessContentType = [Windows.Storage.Streams.IRandomAccessStreamWithContentType, Windows.Storage.Streams, ContentType=WindowsRuntime]
$script:RandomAccessType = [Windows.Storage.Streams.IRandomAccessStream, Windows.Storage.Streams, ContentType=WindowsRuntime]
$script:GenericAsTask = [System.WindowsRuntimeSystemExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsTask' -and $_.IsGenericMethod -and $_.GetParameters().Count -eq 1 } |
    Select-Object -First 1
$script:AsStreamForRead = [System.IO.WindowsRuntimeStreamExtensions].GetMethods() |
    Where-Object { $_.Name -eq 'AsStreamForRead' -and $_.GetParameters().Count -eq 2 } |
    Select-Object -First 1

$script:Manager = $null
$script:WidgetDirectory = $PSScriptRoot
$script:DataDirectory = $PSScriptRoot
$script:Instances = @{}
$script:LastSignatures = @{}
$script:LastGood = $null
$script:LastGoodAt = [datetime]::MinValue

function Await-WinRt {
    param($Operation, [Type] $ResultType)
    $task = $script:GenericAsTask.MakeGenericMethod($ResultType).Invoke($null, @($Operation))
    if (-not $task.Wait(5000)) {
        throw 'Windows media operation timed out.'
    }
    return $task.Result
}

function Convert-ToColorHex {
    param([int] $Alpha, [double] $Red, [double] $Green, [double] $Blue)
    $a = [Math]::Max(0, [Math]::Min(255, [int][Math]::Round($Alpha)))
    $r = [Math]::Max(0, [Math]::Min(255, [int][Math]::Round($Red)))
    $g = [Math]::Max(0, [Math]::Min(255, [int][Math]::Round($Green)))
    $b = [Math]::Max(0, [Math]::Min(255, [int][Math]::Round($Blue)))
    return '#{0:X2}{1:X2}{2:X2}{3:X2}' -f $a, $r, $g, $b
}

function Get-AdaptivePalette {
    param([string] $Path)
    $fallback = [ordered]@{
        Left = '#FF0F172A'
        Right = '#FF111827'
        Accent = '#FF22D3EE'
    }
    if ([string]::IsNullOrWhiteSpace($Path) -or -not [IO.File]::Exists($Path)) {
        return $fallback
    }

    $bitmap = $null
    try {
        $bitmap = [Drawing.Bitmap]::new($Path)
        if ($bitmap.Width -lt 2 -or $bitmap.Height -lt 2) {
            return $fallback
        }
        [double] $leftR = 0; [double] $leftG = 0; [double] $leftB = 0; [int] $leftCount = 0
        [double] $rightR = 0; [double] $rightG = 0; [double] $rightB = 0; [int] $rightCount = 0
        $stepX = [Math]::Max(1, [int]($bitmap.Width / 18))
        $stepY = [Math]::Max(1, [int]($bitmap.Height / 18))
        $edge = [Math]::Max(1, [int]($bitmap.Width / 4))
        for ($y = 0; $y -lt $bitmap.Height; $y += $stepY) {
            for ($x = 0; $x -lt $bitmap.Width; $x += $stepX) {
                if ($x -ge $edge -and $x -lt ($bitmap.Width - $edge)) { continue }
                $pixel = $bitmap.GetPixel($x, $y)
                if ($pixel.A -lt 128) { continue }
                if ($x -lt $edge) {
                    $leftR += $pixel.R; $leftG += $pixel.G; $leftB += $pixel.B; $leftCount++
                } else {
                    $rightR += $pixel.R; $rightG += $pixel.G; $rightB += $pixel.B; $rightCount++
                }
            }
        }
        if ($leftCount -eq 0 -or $rightCount -eq 0) { return $fallback }
        $lr = $leftR / $leftCount; $lg = $leftG / $leftCount; $lb = $leftB / $leftCount
        $rr = $rightR / $rightCount; $rg = $rightG / $rightCount; $rb = $rightB / $rightCount
        $ar = ($lr + $rr) / 2; $ag = ($lg + $rg) / 2; $ab = ($lb + $rb) / 2
        $maximum = [Math]::Max(1, [Math]::Max($ar, [Math]::Max($ag, $ab)))
        $boost = [Math]::Min(1.8, 220 / $maximum)
        return [ordered]@{
            Left = Convert-ToColorHex 255 (8 + $lr * 0.18) (12 + $lg * 0.18) (20 + $lb * 0.18)
            Right = Convert-ToColorHex 255 (10 + $rr * 0.22) (14 + $rg * 0.22) (22 + $rb * 0.22)
            Accent = Convert-ToColorHex 255 ($ar * $boost) ($ag * $boost) ($ab * $boost)
        }
    } catch {
        return $fallback
    } finally {
        if ($null -ne $bitmap) { $bitmap.Dispose() }
    }
}

function Get-StableHash {
    param([string] $Value)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [Text.Encoding]::UTF8.GetBytes($Value)
        $hex = ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '')
        return $hex.Substring(0, 20).ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Save-Thumbnail {
    param($Properties, [string] $MediaKey)
    if ($null -eq $Properties -or $null -eq $Properties.Thumbnail) { return $null }
    $cacheDirectory = Join-Path $script:DataDirectory 'CommunityMediaCache\com.pfc.community-media'
    [IO.Directory]::CreateDirectory($cacheDirectory) | Out-Null
    $path = Join-Path $cacheDirectory ('cover_' + (Get-StableHash $MediaKey) + '.img')
    if ([IO.File]::Exists($path) -and ([IO.FileInfo]$path).Length -gt 0) { return $path }

    $randomAccess = Await-WinRt ($Properties.Thumbnail.OpenReadAsync()) $script:RandomAccessContentType
    $size = [uint64]$script:RandomAccessType.GetProperty('Size').GetValue($randomAccess)
    if ($size -eq 0 -or $size -gt 10485760) { return $null }
    $input = $script:RandomAccessType.GetMethod('GetInputStreamAt').Invoke($randomAccess, @([uint64]0))
    $stream = $script:AsStreamForRead.Invoke($null, @($input, 65536))
    $temporary = $path + '.' + $PID + '.tmp'
    $file = $null
    try {
        $file = [IO.File]::Create($temporary)
        $stream.CopyTo($file)
        $file.Flush()
        $file.Dispose(); $file = $null
        [IO.File]::Move($temporary, $path)
        return $path
    } finally {
        if ($null -ne $file) { $file.Dispose() }
        if ($null -ne $stream) { $stream.Dispose() }
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

function Get-BestMediaSession {
    if ($null -eq $script:Manager) {
        $script:Manager = Await-WinRt ($script:ManagerType::RequestAsync()) $script:ManagerType
    }
    $current = $script:Manager.GetCurrentSession()
    $candidates = [Collections.Generic.List[object]]::new()
    if ($null -ne $current) { $candidates.Add([pscustomobject]@{ Session = $current; Current = $true }) }
    foreach ($session in $script:Manager.GetSessions()) {
        $candidates.Add([pscustomobject]@{ Session = $session; Current = $false })
    }

    $best = $null
    $bestScore = -1
    foreach ($candidate in $candidates) {
        try {
            $properties = Await-WinRt ($candidate.Session.TryGetMediaPropertiesAsync()) $script:PropertiesType
            $playing = $candidate.Session.GetPlaybackInfo().PlaybackStatus.ToString() -eq 'Playing'
            $hasMetadata = -not [string]::IsNullOrWhiteSpace($properties.Title) -or
                -not [string]::IsNullOrWhiteSpace($properties.Artist)
            $score = $(if ($hasMetadata) { 100 } else { 0 }) +
                $(if ($playing) { 40 } else { 0 }) +
                $(if ($candidate.Current) { 10 } else { 0 })
            if ($score -gt $bestScore) {
                $bestScore = $score
                $best = [pscustomobject]@{
                    Session = $candidate.Session
                    Properties = $properties
                    Playing = $playing
                    SourceApp = [string]$candidate.Session.SourceAppUserModelId
                    SessionCount = $candidates.Count
                }
            }
        } catch {
        }
    }
    return $best
}

function Get-MediaState {
    $defaultCover = Join-Path $script:WidgetDirectory 'assets\media_cover.png'
    try {
        $best = Get-BestMediaSession
        if ($null -eq $best) {
            if ($null -ne $script:LastGood -and
                ([datetime]::UtcNow - $script:LastGoodAt).TotalMinutes -le 10) {
                $stale = $script:LastGood.PSObject.Copy()
                $stale.Stale = $true
                return $stale
            }
            return [pscustomobject]@{
                Active = $false; Playing = $false; Stale = $false
                Title = 'No media'; Artist = 'Open a player'; CoverPath = $defaultCover
                BackgroundLeftColor = '#FF0F172A'; BackgroundRightColor = '#FF111827'
                AccentColor = '#FF22D3EE'; SourceApp = ''; SessionCount = 0
            }
        }
        $title = if ([string]::IsNullOrWhiteSpace($best.Properties.Title)) { 'Unknown media' } else { [string]$best.Properties.Title }
        $artist = if ([string]::IsNullOrWhiteSpace($best.Properties.Artist)) { 'Unknown artist' } else { [string]$best.Properties.Artist }
        $coverPath = Save-Thumbnail $best.Properties ($title + '|' + $artist + '|' + $best.SourceApp)
        if ([string]::IsNullOrWhiteSpace($coverPath)) { $coverPath = $defaultCover }
        $palette = Get-AdaptivePalette $coverPath
        $state = [pscustomobject]@{
            Active = $true; Playing = [bool]$best.Playing; Stale = $false
            Title = $title; Artist = $artist; CoverPath = $coverPath
            BackgroundLeftColor = $palette.Left; BackgroundRightColor = $palette.Right
            AccentColor = $palette.Accent; SourceApp = $best.SourceApp
            SessionCount = [int]$best.SessionCount
        }
        $script:LastGood = $state
        $script:LastGoodAt = [datetime]::UtcNow
        return $state
    } catch {
        $script:Manager = $null
        if ($null -ne $script:LastGood -and
            ([datetime]::UtcNow - $script:LastGoodAt).TotalMinutes -le 10) {
            $stale = $script:LastGood.PSObject.Copy()
            $stale.Stale = $true
            return $stale
        }
        throw
    }
}

function Invoke-PlaybackToggle {
    try {
        $best = Get-BestMediaSession
        if ($null -ne $best) {
            Await-WinRt ($best.Session.TryTogglePlayPauseAsync()) ([bool]) | Out-Null
            return
        }
    } catch {
        $script:Manager = $null
    }
    [CommunityMediaKeys]::Toggle()
}

function Set-Instances {
    param($Message)
    $next = @{}
    foreach ($instance in @($Message.instances)) {
        if ($null -ne $instance -and -not [string]::IsNullOrWhiteSpace([string]$instance.instanceId)) {
            $next[[string]$instance.instanceId] = $instance.settings
        }
    }
    $script:Instances = $next
}

function Publish-Snapshots {
    param($State, [bool] $Force)
    foreach ($entry in $script:Instances.GetEnumerator()) {
        $darkMode = $true
        if ($null -ne $entry.Value -and $null -ne $entry.Value.darkMode) {
            $darkMode = [bool]$entry.Value.darkMode
        }
        $data = [ordered]@{
            active = [bool]$State.Active
            playing = [bool]$State.Playing
            stale = [bool]$State.Stale
            title = [string]$State.Title
            artist = [string]$State.Artist
            coverPath = [string]$State.CoverPath
            backgroundLeftColor = $(if ($darkMode) { $State.BackgroundLeftColor } else { '#FFFAFAF8' })
            backgroundRightColor = $(if ($darkMode) { $State.BackgroundRightColor } else { '#FFFAFAF8' })
            titleColor = $(if ($darkMode) { '#FFF8FAFC' } else { '#FF000000' })
            artistColor = $(if ($darkMode) { '#FF94A3B8' } else { '#F0000000' })
            playButtonColor = $(if ($State.Active) { $State.AccentColor } else { '#FF334155' })
            playIconColor = $(if ($State.Active) { '#FF08171F' } else { '#FFCBD5E1' })
            playGlyph = $(if ($State.Playing) { [string][char]0xE769 } else { [string][char]0xE768 })
            sourceApp = [string]$State.SourceApp
            sessionCount = [int]$State.SessionCount
        }
        $signature = $data | ConvertTo-Json -Compress -Depth 6
        if (-not $Force -and $script:LastSignatures[$entry.Key] -eq $signature) { continue }
        $script:LastSignatures[$entry.Key] = $signature
        $message = [ordered]@{
            type = 'snapshot'
            instanceId = [string]$entry.Key
            status = 'ok'
            data = $data
        } | ConvertTo-Json -Compress -Depth 8
        [Console]::Out.WriteLine($message)
        [Console]::Out.Flush()
    }
}

$shutdown = $false
$forceSnapshot = $true
$nextPoll = [datetime]::UtcNow
[CommunityMediaInputReader]::Start()

while (-not $shutdown) {
    $line = $null
    while ([CommunityMediaInputReader]::TryRead([ref]$line)) {
        try {
            $message = $line | ConvertFrom-Json
            switch ([string]$message.type) {
                'initialize' {
                    $script:WidgetDirectory = [string]$message.widgetDirectory
                    $script:DataDirectory = [string]$message.dataDirectory
                    Set-Instances $message
                    $forceSnapshot = $true
                    $nextPoll = [datetime]::UtcNow
                }
                'instancesChanged' {
                    Set-Instances $message
                    $forceSnapshot = $true
                }
                'action' {
                    if ([string]$message.action -eq 'togglePlayback') {
                        Invoke-PlaybackToggle
                        $forceSnapshot = $true
                        $nextPoll = [datetime]::UtcNow
                    }
                }
                'shutdown' { $shutdown = $true }
            }
        } catch {
            [Console]::Error.WriteLine('Protocol input failed: ' + $_.Exception.Message)
        }
    }
    if ([CommunityMediaInputReader]::Ended -and $null -eq $line) { break }

    if (-not $shutdown -and [datetime]::UtcNow -ge $nextPoll) {
        try {
            Publish-Snapshots (Get-MediaState) $forceSnapshot
            $forceSnapshot = $false
        } catch {
            [Console]::Error.WriteLine('Media refresh failed: ' + $_.Exception.Message)
        }
        $nextPoll = [datetime]::UtcNow.AddSeconds(1)
    }
    Start-Sleep -Milliseconds 80
}
