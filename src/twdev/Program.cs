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
                "init" => Init(
                    args[1],
                    Option(args, "--author"),
                    Option(args, "--website"),
                    Option(args, "--renderer"),
                    Option(args, "--runtime")),
                "validate" => Validate(args[1]),
                "hash" => Hash(args[1]),
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

    private static int Init(
        string id,
        string? author,
        string? website,
        string? renderer,
        string? runtime)
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
        if (!string.IsNullOrWhiteSpace(runtime))
        {
            if (!string.Equals(runtime, "process", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("--runtime currently supports only process.");
            }
            renderer = string.IsNullOrWhiteSpace(renderer) ? "native" : renderer;
            if (!string.Equals(renderer, "web", StringComparison.OrdinalIgnoreCase) &&
                !string.Equals(renderer, "native", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    "A process starter supports --renderer native or web.");
            }
            return string.Equals(renderer, "web", StringComparison.OrdinalIgnoreCase)
                ? InitProcess(directory, id, authorJson, websiteJson)
                : InitNativeProcess(directory, id, authorJson, websiteJson);
        }
        if (string.Equals(renderer, "web", StringComparison.OrdinalIgnoreCase))
        {
            return InitWeb(directory, id, authorJson, websiteJson);
        }
        if (!string.IsNullOrWhiteSpace(renderer) &&
            !string.Equals(renderer, "native", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(renderer, "declarative", StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException("--renderer must be native or web.");
        }
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

    private static int Hash(string directory)
    {
        Console.WriteLine(CommunityWidgetRegistry.ComputeContentHashForTool(directory));
        return 0;
    }

    private static int InitNativeProcess(
        string directory,
        string id,
        string authorJson,
        string websiteJson)
    {
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 4,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.4",
          "displayName": "Native Process Widget",
          "description": "A native XAML widget backed by a full-access PowerShell process.",
          "author": { "name": {{authorJson}}{{websiteJson}} },
          "size": { "width": 190, "height": 32 },
          "renderer": {
            "type": "native",
            "entry": "compact.json",
            "expandedEntry": "expanded.json",
            "expandedSize": { "width": 420, "height": 220 },
            "activation": "click"
          },
          "runtime": {
            "type": "process",
            "entry": "provider.ps1",
            "protocol": "json-lines-v1",
            "lifetime": "persistent",
            "runAs": "user",
            "workingDirectory": "package"
          },
          "permissions": {
            "required": [
              {
                "id": "system.fullAccess",
                "reason": "Runs provider.ps1 with the installing user's unrestricted Windows rights."
              },
              {
                "id": "process.list",
                "reason": "Displays the current process count."
              },
              {
                "id": "process.start",
                "scope": ["taskmgr.exe"],
                "reason": "Opens Task Manager after the user presses the native action button."
              }
            ],
            "optional": []
          },
          "supportsMultipleInstances": true,
          "settings": []
        }
        """);
        File.WriteAllText(Path.Combine(directory, "compact.json"), """
        {
          "type": "card",
          "width": 190,
          "height": 32,
          "padding": 6,
          "backgroundColor": "#E6111827",
          "children": [
            {
              "type": "row",
              "gap": 8,
              "children": [
                { "type": "text", "text": "NATIVE", "color": "systemAccent" },
                { "type": "text", "text": "Processes ", "bind": "data.processCount" }
              ]
            }
          ]
        }
        """);
        File.WriteAllText(Path.Combine(directory, "expanded.json"), """
        {
          "type": "card",
          "width": 396,
          "height": 196,
          "padding": 12,
          "backgroundColor": "#FA081220",
          "children": [
            { "type": "text", "text": "Native process widget", "fontSize": 20 },
            { "type": "text", "text": "Running processes: ", "bind": "data.processCount" },
            { "type": "button", "label": "Open Task Manager", "action": "openTaskManager" },
            { "type": "button", "label": "Close", "action": "$close" }
          ]
        }
        """);
        File.WriteAllText(Path.Combine(directory, "provider.ps1"), """
        $ErrorActionPreference = "Stop"
        [Console]::InputEncoding = [Text.Encoding]::UTF8
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        while (($line = [Console]::In.ReadLine()) -ne $null) {
          $message = $line | ConvertFrom-Json
          if ($message.type -eq "action" -and $message.action -eq "openTaskManager") {
            Start-Process taskmgr.exe
          }
          if ($message.type -in @("initialize", "instancesChanged")) {
            foreach ($instance in $message.instances) {
              @{
                type = "snapshot"
                instanceId = $instance.instanceId
                data = @{ processCount = @(Get-Process).Count; updatedAt = [DateTimeOffset]::Now.ToString("O") }
              } | ConvertTo-Json -Compress -Depth 8 | Write-Output
            }
          }
        }
        """);
        Console.WriteLine($"Created {directory}");
        Console.WriteLine("This schema v4 package must be reviewed in Settings before its process can run.");
        return 0;
    }

    private static int InitProcess(string directory, string id, string authorJson, string websiteJson)
    {
        var ui = Path.Combine(directory, "ui");
        Directory.CreateDirectory(ui);
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 4,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.4",
          "displayName": "Full Access Widget",
          "description": "A starter widget backed by a full-access PowerShell process.",
          "author": { "name": {{authorJson}}{{websiteJson}} },
          "size": { "width": 190, "height": 32 },
          "renderer": {
            "type": "web",
            "entry": "ui/index.html",
            "expandedSize": { "width": 420, "height": 220 },
            "activation": "hover"
          },
          "runtime": {
            "type": "process",
            "entry": "provider.ps1",
            "protocol": "json-lines-v1",
            "lifetime": "persistent",
            "runAs": "user",
            "workingDirectory": "package"
          },
          "permissions": {
            "required": [
              {
                "id": "system.fullAccess",
                "reason": "Runs provider.ps1 with the installing user's unrestricted Windows rights."
              }
            ],
            "optional": []
          },
          "supportsMultipleInstances": true,
          "settings": []
        }
        """);
        File.WriteAllText(Path.Combine(ui, "index.html"), """
        <!doctype html>
        <html lang="en">
        <head>
          <meta charset="utf-8">
          <meta name="viewport" content="width=device-width,initial-scale=1">
          <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'self'; script-src 'self'">
          <link rel="stylesheet" href="style.css">
          <script src="widget.js" defer></script>
        </head>
        <body>
          <button id="action" data-tw-drag-region>
            <strong>FULL ACCESS</strong>
            <span>Open Task Manager</span>
          </button>
        </body>
        </html>
        """);
        File.WriteAllText(Path.Combine(ui, "style.css"), """
        * { box-sizing: border-box; }
        html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; background: transparent; }
        body { font: 12px "Segoe UI", sans-serif; color: white; }
        button {
          width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; gap: 10px;
          border-radius: 999px; border: 1px solid #33ffffff; background: #18181b; color: inherit;
        }
        """);
        File.WriteAllText(Path.Combine(ui, "widget.js"), """
        document.querySelector("#action").addEventListener("click", () =>
          window.taskbarWidget.invoke("openTaskManager"));
        window.taskbarWidget.ready();
        """);
        File.WriteAllText(Path.Combine(directory, "provider.ps1"), """
        $ErrorActionPreference = "Stop"
        [Console]::InputEncoding = [Text.Encoding]::UTF8
        [Console]::OutputEncoding = [Text.Encoding]::UTF8
        while (($line = [Console]::In.ReadLine()) -ne $null) {
          $message = $line | ConvertFrom-Json
          if ($message.type -eq "action" -and $message.action -eq "openTaskManager") {
            Start-Process taskmgr.exe
          }
          if ($message.type -in @("initialize", "instancesChanged")) {
            foreach ($instance in $message.instances) {
              @{
                type = "snapshot"
                instanceId = $instance.instanceId
                data = @{ processCount = @(Get-Process).Count; updatedAt = [DateTimeOffset]::Now.ToString("O") }
              } | ConvertTo-Json -Compress -Depth 8 | Write-Output
            }
          }
        }
        """);
        Console.WriteLine($"Created {directory}");
        Console.WriteLine("This schema v4 package must be reviewed in Settings before its process can run.");
        return 0;
    }

    private static int InitWeb(string directory, string id, string authorJson, string websiteJson)
    {
        var ui = Path.Combine(directory, "ui");
        Directory.CreateDirectory(ui);
        File.WriteAllText(Path.Combine(directory, "widget.json"), $$"""
        {
          "schemaVersion": 3,
          "id": "{{id}}",
          "version": "1.0.0",
          "minHostVersion": "0.5.0",
          "displayName": "Web Island",
          "description": "A starter sandboxed web widget.",
          "author": { "name": {{authorJson}}{{websiteJson}} },
          "size": { "width": 170, "height": 32 },
          "renderer": {
            "type": "web",
            "entry": "ui/index.html",
            "expandedSize": { "width": 360, "height": 180 },
            "expandDirection": "auto",
            "activation": "hover",
            "hoverDelayMs": 100,
            "collapseDelayMs": 180,
            "transitionMs": 280
          },
          "permissions": {},
          "supportsMultipleInstances": true,
          "settings": []
        }
        """);
        File.WriteAllText(Path.Combine(ui, "index.html"), """
        <!doctype html>
        <html lang="en">
        <head>
          <meta charset="utf-8">
          <meta name="viewport" content="width=device-width,initial-scale=1">
          <meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'self'; script-src 'self'; img-src 'self' data: blob:; font-src 'self'">
          <link rel="stylesheet" href="style.css">
          <script src="widget.js" defer></script>
        </head>
        <body>
          <main data-tw-drag-region>
            <strong>WEB ISLAND</strong>
            <span id="clock">--:--</span>
          </main>
        </body>
        </html>
        """);
        File.WriteAllText(Path.Combine(ui, "style.css"), """
        * { box-sizing: border-box; }
        html, body { width: 100%; height: 100%; margin: 0; overflow: hidden; background: transparent; }
        body { font: 12px "Segoe UI", sans-serif; color: white; }
        main {
          width: 100%; height: 100%; display: flex; align-items: center; justify-content: center; gap: 12px;
          border-radius: 999px; background: #09090b; border: 1px solid #22ffffff;
        }
        """);
        File.WriteAllText(Path.Combine(ui, "widget.js"), """
        const clock = document.querySelector("#clock");
        function tick() {
          clock.textContent = new Intl.DateTimeFormat([], { hour: "2-digit", minute: "2-digit" }).format(new Date());
        }
        tick();
        setInterval(tick, 1000);
        window.taskbarWidget.ready();
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
        if (definition.ManifestSchemaVersion >= 4)
        {
            throw new InvalidOperationException(
                "Schema v4 packages cannot be side-loaded without review. " +
                "Open this folder from Settings > Developer so its exact permissions are shown before installation.");
        }
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
        Console.WriteLine("twdev init <reverse.domain.id> [--renderer native|web] [--runtime process] [--author \"Name\"] [--website https://example.com]");
        Console.WriteLine("twdev validate <folder>");
        Console.WriteLine("twdev hash <folder>");
        Console.WriteLine("twdev dev <folder>");
        Console.WriteLine("twdev pack <folder> [output.twidget]");
        return 2;
    }
}
