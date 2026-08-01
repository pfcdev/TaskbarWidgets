using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace TaskbarWidgets.Loader.Core;

internal sealed record RuntimeWidgetDefinition(
    string Id,
    string Version,
    string DisplayName,
    string Category,
    string Description,
    string? AuthorName,
    string? AuthorWebsite,
    string Renderer,
    bool Trusted,
    bool Valid,
    string? Error,
    int Width,
    int Height,
    bool SupportsMultipleInstances,
    string? SourcePath,
    JsonObject Permissions,
    JsonArray Settings,
    JsonObject? Layout,
    CommunityProviderDefinition? Provider,
    WebRendererDefinition? WebRenderer = null,
    int ManifestSchemaVersion = 1,
    CommunityProcessRuntimeDefinition? ProcessRuntime = null,
    string? ContentSha256 = null,
    NativeRendererDefinition? NativeRenderer = null,
    JsonObject? ExpandedLayout = null);

internal sealed record CommunityProviderDefinition(
    string Type, string? Path, double RefreshSeconds, JsonObject Configuration);

internal sealed record WebRendererDefinition(
    string Entry,
    int ExpandedWidth,
    int ExpandedHeight,
    string ExpandDirection,
    string Activation,
    int HoverDelayMs,
    int CollapseDelayMs,
    int TransitionMs);

internal sealed record NativeRendererDefinition(
    string? ExpandedEntry,
    int ExpandedWidth,
    int ExpandedHeight,
    string ExpandDirection,
    string Activation);

internal sealed record CommunityProcessRuntimeDefinition(
    string Entry,
    string[] Arguments,
    string Protocol,
    string Lifetime,
    string RunAs,
    string WorkingDirectory);

internal static partial class CommunityWidgetRegistry
{
    private const int MaxFiles = 5000;
    private const long MaxTotalBytes = 250L * 1024 * 1024;
    private const int MaxLayoutDepth = 8;
    private const int MaxLayoutNodes = 64;
    private static readonly object SyncRoot = new();
    private static IReadOnlyDictionary<string, RuntimeWidgetDefinition> _entries =
        new Dictionary<string, RuntimeWidgetDefinition>(StringComparer.OrdinalIgnoreCase);

    private static readonly HashSet<string> LayoutTypes = new(StringComparer.OrdinalIgnoreCase)
    {
        "row", "column", "spacer", "divider", "text", "icon", "image",
        "progress", "bar", "pie", "sparkline", "button", "card"
    };

    [GeneratedRegex("^[a-z][a-z0-9.-]{2,127}$", RegexOptions.CultureInvariant)]
    private static partial Regex CommunityIdPattern();

    [GeneratedRegex("^\\d+\\.\\d+\\.\\d+(?:\\.\\d+)?$", RegexOptions.CultureInvariant)]
    private static partial Regex VersionPattern();

    public static IReadOnlyCollection<RuntimeWidgetDefinition> Entries
    {
        get
        {
            lock (SyncRoot)
            {
                return _entries.Values.ToArray();
            }
        }
    }

    public static bool IsInstalled(string? id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            return false;
        }

