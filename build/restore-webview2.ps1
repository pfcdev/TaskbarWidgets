param(
    [switch]$PrintPath
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$Version = "1.0.4078.44"
$ExpectedHash = "DC4D1D9168DF26B830398303E50210B6E1729F6CE5A7AC69D2C766852F489962"
$ToolsRoot = if ($env:BUILDAGENT_PROJECT_CACHE) {
    Join-Path $env:BUILDAGENT_PROJECT_CACHE "webview2\sdk"
} else {
    Join-Path $RepoRoot "artifacts\tools\webview2"
}
$PackagePath = Join-Path $ToolsRoot "Microsoft.Web.WebView2.$Version.nupkg"
$SdkRoot = Join-Path $ToolsRoot $Version

New-Item -ItemType Directory -Force $ToolsRoot | Out-Null
if (-not (Test-Path $PackagePath)) {
    $Uri = "https://api.nuget.org/v3-flatcontainer/microsoft.web.webview2/$Version/microsoft.web.webview2.$Version.nupkg"
    Invoke-WebRequest -Uri $Uri -OutFile $PackagePath -UseBasicParsing
}

$ActualHash = (Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash
if ($ActualHash -ne $ExpectedHash) {
    throw "WebView2 SDK hash mismatch. Expected $ExpectedHash but found $ActualHash."
}

$Header = Join-Path $SdkRoot "build\native\include\WebView2.h"
$Library = Join-Path $SdkRoot "build\native\x64\WebView2LoaderStatic.lib"
if (-not (Test-Path $Header) -or -not (Test-Path $Library)) {
    $TemporaryZip = "$PackagePath.zip"
    Copy-Item -LiteralPath $PackagePath -Destination $TemporaryZip -Force
    Remove-Item -LiteralPath $SdkRoot -Recurse -Force -ErrorAction SilentlyContinue
    Expand-Archive -LiteralPath $TemporaryZip -DestinationPath $SdkRoot -Force
    Remove-Item -LiteralPath $TemporaryZip -Force
}

if (-not (Test-Path $Header) -or -not (Test-Path $Library)) {
    throw "WebView2 native SDK files are missing after restore."
}

if ($PrintPath) {
    Write-Output $SdkRoot
} else {
    Write-Host "WebView2 SDK $Version restored and verified: $SdkRoot"
}
