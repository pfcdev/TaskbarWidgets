using System.Text.Json.Nodes;

namespace TaskbarWidgets.Loader.Core;

internal sealed record CommunityPermissionRequest(
    string Id,
    bool Optional,
    JsonNode? Scope,
    string Reason);

internal static class CommunityPermissions
{
    internal static readonly HashSet<string> KnownIds = new(StringComparer.Ordinal)
    {
        "accounts.list.read",
        "accounts.profile.read",
        "accounts.history.read",
        "accounts.tokens.read",
        "accounts.active.write",
        "accounts.delete",
        "filesystem.read",
        "filesystem.write",
        "filesystem.delete",
        "filesystem.watch",
        "filesystem.all",
        "registry.read",
        "registry.write",
        "registry.delete",
        "registry.all",
        "process.list",
        "process.start",
        "process.stop",
        "process.control",
        "process.inject",
        "shell.execute",
        "shell.openExternal",
        "network.internet",
        "network.local",
        "network.listen",
        "network.unrestricted",
        "windows.win32",
        "windows.winrt",
        "windows.com",
        "windows.wmi",
        "clipboard.read",
        "clipboard.write",
        "notifications.show",
        "camera",
        "microphone",
        "location",
        "bluetooth",
        "usb",
        "media.sessions.read",
        "media.playback.control",
        "steam.downloads.read",
        "steam.client.control",
        "discord.state.read",
        "system.metrics.read",
        "taskbar.control",
        "settings.read",
        "settings.write",
        "system.fullAccess",
        "system.administrator",
        "system.startup",
        "system.background"
    };

    internal static IReadOnlyList<CommunityPermissionRequest> ValidateV4(JsonObject permissions)
    {
        foreach (var property in permissions)
        {
            if (property.Key is not ("required" or "optional"))
            {
                throw new InvalidDataException($"Unsupported schema v4 permissions property '{property.Key}'.");
            }
        }

        if (permissions["required"] is not JsonArray required)
        {
            throw new InvalidDataException("schema v4 permissions.required must be an array.");
        }

        var result = new List<CommunityPermissionRequest>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        ReadList(required, optional: false, result, seen);
        if (permissions["optional"] is JsonArray optional)
        {
            ReadList(optional, optional: true, result, seen);
        }
        else if (permissions.ContainsKey("optional") && permissions["optional"] is not null)
        {
            throw new InvalidDataException("schema v4 permissions.optional must be an array.");
        }
        return result;
    }

    internal static bool Has(JsonObject permissions, string id, bool includeOptional = true)
    {
        try
        {
            return Requests(permissions).Any(request =>
                string.Equals(request.Id, id, StringComparison.Ordinal) &&
                (includeOptional || !request.Optional));
        }
        catch
        {
            return false;
        }
    }

    internal static IReadOnlyList<CommunityPermissionRequest> Requests(JsonObject permissions)
    {
        if (permissions["required"] is not JsonArray)
        {
            return [];
        }
        return ValidateV4(permissions);
    }

    internal static bool IsApproved(RuntimeWidgetDefinition definition)
    {
        if (definition.ManifestSchemaVersion < 4)
        {
            return true;
        }
        try
        {
            var approval = ReadApproval(definition);
            return approval?["schemaVersion"]?.GetValue<int?>() == 1 &&
                   string.Equals(
                       approval["widgetId"]?.GetValue<string>(),
                       definition.Id,
                       StringComparison.OrdinalIgnoreCase) &&
                   string.Equals(
                       approval["version"]?.GetValue<string>(),
                       definition.Version,
                       StringComparison.Ordinal) &&
                   JsonNode.DeepEquals(approval["permissions"], definition.Permissions) &&
                   !string.IsNullOrWhiteSpace(definition.ContentSha256) &&
                   string.Equals(
                       approval["contentSha256"]?.GetValue<string>(),
                       definition.ContentSha256,
                       StringComparison.OrdinalIgnoreCase);
        }
        catch
        {
            return false;
        }
    }

    internal static JsonObject Effective(RuntimeWidgetDefinition definition)
    {
        if (definition.ManifestSchemaVersion < 4)
        {
            return (JsonObject)definition.Permissions.DeepClone();
        }
        var result = new JsonObject
        {
            ["required"] = definition.Permissions["required"]?.DeepClone() ?? new JsonArray(),
            ["optional"] = new JsonArray()
        };
        if (!IsApproved(definition))
        {
            return result;
        }
        var granted = ReadApproval(definition)?["grantedOptional"] is JsonArray values
            ? values.Select(value => value?.GetValue<string>())
                .Where(value => !string.IsNullOrWhiteSpace(value))
                .Cast<string>()
                .ToHashSet(StringComparer.Ordinal)
            : [];
        var optional = (JsonArray)result["optional"]!;
        foreach (var request in definition.Permissions["optional"] as JsonArray ?? [])
        {
            if (request is JsonObject item &&
                item["id"]?.GetValue<string>() is { } id &&
                granted.Contains(id))
            {
                optional.Add(item.DeepClone());
            }
        }
        return result;
    }

