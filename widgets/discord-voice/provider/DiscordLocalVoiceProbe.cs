using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Automation;

namespace TaskbarWidgets.Loader;

internal static class DiscordLocalVoiceProbe
{
    private const int MaxUsers = 5;

    public static LocalDiscordVoiceSnapshot Capture(string avatarDirectory)
    {
        using var process = FindDiscordWindowProcess();
        if (process is null)
        {
            return LocalDiscordVoiceSnapshot.Empty("Discord not running");
        }

        var window = process.MainWindowHandle;
        var selectedChannelName = ParseActiveChannelName(process.MainWindowTitle);
        var root = AutomationElement.FromHandle(window);
        if (root is null)
        {
            return LocalDiscordVoiceSnapshot.Empty("Discord window unavailable");
        }

        var voice = FindActiveVoiceItem(root, selectedChannelName);
        if (voice is null)
        {
            return LocalDiscordVoiceSnapshot.Empty("No voice");
        }

        var users = ReadUsers(voice.Item, avatarDirectory, DiscordWebRtcLogProbe.Capture());
        return new LocalDiscordVoiceSnapshot
        {
            Connected = true,
            Status = "Voice",
            ChannelName = voice.ChannelName,
            Users = users
        };
    }

    internal static string ParseActiveChannelName(string? windowTitle)
    {
        if (string.IsNullOrWhiteSpace(windowTitle))
        {
            return "";
        }

        var title = windowTitle.Trim();
        const string discordSuffix = " - Discord";
        if (title.EndsWith(discordSuffix, StringComparison.OrdinalIgnoreCase))
        {
            title = title[..^discordSuffix.Length];
        }

        var separator = title.IndexOf(" | ", StringComparison.Ordinal);
        return (separator >= 0 ? title[..separator] : title).Trim();
    }

    internal static string StableUserId(string displayName, int duplicateIndex = 0)
    {
        var source = duplicateIndex == 0 ? displayName : $"{displayName}\n{duplicateIndex}";
        var hash = SHA256.HashData(Encoding.UTF8.GetBytes(source.ToUpperInvariant()));
        return Convert.ToHexString(hash.AsSpan(0, 10)).ToLowerInvariant();
    }

    private static Process? FindDiscordWindowProcess()
    {
        Process? selected = null;
        foreach (var process in Process.GetProcessesByName("Discord"))
        {
            try
            {
                if (process.MainWindowHandle != IntPtr.Zero)
                {
                    selected?.Dispose();
                    selected = process;
                }
                else
                {
                    process.Dispose();
                }
            }
            catch
            {
                process.Dispose();
            }
        }
        return selected;
    }

