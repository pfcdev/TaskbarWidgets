param(
    [string]$Tag = "",
    [string]$Repo = "pfcdev/TaskbarWidgets",
    [switch]$Draft,
    [switch]$Prerelease,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($Tag)) {
    $Tag = "v$((Get-Content (Join-Path $RepoRoot 'VERSION') -Raw).Trim())"
}
$Version = $Tag.TrimStart("v", "V")
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Tag must be vMAJOR.MINOR.PATCH." }

if (-not $SkipBuild) {
    & (Join-Path $RepoRoot "build.ps1") -Target Package -Configuration Release -Version $Version
}

$ArtifactDir = Join-Path $RepoRoot "artifacts"
$Files = @(
    (Join-Path $ArtifactDir "TaskbarWidgetsSetup-x64.exe"),
    (Join-Path $ArtifactDir "TaskbarWidgetsSetup-x64.exe.sha256"),
    (Join-Path $ArtifactDir "TaskbarWidgets-portable-x64.zip"),
    (Join-Path $ArtifactDir "TaskbarWidgets-portable-x64.zip.sha256"),
    (Join-Path $ArtifactDir "release-manifest.json")
)
foreach ($file in $Files) { if (-not (Test-Path $file)) { throw "Release artifact missing: $file" } }
if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw "GitHub CLI is required to publish a release." }

$unsigned = -not [bool]$env:WINDOWS_SIGNING_CERT_BASE64
$releaseNotesPath = Join-Path $RepoRoot "RELEASE_NOTES.md"
$notes = if (Test-Path $releaseNotesPath) {
    (Get-Content -LiteralPath $releaseNotesPath -Raw).Trim()
} else {
    "Taskbar Widgets $Tag for Windows 11 x64. TaskbarWidgetsSetup-x64.exe is the single installer for installation and updates; existing user data is migrated and preserved."
}
if ($unsigned) { $notes += " This build is unsigned and Windows SmartScreen may show a warning." }

gh release view $Tag --repo $Repo *> $null
if ($LASTEXITCODE -eq 0) {
    gh release upload $Tag @Files --repo $Repo --clobber
} else {
    $Arguments = @("release", "create", $Tag) + $Files + @("--repo", $Repo, "--title", "Taskbar Widgets $Tag", "--notes", $notes)
    if ($Draft) { $Arguments += "--draft" }
    if ($Prerelease) { $Arguments += "--prerelease" }
    gh @Arguments
}
if ($LASTEXITCODE -ne 0) { throw "GitHub release publish failed." }
