using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace TaskbarWidgets.Loader;

internal static partial class DiscordWebRtcLogProbe
{
    private const int TailBytes = 768 * 1024;
    private static readonly object Gate = new();
    private static readonly Dictionary<string, UserCounters> Previous = new();
    private static string _lastPath = "";
    private static long _lastLength = -1;
    private static DiscordWebRtcSnapshot _cached = new();

    public static DiscordWebRtcSnapshot Capture()
    {
        lock (Gate)
        {
            var path = FindCurrentLog();
            if (string.IsNullOrWhiteSpace(path))
            {
                return new DiscordWebRtcSnapshot();
            }

            var info = new FileInfo(path);
            if (string.Equals(path, _lastPath, StringComparison.OrdinalIgnoreCase) &&
                info.Length == _lastLength)
            {
                return WithCurrentSpeaking(_cached);
            }

            _lastPath = path;
            _lastLength = info.Length;
            _cached = ParseTail(path);
            return WithCurrentSpeaking(_cached);
        }
    }

    internal static DiscordWebRtcSnapshot Parse(string text)
    {
        var observations = new List<LogObservation>();
        var index = 0;
        foreach (var line in text.Split('\n'))
        {
            var inbound = InboundRegex().Match(line);
            if (inbound.Success)
            {
                observations.Add(new LogObservation(
                    inbound.Groups["id"].Value,
                    ParseUInt(inbound.Groups["ssrc"].Value),
                    ParseLong(inbound.Groups["packets"].Value),
                    ParseLong(inbound.Groups["normal"].Value),
                    false,
                    ParseTimestamp(inbound.Groups["time"].Value),
                    index++));
                continue;
            }

            var outbound = OutboundRegex().Match(line);
            if (outbound.Success)
            {
                observations.Add(new LogObservation(
                    outbound.Groups["id"].Value,
                    ParseUInt(outbound.Groups["ssrc"].Value),
                    ParseLong(outbound.Groups["packets"].Value),
                    0,
                    true,
                    ParseTimestamp(outbound.Groups["time"].Value),
                    index++));
            }
        }

        if (observations.Count == 0)
        {
            return new DiscordWebRtcSnapshot();
        }

        var newest = observations.Max(item => item.Timestamp);
        var latestBatch = observations
            .Where(item => newest - item.Timestamp <= TimeSpan.FromSeconds(2))
            .GroupBy(item => item.Id)
            .Select(group => group.OrderByDescending(item => item.Sequence).First())
            .OrderBy(item => item.Sequence)
            .ToList();

        var now = DateTimeOffset.UtcNow;
        var users = new List<DiscordWebRtcUser>();
        foreach (var item in latestBatch)
        {
            var speaking = false;
            if (Previous.TryGetValue(item.Id, out var previous) &&
                (item.NormalFrames > previous.NormalFrames ||
                 item.Packets > previous.Packets))
            {
                previous.SpeakingUntilUtc = now.AddSeconds(2.5);
                speaking = true;
            }

            Previous[item.Id] = new UserCounters(
                item.Packets,
                item.NormalFrames,
                speaking
                    ? now.AddSeconds(2.5)
                    : Previous.GetValueOrDefault(item.Id)?.SpeakingUntilUtc ?? DateTimeOffset.MinValue);
            var hasRealtimeState = DiscordRealtimeVoiceClient.TryGetSpeaking(
                item.Ssrc,
                out var realtimeSpeaking);
            users.Add(new DiscordWebRtcUser
            {
                Id = item.Id,
                Ssrc = item.Ssrc,
                Local = item.Local,
                Speaking = hasRealtimeState
                    ? realtimeSpeaking
                    : speaking || Previous[item.Id].SpeakingUntilUtc > now
            });
        }

        return new DiscordWebRtcSnapshot { Users = users };
    }

    private static DiscordWebRtcSnapshot ParseTail(string path)
    {
        using var stream = new FileStream(
            path,
            FileMode.Open,
            FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete);
        var length = checked((int)Math.Min(stream.Length, TailBytes));
        stream.Seek(-length, SeekOrigin.End);
        var bytes = new byte[length];
        stream.ReadExactly(bytes);
        return Parse(Encoding.UTF8.GetString(bytes));
    }

    private static DiscordWebRtcSnapshot WithCurrentSpeaking(DiscordWebRtcSnapshot snapshot)
    {
        var now = DateTimeOffset.UtcNow;
        return new DiscordWebRtcSnapshot
        {
            Users = snapshot.Users.Select(user => new DiscordWebRtcUser
            {
                Id = user.Id,
                Ssrc = user.Ssrc,
                Local = user.Local,
                Speaking = DiscordRealtimeVoiceClient.TryGetSpeaking(user.Ssrc, out var realtimeSpeaking)
                    ? realtimeSpeaking
                    : Previous.GetValueOrDefault(user.Id)?.SpeakingUntilUtc > now
            }).ToList()
        };
    }

    private static string? FindCurrentLog()
    {
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        foreach (var product in new[] { "discord", "discordcanary", "discordptb" })
        {
            var logs = Path.Combine(roaming, product, "logs");
            var current = Path.Combine(logs, "discord-webrtc_0");
            if (File.Exists(current) && new FileInfo(current).Length > 0)
            {
                return current;
            }
            var previous = Path.Combine(logs, "discord-last-webrtc_0");
            if (File.Exists(previous))
            {
                return previous;
            }
        }
        return null;
    }

    private static long ParseLong(string value) =>
        long.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : 0;

    private static uint ParseUInt(string value) =>
        uint.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out var parsed)
            ? parsed
            : 0;

    private static DateTimeOffset ParseTimestamp(string value) =>
        DateTimeOffset.TryParseExact(
            value,
            "yyyy-MM-dd HH:mm:ss.fff",
            CultureInfo.InvariantCulture,
            DateTimeStyles.AssumeLocal,
            out var parsed)
            ? parsed.ToUniversalTime()
            : DateTimeOffset.MinValue;

    [GeneratedRegex(
        @"^\[(?<time>[^\]]+)\].*\[default\] Inbound stats for user: (?<id>\d+), audio ssrc: (?<ssrc>\d+),.*packets received: (?<packets>\d+),.*audio frames normal: (?<normal>\d+),",
        RegexOptions.CultureInvariant)]
    private static partial Regex InboundRegex();

    [GeneratedRegex(
        @"^\[(?<time>[^\]]+)\].*\[default\] Outbound audio stats for user: (?<id>\d+), audio ssrc: (?<ssrc>\d+),.*packets sent: (?<packets>\d+),",
        RegexOptions.CultureInvariant)]
    private static partial Regex OutboundRegex();

    private sealed record LogObservation(
        string Id,
        uint Ssrc,
        long Packets,
        long NormalFrames,
        bool Local,
        DateTimeOffset Timestamp,
        int Sequence);

    private sealed class UserCounters(
        long packets,
        long normalFrames,
        DateTimeOffset speakingUntilUtc)
    {
        public long Packets { get; } = packets;
        public long NormalFrames { get; } = normalFrames;
        public DateTimeOffset SpeakingUntilUtc { get; set; } = speakingUntilUtc;
    }
}

internal sealed class DiscordWebRtcSnapshot
{
    public List<DiscordWebRtcUser> Users { get; init; } = [];
}

internal sealed class DiscordWebRtcUser
{
    public string Id { get; init; } = "";
    public uint Ssrc { get; init; }
    public bool Local { get; init; }
    public bool Speaking { get; init; }
}