    private static JsonObject? ReadApproval(RuntimeWidgetDefinition definition)
    {
        var path = Path.Combine(
            AppPaths.CommunityWidgetApprovalsDirectory,
            definition.Id + ".json");
        return JsonNode.Parse(File.ReadAllText(path))?.AsObject();
    }

    internal static IReadOnlyCollection<string> NetworkHosts(JsonObject permissions)
    {
        if (permissions["required"] is not JsonArray)
        {
            return permissions["network"] is JsonArray legacy
                ? legacy.Select(node => node?.GetValue<string>())
                    .Where(value => !string.IsNullOrWhiteSpace(value))
                    .Cast<string>()
                    .ToArray()
                : [];
        }
        return Requests(permissions)
            .Where(request => request.Id == "network.internet")
            .SelectMany(request => ScopeStrings(request.Scope))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    internal static bool HasUnrestrictedNetwork(JsonObject permissions) =>
        permissions["required"] is JsonArray &&
        Has(permissions, "network.unrestricted");

    internal static IReadOnlyCollection<string> SystemMetrics(JsonObject permissions)
    {
        if (permissions["required"] is not JsonArray)
        {
            return permissions["systemMetrics"] is JsonArray legacy
                ? legacy.Select(node => node?.GetValue<string>())
                    .Where(value => !string.IsNullOrWhiteSpace(value))
                    .Cast<string>()
                    .ToArray()
                : [];
        }
        var scopes = Requests(permissions)
            .Where(request => request.Id == "system.metrics.read")
            .SelectMany(request => ScopeStrings(request.Scope))
            .Where(value => value is "cpu" or "storage" or "network" or "memory")
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToArray();
        return scopes.Length == 0 && Has(permissions, "system.metrics.read")
            ? ["cpu", "storage", "network", "memory"]
            : scopes;
    }

    private static void ReadList(
        JsonArray values,
        bool optional,
        List<CommunityPermissionRequest> result,
        HashSet<string> seen)
    {
        if (values.Count > 64)
        {
            throw new InvalidDataException("A widget may request at most 64 permissions per list.");
        }
        foreach (var node in values)
        {
            if (node is not JsonObject request)
            {
                throw new InvalidDataException("Every permission request must be an object.");
            }
            foreach (var property in request)
            {
                if (property.Key is not ("id" or "scope" or "reason"))
                {
                    throw new InvalidDataException(
                        $"Unsupported permission request property '{property.Key}'.");
                }
            }
            var id = request["id"]?.GetValue<string>() ?? "";
            if (!KnownIds.Contains(id))
            {
                throw new InvalidDataException($"Unknown permission id '{id}'.");
            }
            if (!seen.Add(id))
            {
                throw new InvalidDataException($"Permission '{id}' is requested more than once.");
            }
            var reason = request["reason"]?.GetValue<string>()?.Trim() ?? "";
            if (reason.Length is < 3 or > 300)
            {
                throw new InvalidDataException(
                    $"Permission '{id}' reason must be between 3 and 300 characters.");
            }
            if (request["scope"] is JsonValue scopeValue)
            {
                var scope = scopeValue.GetValue<string>();
                if (scope.Length is < 1 or > 1024)
                {
                    throw new InvalidDataException($"Permission '{id}' has an invalid scope.");
                }
            }
            else if (request["scope"] is JsonArray scopeArray)
            {
                if (scopeArray.Count > 64 ||
                    scopeArray.Any(item => item is not JsonValue ||
                        (item.GetValue<string>()?.Length ?? 0) is < 1 or > 1024))
                {
                    throw new InvalidDataException($"Permission '{id}' has an invalid scope list.");
                }
            }
            else if (request.ContainsKey("scope") && request["scope"] is not null)
            {
                throw new InvalidDataException($"Permission '{id}' scope must be a string or array.");
            }
            result.Add(new CommunityPermissionRequest(
                id,
                optional,
                request["scope"]?.DeepClone(),
                reason));
        }
    }

    private static IEnumerable<string> ScopeStrings(JsonNode? scope)
    {
        if (scope is JsonValue value)
        {
            var text = value.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(text))
            {
                yield return text;
            }
        }
        else if (scope is JsonArray array)
        {
            foreach (var node in array)
            {
                var text = node?.GetValue<string>();
                if (!string.IsNullOrWhiteSpace(text))
                {
                    yield return text;
                }
            }
        }
    }
}
