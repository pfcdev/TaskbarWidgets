param(
    [ValidateSet("Build", "Verify", "Package", "Release")]
    [string]$Target = "Build",
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version,
    [switch]$InstallDependencies,
    [switch]$Draft,
    [switch]$Prerelease
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path $MyInvocation.MyCommand.Path -Parent
$SourceVersion = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
if (-not $Version) { $Version = $SourceVersion }
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "VERSION must contain a numeric version." }
if ($Version -ne $SourceVersion) { throw "-Version must match the root VERSION file ($SourceVersion)." }

& (Join-Path $RepoRoot "build\sync-version.ps1")
& (Join-Path $RepoRoot "build\generate-widget-catalog.ps1")

if ($Target -eq "Verify") {
    & (Join-Path $RepoRoot "build\verify.ps1") -Configuration $Configuration
    return
}

& (Join-Path $RepoRoot "build\build-product.ps1") -Configuration $Configuration -Version $Version

if ($Target -in @("Package", "Release")) {
    & (Join-Path $RepoRoot "build\build-nsis.ps1") -Configuration $Configuration -Version $Version -SkipProductBuild -InstallNsisIfMissing:$InstallDependencies
}

if ($Target -eq "Release") {
    & (Join-Path $RepoRoot "build\create-release.ps1") -Tag "v$Version" -Draft:$Draft -Prerelease:$Prerelease -SkipBuild
}
