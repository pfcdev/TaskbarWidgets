param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = "0.1.0",
    [switch]$SkipProductBuild,
    [switch]$InstallNsisIfMissing
)

$ErrorActionPreference = "Stop"

$NsisBuildScript = Join-Path $PSScriptRoot "build-nsis.ps1"
$Arguments = @(
    "-ExecutionPolicy", "Bypass",
    "-File", $NsisBuildScript,
    "-Configuration", $Configuration,
    "-Version", $Version
)
if ($SkipProductBuild) {
    $Arguments += "-SkipProductBuild"
}
if ($InstallNsisIfMissing) {
    $Arguments += "-InstallNsisIfMissing"
}

powershell @Arguments
if ($LASTEXITCODE -ne 0) {
    throw "NSIS installer build failed with exit code $LASTEXITCODE."
}