        lock (SyncRoot)
        {
            return _entries.TryGetValue(id, out var entry) && entry.Valid;
        }
    }

    public static RuntimeWidgetDefinition? Find(string? id)
    {
        if (string.IsNullOrWhiteSpace(id))
        {
            return null;
        }

        lock (SyncRoot)
        {
            return _entries.TryGetValue(id, out var entry) ? entry : null;
        }
    }

    internal static RuntimeWidgetDefinition ValidateForTool(string directory) =>
        ValidateCommunityDirectory(
            Path.GetFullPath(directory),
            WidgetCatalog.KnownIds);

    internal static string ComputeContentHashForTool(string directory) =>
        ValidateDirectoryEnvelope(Path.GetFullPath(directory));

    public static void Initialize()
    {
        Directory.CreateDirectory(AppPaths.CommunityWidgetsDirectory);
        Directory.CreateDirectory(AppPaths.CommunityWidgetCacheDirectory);
        Directory.CreateDirectory(AppPaths.CommunityWidgetLogsDirectory);
        Directory.CreateDirectory(AppPaths.CommunityWidgetApprovalsDirectory);
        Directory.CreateDirectory(AppPaths.RuntimeDirectory);
        lock (SyncRoot)
        {
            _entries = ReadPreviousCatalog();
        }
        Refresh();
    }

    public static async Task RunWatcherAsync(CancellationToken cancellationToken)
    {
        using var watcher = new FileSystemWatcher(AppPaths.CommunityWidgetsDirectory)
        {
            IncludeSubdirectories = true,
            NotifyFilter = NotifyFilters.FileName | NotifyFilters.DirectoryName |
                           NotifyFilters.LastWrite | NotifyFilters.Size,
            EnableRaisingEvents = true
        };
        using var signal = new SemaphoreSlim(0, 1);
        void Changed(object? _, FileSystemEventArgs __)
        {
            if (signal.CurrentCount == 0)
            {
                signal.Release();
            }
        }
        watcher.Changed += Changed;
        watcher.Created += Changed;
        watcher.Deleted += Changed;
        watcher.Renamed += Changed;

        while (!cancellationToken.IsCancellationRequested)
        {
            try
            {
                var changed = await signal.WaitAsync(TimeSpan.FromMinutes(5), cancellationToken);
                if (changed)
                {
                    await Task.Delay(TimeSpan.FromMilliseconds(750), cancellationToken);
                    while (signal.Wait(0)) { }
                }
                Refresh();
            }
            catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
            {
                break;
            }
            catch (Exception ex)
            {
                Log($"Community widget watcher failed: {ex.Message}");
            }
        }
    }

    public static void Refresh()
    {
        IReadOnlyDictionary<string, RuntimeWidgetDefinition> previous;
        lock (SyncRoot)
        {
            previous = _entries;
        }
        var definitions = new Dictionary<string, RuntimeWidgetDefinition>(StringComparer.OrdinalIgnoreCase);
        foreach (var builtin in ReadBuiltins())
        {
            definitions[builtin.Id] = builtin;
        }

        foreach (var directory in Directory.EnumerateDirectories(AppPaths.CommunityWidgetsDirectory)
                     .OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
        {
            var definition = ValidateCommunityDirectory(directory, definitions.Keys);
            if (definition.Valid &&
                definition.ManifestSchemaVersion >= 4 &&
                !CommunityPermissions.IsApproved(definition))
            {
                definition = definition with
                {
                    Valid = false,
                    Error = "This schema v4 package is not approved for its current content hash and permissions. Review it again in Settings."
                };
            }
            if (!definition.Valid && previous.TryGetValue(definition.Id, out var lastWorking) &&
                lastWorking.Valid &&
                lastWorking.ManifestSchemaVersion < 4 &&
                string.Equals(lastWorking.SourcePath, directory, StringComparison.OrdinalIgnoreCase))
            {
                definition = lastWorking with
                {
                    Error = $"Using last working version because the current files are invalid: {definition.Error}"
                };
            }
            if (definitions.ContainsKey(definition.Id))
            {
                definition = definition with
                {
                    Valid = false,
                    Error = $"Widget id '{definition.Id}' is reserved or duplicated."
                };
            }
            else
            {
                definitions[definition.Id] = definition;
            }
        }

        var catalog = new JsonObject
        {
            ["schemaVersion"] = 2,
            ["generatedAtUnix"] = DateTimeOffset.UtcNow.ToUnixTimeSeconds(),
            ["communityWidgetsDirectory"] = AppPaths.CommunityWidgetsDirectory,
            ["widgets"] = JsonSerializer.SerializeToNode(definitions.Values.OrderBy(item => item.Id), JsonOptions())
        };
        AtomicJson.Write(AppPaths.RuntimeWidgetCatalogPath, catalog, WidgetConfiguration.JsonOptions());
        lock (SyncRoot)
        {
            _entries = definitions;
        }
        Log($"Runtime widget catalog refreshed: {definitions.Count} entries");
    }

    private static IEnumerable<RuntimeWidgetDefinition> ReadBuiltins()
    {
        var manifestDirectory = Path.Combine(AppPaths.InstallDirectory, "Widgets");
        if (Directory.Exists(manifestDirectory))
        {
            foreach (var path in Directory.EnumerateFiles(manifestDirectory, "*.json", SearchOption.TopDirectoryOnly))
            {
                RuntimeWidgetDefinition? definition = null;
                try
                {
                    var manifest = JsonNode.Parse(File.ReadAllText(path))?.AsObject();
                    var id = manifest?["id"]?.GetValue<string>();
                    if (string.IsNullOrWhiteSpace(id) || !WidgetCatalog.IsKnown(id))
                    {
                        continue;
                    }
                    var size = manifest?["defaultSize"]?.AsObject();
                    definition = new RuntimeWidgetDefinition(
                        id,
                        CurrentVersion(),
                        manifest?["displayName"]?.GetValue<string>() ?? id,
                        manifest?["category"]?.GetValue<string>() ?? "Built-in",
                        manifest?["description"]?.GetValue<string>() ?? "",
                        "Taskbar Widgets",
                        null,
                        "builtin",
                        true,
                        true,
                        null,
                        size?["width"]?.GetValue<int?>() ?? 184,
                        size?["height"]?.GetValue<int?>() ?? 36,
                        false,
                        null,
                        new JsonObject(),
                        manifest?["settings"]?.DeepClone().AsArray() ?? new JsonArray(),
                        null,
                        null,
                        null);
                }
                catch (Exception ex)
                {
                    Log($"Built-in manifest ignored ({Path.GetFileName(path)}): {ex.Message}");
                }
                if (definition is not null)
                {
                    yield return definition;
                }
            }
            yield break;
        }

        foreach (var id in WidgetCatalog.KnownIds)
        {
            yield return new RuntimeWidgetDefinition(
                id, CurrentVersion(), id, "Built-in", "", "Taskbar Widgets", null, "builtin", true, true,
                null, 184, 36, false, null, new JsonObject(), new JsonArray(), null, null, null);
        }
    }

    private static IReadOnlyDictionary<string, RuntimeWidgetDefinition> ReadPreviousCatalog()
    {
        try
        {
            if (!File.Exists(AppPaths.RuntimeWidgetCatalogPath))
            {
                return new Dictionary<string, RuntimeWidgetDefinition>(StringComparer.OrdinalIgnoreCase);
            }
            var root = JsonNode.Parse(File.ReadAllText(AppPaths.RuntimeWidgetCatalogPath))?.AsObject();
            var values = root?["widgets"]?.Deserialize<List<RuntimeWidgetDefinition>>(new JsonSerializerOptions
            {
                PropertyNameCaseInsensitive = true
            }) ?? [];
            return values.Where(value => value.Valid).ToDictionary(
                value => value.Id,
                StringComparer.OrdinalIgnoreCase);
        }
        catch
        {
            return new Dictionary<string, RuntimeWidgetDefinition>(StringComparer.OrdinalIgnoreCase);
        }
    }

    private static RuntimeWidgetDefinition ValidateCommunityDirectory(
        string directory,
        IEnumerable<string> reservedIds)
    {
        var fallbackId = Path.GetFileName(directory).ToLowerInvariant();
        try
        {
            var contentSha256 = ValidateDirectoryEnvelope(directory);
            var manifestPath = Path.Combine(directory, "widget.json");
            if (!File.Exists(manifestPath))
            {
                throw new InvalidDataException("widget.json is missing.");
            }
            var manifest = JsonNode.Parse(File.ReadAllText(manifestPath))?.AsObject()
                           ?? throw new InvalidDataException("widget.json must contain an object.");
            var schemaVersion = manifest["schemaVersion"]?.GetValue<int?>() ?? 0;
            if (schemaVersion is not (2 or 3 or 4))
            {
                throw new InvalidDataException("Only community manifest schemaVersion 2, 3 or 4 is supported.");
            }
            var id = manifest["id"]?.GetValue<string>() ?? "";
            if (!CommunityIdPattern().IsMatch(id) || !id.Contains('.'))
            {
                throw new InvalidDataException("id must use reverse-domain form, for example com.example.clock.");
            }
            if (!string.Equals(id, Path.GetFileName(directory), StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException("The folder name must exactly match the widget id.");
            }
            if (reservedIds.Contains(id, StringComparer.OrdinalIgnoreCase) || WidgetCatalog.IsKnown(id))
            {
                throw new InvalidDataException("The widget id is reserved by a built-in widget.");
            }
            var version = RequiredString(manifest, "version");
            if (!VersionPattern().IsMatch(version))
            {
                throw new InvalidDataException("version must use MAJOR.MINOR.PATCH format.");
            }
            var minHostVersion = RequiredString(manifest, "minHostVersion");
            if (!Version.TryParse(minHostVersion, out var minimum) ||
                !Version.TryParse(CurrentVersion(), out var current) || minimum > current)
            {
                throw new InvalidDataException($"This widget requires Taskbar Widgets {minHostVersion} or later.");
            }

            var size = manifest["size"]?.AsObject()
                       ?? throw new InvalidDataException("size is required.");
            var width = size["width"]?.GetValue<int?>() ?? 0;
            var height = size["height"]?.GetValue<int?>() ?? 0;
            if (width is < 32 or > 480 || height is < 24 or > 48)
            {
                throw new InvalidDataException("size must be between 32x24 and 480x48 DIP.");
            }

            var entry = manifest["entry"] as JsonObject ?? new JsonObject();
            JsonObject? layout = null;
            JsonObject? expandedLayout = null;
            WebRendererDefinition? webRenderer = null;
            NativeRendererDefinition? nativeRenderer = null;
            var renderer = "declarative";
            if (schemaVersion == 2)
            {
                var layoutRelativePath = RequiredString(entry, "layout");
                var layoutPath = ResolveContainedFile(directory, layoutRelativePath, ".json");
                layout = JsonNode.Parse(File.ReadAllText(layoutPath))?.AsObject()
                         ?? throw new InvalidDataException("layout.json must contain an object.");
                var nodeCount = 0;
                ValidateLayout(layout, 1, ref nodeCount);
            }
            else if (schemaVersion == 3)
            {
                var rendererNode = manifest["renderer"]?.AsObject()
                                   ?? throw new InvalidDataException("schemaVersion 3 requires renderer.");
                if (!string.Equals(RequiredString(rendererNode, "type"), "web", StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("schemaVersion 3 currently supports only renderer.type web.");
                }
                var webEntry = RequiredString(rendererNode, "entry");
                ResolveContainedFile(directory, webEntry, ".html");
                var expandedSize = rendererNode["expandedSize"] as JsonObject;
                var expandedWidth = expandedSize?["width"]?.GetValue<int?>() ?? width;
                var expandedHeight = expandedSize?["height"]?.GetValue<int?>() ?? height;
                if (expandedWidth is < 32 or > 640 || expandedHeight is < 24 or > 480)
                {
                    throw new InvalidDataException("renderer.expandedSize must be between 32x24 and 640x480 DIP.");
                }
                var expandDirection = (rendererNode["expandDirection"]?.GetValue<string>() ?? "auto").ToLowerInvariant();
                if (expandDirection is not ("auto" or "up" or "down"))
                {
                    throw new InvalidDataException("renderer.expandDirection must be auto, up, or down.");
                }
                var activation = (rendererNode["activation"]?.GetValue<string>() ?? "hover").ToLowerInvariant();
                if (activation is not ("hover" or "click" or "manual"))
                {
                    throw new InvalidDataException("renderer.activation must be hover, click, or manual.");
                }
                webRenderer = new WebRendererDefinition(
                    webEntry,
                    expandedWidth,
                    expandedHeight,
                    expandDirection,
                    activation,
                    ReadClampedInt(rendererNode, "hoverDelayMs", 100, 0, 1000),
                    ReadClampedInt(rendererNode, "collapseDelayMs", 180, 0, 2000),
                    ReadClampedInt(rendererNode, "transitionMs", 280, 0, 500));
                renderer = "web";
            }
            else
            {
                var rendererNode = manifest["renderer"]?.AsObject()
                                   ?? throw new InvalidDataException("schemaVersion 4 requires renderer.");
                var rendererType = RequiredString(rendererNode, "type").ToLowerInvariant();
                if (rendererType is "declarative" or "native")
                {
                    var layoutRelativePath = RequiredString(rendererNode, "entry");
                    var layoutPath = ResolveContainedFile(directory, layoutRelativePath, ".json");
                    layout = JsonNode.Parse(File.ReadAllText(layoutPath))?.AsObject()
                             ?? throw new InvalidDataException("Declarative renderer entry must contain an object.");
                    var nodeCount = 0;
                    ValidateLayout(layout, 1, ref nodeCount);
                    if (rendererType == "native")
                    {
                        var expandedEntry = rendererNode["expandedEntry"]?.GetValue<string>();
                        var expandedSize = rendererNode["expandedSize"] as JsonObject;
                        var expandedWidth = expandedSize?["width"]?.GetValue<int?>() ?? width;
                        var expandedHeight = expandedSize?["height"]?.GetValue<int?>() ?? height;
                        if (expandedWidth is < 32 or > 640 || expandedHeight is < 24 or > 480)
                        {
                            throw new InvalidDataException(
                                "renderer.expandedSize must be between 32x24 and 640x480 DIP.");
                        }
                        if (!string.IsNullOrWhiteSpace(expandedEntry))
                        {
                            var expandedPath = ResolveContainedFile(directory, expandedEntry, ".json");
                            expandedLayout = JsonNode.Parse(File.ReadAllText(expandedPath))?.AsObject()
                                             ?? throw new InvalidDataException(
                                                 "Native expandedEntry must contain an object.");
                            nodeCount = 0;
                            ValidateLayout(expandedLayout, 1, ref nodeCount);
                        }
                        var expandDirection =
                            (rendererNode["expandDirection"]?.GetValue<string>() ?? "auto").ToLowerInvariant();
                        if (expandDirection is not ("auto" or "up" or "down"))
                        {
                            throw new InvalidDataException(
                                "renderer.expandDirection must be auto, up, or down.");
                        }
                        var activation =
                            (rendererNode["activation"]?.GetValue<string>() ?? "click").ToLowerInvariant();
                        if (activation is not ("hover" or "click" or "manual"))
                        {
                            throw new InvalidDataException(
                                "renderer.activation must be hover, click, or manual.");
                        }
                        nativeRenderer = new NativeRendererDefinition(
                            expandedEntry,
                            expandedWidth,
                            expandedHeight,
                            expandDirection,
                            activation);
                    }
                    renderer = rendererType;
                }
                else if (rendererType == "web")
                {
                    var webEntry = RequiredString(rendererNode, "entry");
                    ResolveContainedFile(directory, webEntry, ".html");
                    var expandedSize = rendererNode["expandedSize"] as JsonObject;
                    var expandedWidth = expandedSize?["width"]?.GetValue<int?>() ?? width;
                    var expandedHeight = expandedSize?["height"]?.GetValue<int?>() ?? height;
                    if (expandedWidth is < 32 or > 640 || expandedHeight is < 24 or > 480)
                    {
                        throw new InvalidDataException(
                            "renderer.expandedSize must be between 32x24 and 640x480 DIP.");
                    }
                    var expandDirection =
                        (rendererNode["expandDirection"]?.GetValue<string>() ?? "auto").ToLowerInvariant();
                    if (expandDirection is not ("auto" or "up" or "down"))
                    {
                        throw new InvalidDataException(
                            "renderer.expandDirection must be auto, up, or down.");
                    }
                    var activation =
                        (rendererNode["activation"]?.GetValue<string>() ?? "hover").ToLowerInvariant();
                    if (activation is not ("hover" or "click" or "manual"))
                    {
                        throw new InvalidDataException(
                            "renderer.activation must be hover, click, or manual.");
                    }
                    webRenderer = new WebRendererDefinition(
                        webEntry,
                        expandedWidth,
                        expandedHeight,
                        expandDirection,
                        activation,
                        ReadClampedInt(rendererNode, "hoverDelayMs", 100, 0, 1000),
                        ReadClampedInt(rendererNode, "collapseDelayMs", 180, 0, 2000),
                        ReadClampedInt(rendererNode, "transitionMs", 280, 0, 500));
                    renderer = "web";
                }
                else
                {
                    throw new InvalidDataException(
                        "schemaVersion 4 renderer.type must be declarative, native, or web.");
                }
            }

            CommunityProviderDefinition? provider = null;
            if (entry["provider"] is JsonObject providerNode)
            {
                var type = RequiredString(providerNode, "type").ToLowerInvariant();
                if (type is not ("javascript" or "static" or "clock" or "http-json"))
                {
                    throw new InvalidDataException($"Unsupported provider type '{type}'.");
                }
                string? providerRelativePath = providerNode["path"]?.GetValue<string>();
                if (type == "javascript")
                {
                    if (string.IsNullOrWhiteSpace(providerRelativePath))
                    {
                        throw new InvalidDataException("A JavaScript provider requires entry.provider.path.");
                    }
                    ResolveContainedFile(directory, providerRelativePath, ".js");
                }
                var refreshSeconds = Math.Clamp(
                    providerNode["refreshSeconds"]?.GetValue<double?>() ?? 3d, 1d, 3600d);
                provider = new CommunityProviderDefinition(
                    type, providerRelativePath, refreshSeconds,
                    (JsonObject)providerNode.DeepClone());
            }

            var permissions = manifest["permissions"]?.DeepClone().AsObject() ?? new JsonObject();
            if (schemaVersion == 4)
            {
                CommunityPermissions.ValidateV4(permissions);
            }
            else
            {
                ValidatePermissions(permissions);
            }

            CommunityProcessRuntimeDefinition? processRuntime = null;
            if (manifest["runtime"] is JsonObject runtimeNode)
            {
                if (schemaVersion != 4)
                {
                    throw new InvalidDataException("A process runtime requires schemaVersion 4.");
                }
                if (!string.Equals(
                        RequiredString(runtimeNode, "type"),
                        "process",
                        StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidDataException("runtime.type must be process.");
                }
                var runtimeEntry = RequiredString(runtimeNode, "entry");
                ResolveContainedPath(directory, runtimeEntry);
                var arguments = runtimeNode["arguments"] is JsonArray argumentNodes
                    ? argumentNodes.Select(node => node?.GetValue<string>() ?? "").ToArray()
                    : [];
                if (arguments.Length > 64 || arguments.Any(argument => argument.Length > 1024))
                {
                    throw new InvalidDataException("runtime.arguments exceeds the supported limits.");
                }
                var protocol =
                    (runtimeNode["protocol"]?.GetValue<string>() ?? "json-lines-v1").ToLowerInvariant();
                var lifetime =
                    (runtimeNode["lifetime"]?.GetValue<string>() ?? "persistent").ToLowerInvariant();
                var runAs = (runtimeNode["runAs"]?.GetValue<string>() ?? "user").ToLowerInvariant();
                var workingDirectory =
                    (runtimeNode["workingDirectory"]?.GetValue<string>() ?? "package").ToLowerInvariant();
                if (protocol is not ("none" or "json-lines-v1") ||
                    lifetime != "persistent" ||
                    runAs is not ("user" or "administrator") ||
                    workingDirectory is not ("package" or "data"))
                {
                    throw new InvalidDataException("runtime contains an unsupported option.");
                }
                if (!CommunityPermissions.Has(permissions, "system.fullAccess", includeOptional: false))
                {
                    throw new InvalidDataException(
                        "A process runtime must request required permission system.fullAccess.");
                }
                if (permissions["optional"] is JsonArray { Count: > 0 })
                {
                    throw new InvalidDataException(
                        "A full-access process cannot use optional permissions because they cannot be technically restricted.");
                }
                if (runAs == "administrator" &&
                    !CommunityPermissions.Has(permissions, "system.administrator", includeOptional: false))
                {
                    throw new InvalidDataException(
                        "An administrator runtime must request required permission system.administrator.");
                }
                if (runAs == "administrator" && protocol != "none")
                {
                    throw new InvalidDataException(
                        "Administrator runtimes currently require protocol none.");
                }
                processRuntime = new CommunityProcessRuntimeDefinition(
                    runtimeEntry,
                    arguments,
                    protocol,
                    lifetime,
                    runAs,
                    workingDirectory);
            }
            var author = manifest["author"]?.AsObject()
                         ?? throw new InvalidDataException("author is required.");
            var authorName = RequiredString(author, "name");
            var authorWebsite = author["website"]?.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(authorWebsite) &&
                (!Uri.TryCreate(authorWebsite, UriKind.Absolute, out var authorUri) ||
                 authorUri.Scheme != Uri.UriSchemeHttps))
            {
                throw new InvalidDataException("author.website must use HTTPS.");
            }
            return new RuntimeWidgetDefinition(
                id,
                version,
                RequiredString(manifest, "displayName"),
                manifest["category"]?.GetValue<string>() ?? "Community",
                RequiredString(manifest, "description"),
                authorName,
                authorWebsite,
                renderer,
                false,
                true,
                null,
                width,
                height,
                manifest["supportsMultipleInstances"]?.GetValue<bool?>() ?? false,
                directory,
                permissions,
                manifest["settings"]?.DeepClone().AsArray() ?? new JsonArray(),
                layout,
                provider,
                webRenderer,
                schemaVersion,
                processRuntime,
                contentSha256,
                nativeRenderer,
                expandedLayout);
        }
        catch (Exception ex)
        {
            return new RuntimeWidgetDefinition(
                fallbackId, "0.0.0", fallbackId, "Community", "", null, null, "declarative", false,
                false, ex.Message, 96, 24, false, directory, new JsonObject(), new JsonArray(), null, null, null);
        }
    }

    private static string ValidateDirectoryEnvelope(string directory)
    {
        if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
        {
            throw new InvalidDataException("Symbolic links and reparse points are not allowed.");
        }
        var files = Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories)
            .OrderBy(path => Path.GetRelativePath(directory, path).Replace('\\', '/'), StringComparer.Ordinal)
            .ToArray();
        if (files.Length > MaxFiles)
        {
            throw new InvalidDataException($"A widget may contain at most {MaxFiles} files.");
        }
        long bytes = 0;
        foreach (var path in files)
        {
            var info = new FileInfo(path);
            if ((info.Attributes & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException("Symbolic links and reparse points are not allowed.");
            }
            bytes += info.Length;
            if (bytes > MaxTotalBytes)
            {
            throw new InvalidDataException("A widget folder may not exceed 250 MB.");
            }
        }
        foreach (var path in Directory.EnumerateDirectories(directory, "*", SearchOption.AllDirectories))
        {
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            {
                throw new InvalidDataException("Symbolic links and reparse points are not allowed.");
            }
        }
        using var digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buffer = new byte[81920];
        foreach (var path in files)
        {
            var relative = Path.GetRelativePath(directory, path).Replace('\\', '/');
            digest.AppendData(Encoding.UTF8.GetBytes(relative));
            digest.AppendData([0]);
            using var stream = File.OpenRead(path);
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                digest.AppendData(buffer, 0, read);
            }
            digest.AppendData([0xff]);
        }
        return Convert.ToHexString(digest.GetHashAndReset()).ToLowerInvariant();
    }

    private static void ValidateLayout(JsonObject node, int depth, ref int nodeCount)
    {
        if (depth > MaxLayoutDepth || ++nodeCount > MaxLayoutNodes)
        {
            throw new InvalidDataException("layout exceeds the 8-level or 64-node limit.");
        }
        var type = RequiredString(node, "type");
        if (!LayoutTypes.Contains(type))
        {
            throw new InvalidDataException($"Unsupported layout element '{type}'.");
        }
        if (node["children"] is JsonArray children)
        {
            if (type is not ("row" or "column" or "card"))
            {
                throw new InvalidDataException($"Element '{type}' cannot contain children.");
            }
            foreach (var child in children)
            {
                if (child is not JsonObject childObject)
                {
                    throw new InvalidDataException("Every layout child must be an object.");
                }
                ValidateLayout(childObject, depth + 1, ref nodeCount);
            }
        }
        if (type.Equals("button", StringComparison.OrdinalIgnoreCase))
        {
            var action = RequiredString(node, "action");
            var reserved = action is "$close" or "$settings";
            if (!reserved &&
                (action.Length > 120 ||
                 action.Any(character =>
                     !(char.IsAsciiLetterOrDigit(character) ||
                       character is '.' or '_' or ':' or '-'))))
            {
                throw new InvalidDataException(
                    "Button actions must be $close, $settings, or a safe action id.");
            }
        }
        if (node["bind"] is JsonValue bindValue)
        {
            var binding = bindValue.GetValue<string>();
            if (!binding.StartsWith("data.", StringComparison.Ordinal) &&
                !binding.StartsWith("settings.", StringComparison.Ordinal))
            {
                throw new InvalidDataException("Bindings must start with data. or settings.");
            }
        }
    }

    private static void ValidatePermissions(JsonObject permissions)
    {
        if (permissions["network"] is JsonArray hosts)
        {
            foreach (var item in hosts)
            {
                var host = item?.GetValue<string>() ?? "";
                if (host.Length is < 1 or > 253 || host.Contains('/') || host.Contains(':') || host.Contains('*'))
                {
                    throw new InvalidDataException($"Invalid network host permission '{host}'.");
                }
            }
        }
        foreach (var property in permissions)
        {
            if (property.Key is not ("network" or "systemMetrics" or "openExternal" or "storage" or "graphics"))
            {
                throw new InvalidDataException($"Unsupported permission '{property.Key}'.");
            }
        }
        if (permissions["graphics"] is JsonArray graphics)
        {
            foreach (var item in graphics)
            {
                if (item?.GetValue<string>() is not ("webgl" or "continuousAnimation"))
                {
                    throw new InvalidDataException("graphics permissions support only webgl and continuousAnimation.");
                }
            }
        }
    }

    private static int ReadClampedInt(
        JsonObject node,
        string key,
        int fallback,
        int minimum,
        int maximum)
    {
        var value = node[key]?.GetValue<int?>() ?? fallback;
        if (value < minimum || value > maximum)
        {
            throw new InvalidDataException($"{key} must be between {minimum} and {maximum}.");
        }
        return value;
    }

    private static string ResolveContainedFile(string root, string relativePath, string extension)
    {
        if (Path.IsPathRooted(relativePath) || relativePath.Contains("..", StringComparison.Ordinal))
        {
            throw new InvalidDataException("Package paths must be relative and may not contain '..'.");
        }
        var rootPath = Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var fullPath = Path.GetFullPath(Path.Combine(root, relativePath));
        if (!fullPath.StartsWith(rootPath, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(Path.GetExtension(fullPath), extension, StringComparison.OrdinalIgnoreCase) ||
            !File.Exists(fullPath))
        {
            throw new InvalidDataException($"Package file is missing or invalid: {relativePath}");
        }
        return fullPath;
    }

    private static string ResolveContainedPath(string root, string relativePath)
    {
        if (Path.IsPathRooted(relativePath) || relativePath.Contains("..", StringComparison.Ordinal))
        {
            throw new InvalidDataException("Package paths must be relative and may not contain '..'.");
        }
        var rootPath =
            Path.GetFullPath(root).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var fullPath = Path.GetFullPath(Path.Combine(root, relativePath));
        if (!fullPath.StartsWith(rootPath, StringComparison.OrdinalIgnoreCase) ||
            !File.Exists(fullPath))
        {
            throw new InvalidDataException($"Package file is missing or invalid: {relativePath}");
        }
        return fullPath;
    }

    private static string RequiredString(JsonObject node, string key)
    {
        var value = node[key]?.GetValue<string>();
        return string.IsNullOrWhiteSpace(value)
            ? throw new InvalidDataException($"'{key}' is required.")
            : value.Trim();
    }

    private static string CurrentVersion() =>
        Assembly.GetExecutingAssembly().GetName().Version is { } version
            ? $"{version.Major}.{version.Minor}.{version.Build}"
            : "0.0.0";

    private static JsonSerializerOptions JsonOptions() => new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        WriteIndented = true
    };

    private static void Log(string message)
    {
        try
        {
            Directory.CreateDirectory(AppPaths.LogsDirectory);
            File.AppendAllText(
                Path.Combine(AppPaths.LogsDirectory, "loader.log"),
                $"[{DateTimeOffset.Now:O}] {message}{Environment.NewLine}");
        }
        catch
        {
            // Registry diagnostics must never stop the loader.
        }
    }
}
