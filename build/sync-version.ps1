param(
    [switch]$Check
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Version = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "VERSION must contain a numeric version." }
$AssemblyVersion = if ($Version.Split('.').Count -eq 3) { "$Version.0" } else { $Version }

$updates = @(
    @{ Path = "src\loader\TaskbarWidgets.csproj"; Pattern = '<Version>[^<]+</Version>'; Replacement = "<Version>$Version</Version>" },
    @{ Path = "src\loader\TaskbarWidgets.csproj"; Pattern = '<AssemblyVersion>[^<]+</AssemblyVersion>'; Replacement = "<AssemblyVersion>$AssemblyVersion</AssemblyVersion>" },
    @{ Path = "src\loader\TaskbarWidgets.csproj"; Pattern = '<FileVersion>[^<]+</FileVersion>'; Replacement = "<FileVersion>$AssemblyVersion</FileVersion>" },
    @{ Path = "src\settings\src-tauri\Cargo.toml"; Pattern = '(?m)^version = "[^"]+"'; Replacement = "version = `"$Version`"" },
    @{ Path = "src\settings\src-tauri\tauri.conf.json"; Pattern = '(?m)^(\s*)"version": "[^"]+"'; Replacement = "`$1`"version`": `"$Version`"" },
    @{ Path = "src\native\CMakeLists.txt"; Pattern = 'project\(TaskbarWidgetsNative VERSION [^ ]+'; Replacement = "project(TaskbarWidgetsNative VERSION $Version" },
    @{ Path = "src\settings\dist\app.js"; Pattern = 'const current = update\.currentVersion \|\| "[^"]+";'; Replacement = "const current = update.currentVersion || `"$Version`";" },
    @{ Path = "widgets\codex-status\provider\CodexStatusWorker.cs"; Pattern = '(?m)^(\s*)version = "[^"]+"$'; Replacement = "`$1version = `"$Version`"" }
)

$changed = @()
foreach ($update in $updates) {
    $path = Join-Path $RepoRoot $update.Path
    $current = [IO.File]::ReadAllText($path)
    $next = [Text.RegularExpressions.Regex]::Replace($current, $update.Pattern, $update.Replacement, 1)
    if ($next -eq $current -and $current -notmatch [regex]::Escape($Version)) {
        throw "Version field was not found in $($update.Path)."
    }
    if ($next -ne $current) {
        $changed += $update.Path
        if (-not $Check) { [IO.File]::WriteAllText($path, $next, [Text.UTF8Encoding]::new($false)) }
    }
}

if ($Check -and $changed.Count -gt 0) {
    throw "Version metadata is out of sync with VERSION: $($changed -join ', ')"
}
Write-Host "Version metadata validated: $Version"
