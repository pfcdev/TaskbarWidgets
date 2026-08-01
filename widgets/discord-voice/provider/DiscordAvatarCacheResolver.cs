using System.Collections.Concurrent;
using System.Text;
using System.Text.RegularExpressions;

namespace TaskbarWidgets.Loader;

internal static partial class DiscordAvatarCacheResolver
{
    private static readonly ConcurrentDictionary<string, byte> Pending = new();
    private static readonly HttpClient Client = new()
    {
        Timeout = TimeSpan.FromSeconds(12)
    };

    public static void Queue(string discordUserId, string destinationPath)
    {
        if (File.Exists(destinationPath) || !Pending.TryAdd(discordUserId, 0))
        {
            return;
        }

        _ = Task.Run(async () =>
        {
            try
            {
                var url = FindAvatarUrl(discordUserId);
                if (string.IsNullOrWhiteSpace(url))
                {
                    return;
                }

                var bytes = await Client.GetByteArrayAsync(url);
                if (bytes.Length is < 128 or > 2_000_000)
                {
                    return;
                }

                var temporaryPath = destinationPath + ".tmp";
                await File.WriteAllBytesAsync(temporaryPath, bytes);
                File.Move(temporaryPath, destinationPath, overwrite: true);
            }
            catch
            {
                // A participant can still be shown while Discord repopulates its cache.
            }
            finally
            {
                Pending.TryRemove(discordUserId, out _);
            }
        });
    }

    internal static string? FindAvatarUrl(string discordUserId)
    {
        foreach (var cacheIndex in GetCacheIndexPaths())
        {
            try
            {
                using var stream = new FileStream(
                    cacheIndex,
                    FileMode.Open,
                    FileAccess.Read,
                    FileShare.ReadWrite | FileShare.Delete);
                if (stream.Length is <= 0 or > 32 * 1024 * 1024)
                {
                    continue;
                }

                var bytes = new byte[checked((int)stream.Length)];
                stream.ReadExactly(bytes);
                var contents = Encoding.Latin1.GetString(bytes);
                var matches = AvatarUrlRegex().Matches(contents);
                for (var index = matches.Count - 1; index >= 0; index--)
                {
                    var match = matches[index];
                    if (!string.Equals(
                            match.Groups["id"].Value,
                            discordUserId,
                            StringComparison.Ordinal))
                    {
                        continue;
                    }

                    var hash = match.Groups["hash"].Value;
                    return $"https://cdn.discordapp.com/avatars/{discordUserId}/{hash}.webp?size=128";
                }
            }
            catch
            {
                // Discord may rotate this index while it is being read.
            }
        }
        return null;
    }

    private static IEnumerable<string> GetCacheIndexPaths()
    {
        var roaming = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        foreach (var product in new[] { "discord", "discordcanary", "discordptb" })
        {
            yield return Path.Combine(roaming, product, "Cache", "Cache_Data", "data_1");
        }
    }

    [GeneratedRegex(
        @"https://(?:cdn|media)\.discordapp\.(?:com|net)/avatars/(?<id>\d+)/(?<hash>[a-fA-F0-9_]+)\.webp\?size=\d+",
        RegexOptions.CultureInvariant)]
    private static partial Regex AvatarUrlRegex();
}