    private static VoiceItemMatch? FindActiveVoiceItem(
        AutomationElement root,
        string selectedChannelName)
    {
        var links = root.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Hyperlink));

        VoiceItemMatch? fallback = null;
        for (var i = 0; i < links.Count; i++)
        {
            var link = links[i];
            var name = SafeCurrentName(link);
            var listItem = FindAncestorListItem(link);
            if (listItem is null || !ContainsParticipantRows(listItem))
            {
                continue;
            }

            var accessibleChannelName = ExtractAccessibleChannelName(name);
            var match = new VoiceItemMatch(
                listItem,
                string.IsNullOrWhiteSpace(accessibleChannelName)
                    ? selectedChannelName
                    : accessibleChannelName);
            if (!string.IsNullOrWhiteSpace(selectedChannelName) &&
                name.StartsWith(selectedChannelName, StringComparison.CurrentCultureIgnoreCase))
            {
                return match;
            }
            fallback ??= match;
        }

        return fallback;
    }

    private static string ExtractAccessibleChannelName(string value)
    {
        var controlTypeMarker = value.IndexOf(" (", StringComparison.Ordinal);
        if (controlTypeMarker > 0)
        {
            return value[..controlTypeMarker].Trim();
        }
        var comma = value.IndexOf(',', StringComparison.Ordinal);
        return (comma > 0 ? value[..comma] : value).Trim();
    }

    private static AutomationElement? FindAncestorListItem(AutomationElement element)
    {
        var walker = TreeWalker.ControlViewWalker;
        var current = element;
        for (var depth = 0; depth < 10; depth++)
        {
            current = walker.GetParent(current);
            if (current is null)
            {
                return null;
            }
            if (current.Current.ControlType == ControlType.ListItem)
            {
                return current;
            }
        }
        return null;
    }

    private static bool ContainsParticipantRows(AutomationElement listItem)
    {
        var buttons = listItem.FindAll(
            TreeScope.Descendants,
            new PropertyCondition(AutomationElement.ControlTypeProperty, ControlType.Button));
        for (var i = 0; i < buttons.Count; i++)
        {
            if (IsParticipantButton(buttons[i]) || LooksLikeParticipantButton(buttons[i]))
            {
                return true;
            }
        }
        return false;
    }

    private static List<LocalDiscordVoiceUser> ReadUsers(
        AutomationElement voiceItem,
        string avatarDirectory,
        DiscordWebRtcSnapshot webRtc)
    {
        Directory.CreateDirectory(avatarDirectory);
        var descendants = voiceItem.FindAll(TreeScope.Descendants, Condition.TrueCondition);
        var texts = new List<AutomationElement>();
        var allButtons = new List<AutomationElement>();
        for (var i = 0; i < descendants.Count; i++)
        {
            var element = descendants[i];
            try
            {
                if (element.Current.ControlType == ControlType.Text &&
                    !string.IsNullOrWhiteSpace(element.Current.Name))
                {
                    texts.Add(element);
                }
                else if (element.Current.ControlType == ControlType.Button)
                {
                    allButtons.Add(element);
                }
            }
            catch (ElementNotAvailableException)
            {
                // Discord rebuilt this row during the accessibility scan.
            }
        }

        var buttons = SelectParticipantButtons(allButtons, texts);

        var duplicateNames = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
        var result = new List<LocalDiscordVoiceUser>();
        var orderedButtons = buttons
            .OrderBy(button => SafeRectangle(button).Top)
            .Take(MaxUsers)
            .ToList();
        for (var userIndex = 0; userIndex < orderedButtons.Count; userIndex++)
        {
            var button = orderedButtons[userIndex];
            var rectangle = SafeRectangle(button);
            if (rectangle.IsEmpty || rectangle.Height < 10 || rectangle.Width < 30)
            {
                continue;
            }

            var accessibleName = SafeCurrentName(button);
            var displayName = FindRowText(texts, rectangle);
            if (string.IsNullOrWhiteSpace(displayName))
            {
                displayName = CleanAccessibleUserName(accessibleName);
            }
            if (string.IsNullOrWhiteSpace(displayName))
            {
                continue;
            }

            duplicateNames.TryGetValue(displayName, out var duplicateIndex);
            duplicateNames[displayName] = duplicateIndex + 1;
            var webRtcUser = userIndex < webRtc.Users.Count
                ? webRtc.Users[userIndex]
                : null;
            var id = webRtcUser?.Id ?? StableUserId(displayName, duplicateIndex);
            var avatarPath = Path.Combine(avatarDirectory, $"discord-{id}.webp");
            if (webRtcUser is not null)
            {
                DiscordAvatarCacheResolver.Queue(id, avatarPath);
            }

            result.Add(new LocalDiscordVoiceUser
            {
                Id = id,
                DisplayName = displayName,
                AvatarPath = File.Exists(avatarPath) ? avatarPath : "",
                Speaking = webRtcUser?.Speaking ?? false,
                Muted = HasAccessibleState(accessibleName, MutedStateNames),
                Deafened = HasAccessibleState(accessibleName, DeafenedStateNames),
                Streaming = HasAccessibleState(accessibleName, StreamingStateNames)
            });
        }

        return result;
    }

    private static bool IsParticipantButton(AutomationElement element)
    {
        try
        {
            return element.Current.ClassName.StartsWith("focusTarget", StringComparison.Ordinal) &&
                   !string.IsNullOrWhiteSpace(element.Current.Name);
        }
        catch (ElementNotAvailableException)
        {
            return false;
        }
    }

    private static bool LooksLikeParticipantButton(AutomationElement element)
    {
        var rectangle = SafeRectangle(element);
        return !rectangle.IsEmpty &&
               rectangle.Width is >= 80 and <= 420 &&
               rectangle.Height is >= 18 and <= 48 &&
               !string.IsNullOrWhiteSpace(SafeCurrentName(element));
    }

    private static List<AutomationElement> SelectParticipantButtons(
        IEnumerable<AutomationElement> allButtons,
        IEnumerable<AutomationElement> texts)
    {
        var available = allButtons.Where(LooksLikeParticipantButton).ToList();
        var preferred = available.Where(IsParticipantButton).ToList();
        if (preferred.Count > 0)
        {
            return preferred;
        }

        // Discord hashes its CSS class names. If focusTarget changes, participant
        // rows are still button-sized controls with an overlapping text label.
        return available
            .Where(button => !string.IsNullOrWhiteSpace(FindRowText(texts, SafeRectangle(button))))
            .GroupBy(button =>
            {
                var rectangle = SafeRectangle(button);
                return (
                    Left: (int)Math.Round(rectangle.Left / 4),
                    Width: (int)Math.Round(rectangle.Width / 4),
                    Height: (int)Math.Round(rectangle.Height / 3));
            })
            .OrderByDescending(group => group.Count())
            .ThenBy(group => group.Min(button => SafeRectangle(button).Top))
            .FirstOrDefault()?
            .ToList() ?? [];
    }

    private static string FindRowText(
        IEnumerable<AutomationElement> texts,
        System.Windows.Rect buttonRectangle)
    {
        return texts
            .Select(text => new { Element = text, Rectangle = SafeRectangle(text) })
            .Where(item => !item.Rectangle.IsEmpty &&
                           item.Rectangle.Top < buttonRectangle.Bottom &&
                           item.Rectangle.Bottom > buttonRectangle.Top)
            .OrderBy(item => item.Rectangle.Left)
            .Select(item => SafeCurrentName(item.Element).Trim())
            .FirstOrDefault(name => !string.IsNullOrWhiteSpace(name)) ?? "";
    }

    private static string CleanAccessibleUserName(string value)
    {
        foreach (var suffix in MutedStateNames
                     .Concat(DeafenedStateNames)
                     .Concat(StreamingStateNames)
                     .Select(state => $", {state}"))
        {
            if (value.EndsWith(suffix, StringComparison.CurrentCultureIgnoreCase))
            {
                return value[..^suffix.Length].Trim();
            }
        }
        return value.Trim();
    }

    private static bool HasAccessibleState(string value, IEnumerable<string> stateNames) =>
        stateNames.Any(state =>
            value.Contains($", {state}", StringComparison.CurrentCultureIgnoreCase));

    private static readonly string[] MutedStateNames =
    [
        "Muted", "Susturuldu", "Stummgeschaltet", "En sourdine", "Silenciado",
        "Silenziato", "Wyciszony", "Заглушен"
    ];

    private static readonly string[] DeafenedStateNames =
    [
        "Deafened", "Sağırlaştırıldı", "Taubgeschaltet", "Assourdi", "Ensordecido",
        "Disattivato audio", "Ogłuszony", "Оглушен"
    ];

    private static readonly string[] StreamingStateNames =
    [
        "Streaming", "Yayın yapıyor", "Streamt", "En stream", "Transmitiendo",
        "In streaming", "Transmituje", "Стримит"
    ];

    private static string SafeCurrentName(AutomationElement element)
    {
        try
        {
            return element.Current.Name ?? "";
        }
        catch (ElementNotAvailableException)
        {
            return "";
        }
    }

    private static System.Windows.Rect SafeRectangle(AutomationElement element)
    {
        try
        {
            return element.Current.BoundingRectangle;
        }
        catch (ElementNotAvailableException)
        {
            return System.Windows.Rect.Empty;
        }
    }

    private sealed record VoiceItemMatch(AutomationElement Item, string ChannelName);
}

internal sealed class LocalDiscordVoiceSnapshot
{
    public bool Connected { get; init; }
    public string Status { get; init; } = "No voice";
    public string ChannelName { get; init; } = "";
    public List<LocalDiscordVoiceUser> Users { get; init; } = [];

    public static LocalDiscordVoiceSnapshot Empty(string status) => new() { Status = status };
}

internal sealed class LocalDiscordVoiceUser
{
    public string Id { get; init; } = "";
    public string DisplayName { get; init; } = "";
    public string AvatarPath { get; init; } = "";
    public bool Speaking { get; init; }
    public bool Muted { get; init; }
    public bool Deafened { get; init; }
    public bool Streaming { get; init; }
}
