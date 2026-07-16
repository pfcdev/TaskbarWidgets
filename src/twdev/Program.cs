using System.IO.Compression;
using System.Text.Json;
using TaskbarWidgets.Loader;
using TaskbarWidgets.Loader.Core;

namespace TaskbarWidgets.TwDev;

internal static class Program
{
    private static int Main(string[] args)
    {
        try
        {
            if (args.Length < 2) return Usage();
            return args[0].ToLowerInvariant() switch
            {
                "init" => Init(args[1], Option(args, "--author"), Option(args, "--website")),
                "validate" => Validate(args[1]),
                "dev" => Dev(args[1]),
                "pack" => Pack(args[1], args.Length > 2 ? args[2] : null),
                _ => Usage()
            };
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("Error: " + ex.Message);
            return 1;
        }
    }

    private static string? Option(string[] args, string name)
    {
        var index = Array.FindIndex(args, value => string.Equals(value, name, StringComparison.OrdinalIgnoreCase));
        return index >= 0 && index + 1 < args.Length ? args[index + 1] : null;
    }

    private static int Init(string id, string? author, string? website)
    {
        if (!id.Contains('.') || id.Any(character =>
                !(char.IsAsciiLetterOrDigit(character) || character is '.' or '-')))
        {
            throw new InvalidDataException("Use a reverse-domain id such as com.example.clock.");
        }
        author = string.IsNullOrWhiteSpace(author) ? "Your Name" : author.Trim();
        if (author.Length > 80) throw new InvalidDataException("Author name must be 80 characters or fewer.");
        if (!string.IsNullOrWhiteSpace(website) &&
            (!Uri.TryCreate(website, UriKind.Absolute, out var authorUri) || authorUri.Scheme != Uri.UriSchemeHttps))
        {
            throw new InvalidDataException("Author website must use HTTPS.");
        }
        var authorJson = JsonSerializer.Serialize(author);
        var websiteJson = string.IsNullOrWhiteSpace(website)
            ? ""
            : $", \"website\": {JsonSerializer.Serialize(website)}";
        var directory = Path.GetFullPath(id);
        if (Directory.Exists(directory)) throw new IOException("Target directory already exists.");
        Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 2,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.4.0",
          "displayName": "Community Clock",
          "description": "A starter Taskbar Widgets clock.",
          "author": { "name": {{authorJson}}{{websiteJson}} },
          "size": { "width": 96, "height": 24 },
          "entry": {
            "layout": "layout.json",
            "provider": { "type": "clock", "refreshSeconds": 1 }
          },
          "permissions": {},
          "supportsMultipleInstances": true,
          "settings": []
        }
        """);
        File.WriteAllText(Path.Combine(directory, "layout.json"), """
        {
          "type": "row",
          "gap": 3,
          "children": [
            { "type": "icon", "glyph": "\\uE823", "color": "systemAccent" },
            { "type": "text", "bind": "data.time", "fontSize": 11 }
          ]
        }
        """);
        Console.WriteLine($"Created {directory}");
        return 0;
    }

    private static int Validate(string path)
    {
        var definition = CommunityWidgetRegistry.ValidateForTool(path);
        if (!definition.Valid)
        {
            Console.Error.WriteLine(definition.Error);
            return 1;
        }
        Console.WriteLine($"Valid: {definition.Id} {definition.Version} ({definition.Width}x{definition.Height})");
        return 0;
    }

    private static int Dev(string path)
    {
        var definition = CommunityWidgetRegistry.ValidateForTool(path);
        if (!definition.Valid) throw new InvalidDataException(definition.Error);
        var target = Path.Combine(AppPaths.CommunityWidgetsDirectory, definition.Id);
        Directory.CreateDirectory(AppPaths.CommunityWidgetsDirectory);
        CopyDirectory(Path.GetFullPath(path), target);
        Console.WriteLine($"Installed development copy: {target}");
        return 0;
    }

    private static int Pack(string path, string? output)
    {
        var definition = CommunityWidgetRegistry.ValidateForTool(path);
        if (!definition.Valid) throw new InvalidDataException(definition.Error);
        output = Path.GetFullPath(output ?? $"{definition.Id}-{definition.Version}.twidget");
        if (File.Exists(output)) File.Delete(output);
        ZipFile.CreateFromDirectory(Path.GetFullPath(path), output, CompressionLevel.Optimal, false);
        Console.WriteLine(output);
        return 0;
    }

    private static void CopyDirectory(string source, string target)
    {
        Directory.CreateDirectory(target);
        foreach (var file in Directory.EnumerateFiles(source))
        {
            File.Copy(file, Path.Combine(target, Path.GetFileName(file)), true);
        }
        foreach (var child in Directory.EnumerateDirectories(source))
        {
            CopyDirectory(child, Path.Combine(target, Path.GetFileName(child)));
        }
    }

    private static int Usage()
    {
        Console.WriteLine("twdev init <reverse.domain.id> [--author \"Name\"] [--website https://example.com]");
        Console.WriteLine("twdev validate <folder>");
        Console.WriteLine("twdev dev <folder>");
        Console.WriteLine("twdev pack <folder> [output.twidget]");
        return 2;
    }
}
