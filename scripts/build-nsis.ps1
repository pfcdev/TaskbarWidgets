param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = "0.1.0",
    [switch]$SkipProductBuild,
    [switch]$InstallNsisIfMissing
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Version must be numeric SemVer-like text such as 0.1.0 or 0.1.0.1."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProductBuildScript = Join-Path $RepoRoot "scripts\build-product.ps1"
$ProductDir = Join-Path $RepoRoot "artifacts\TaskbarStats"
$ArtifactDir = Join-Path $RepoRoot "artifacts"
$StagingRoot = Join-Path $RepoRoot "artifacts\nsis"
$PackageRoot = Join-Path $StagingRoot "package"
$InstallerOutput = Join-Path $ArtifactDir "TaskbarStatsSetup.exe"
$InstallerSha = Join-Path $ArtifactDir "TaskbarStatsSetup.exe.sha256"
$NsisScript = Join-Path $RepoRoot "product\TaskbarStatsNsis\TaskbarStats.nsi"
$IconFile = Join-Path $RepoRoot "product\TaskbarStatsSettingsTauri\src-tauri\icons\icon.ico"

function Test-SubPath {
    param(
        [string]$BasePath,
        [string]$TargetPath
    )

    $BaseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $TargetFullPath = [IO.Path]::GetFullPath($TargetPath).TrimEnd('\', '/')
    return $TargetFullPath.StartsWith($BaseFullPath, [StringComparison]::OrdinalIgnoreCase)
}

function Find-Makensis {
    $Command = Get-Command makensis -ErrorAction SilentlyContinue
    if ($Command) {
        return $Command.Source
    }

    $Candidates = @(
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
        (Join-Path $RepoRoot "artifacts\tools\nsis\makensis.exe")
    )

    foreach ($Candidate in $Candidates) {
        if ($Candidate -and (Test-Path $Candidate)) {
            return $Candidate
        }
    }

    return $null
}

function Install-Nsis {
    if (Get-Command choco -ErrorAction SilentlyContinue) {
        choco install nsis -y --no-progress
        if ($LASTEXITCODE -eq 0) {
            return
        }
        Write-Warning "Chocolatey could not install NSIS. Trying winget."
    }

    if (Get-Command winget -ErrorAction SilentlyContinue) {
        winget install --id NSIS.NSIS --exact --accept-package-agreements --accept-source-agreements
        if ($LASTEXITCODE -eq 0) {
            return
        }
    }

    throw "NSIS was not found. Install NSIS or put makensis.exe on PATH."
}

if (-not $SkipProductBuild) {
    powershell -ExecutionPolicy Bypass -File $ProductBuildScript -Configuration $Configuration -Version $Version
}

$Makensis = Find-Makensis
if (-not $Makensis -and $InstallNsisIfMissing) {
    Install-Nsis
    $Makensis = Find-Makensis
}
if (-not $Makensis) {
    throw "makensis.exe was not found. Install NSIS or run this script with -InstallNsisIfMissing."
}

foreach ($ProcessName in @("TaskbarStats", "TaskbarStatsMediaHelper", "TaskbarStatsSettings")) {
    Get-Process $ProcessName -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path (Join-Path $ProductDir "TaskbarStats.exe"))) {
    throw "Product artifact was not found. Run scripts\build-product.ps1 first."
}

if (-not (Test-SubPath -BasePath $ArtifactDir -TargetPath $StagingRoot)) {
    throw "Refusing to clean staging path outside artifacts: $StagingRoot"
}

Remove-Item -Recurse -Force $StagingRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $PackageRoot | Out-Null

$RequiredFiles = @(
    "TaskbarStats.exe",
    "TaskbarStats.exe.sha256",
    "TaskbarStatsSettings.exe",
    "TaskbarStatsMediaHelper.exe"
)

foreach ($FileName in $RequiredFiles) {
    $Source = Join-Path $ProductDir $FileName
    if (-not (Test-Path $Source)) {
        throw "Required product file is missing: $Source"
    }

    Copy-Item -Force $Source (Join-Path $PackageRoot $FileName)
}

$AssetsSource = Join-Path $RepoRoot "assets"
if (Test-Path $AssetsSource) {
    Copy-Item -Path $AssetsSource -Destination (Join-Path $PackageRoot "Assets") -Recurse -Force
}

$WidgetLibraries = Join-Path $PackageRoot "WidgetLibraries"
New-Item -ItemType Directory -Force $WidgetLibraries | Out-Null
Set-Content `
    -Path (Join-Path $WidgetLibraries "README.txt") `
    -Value "TaskbarStats widget design packs." `
    -Encoding ASCII

Remove-Item -Force $InstallerOutput, $InstallerSha -ErrorAction SilentlyContinue

$NsisArgs = @(
    "/DVERSION=$Version",
    "/DPACKAGE_ROOT=$PackageRoot",
    "/DOUTPUT_FILE=$InstallerOutput"
)
if (Test-Path $IconFile) {
    $NsisArgs += "/DICON_FILE=$IconFile"
}
$NsisArgs += $NsisScript

& $Makensis @NsisArgs
if ($LASTEXITCODE -ne 0) {
    throw "NSIS installer build failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path $InstallerOutput)) {
    throw "Installer output was not found: $InstallerOutput"
}

$Hash = (Get-FileHash $InstallerOutput -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $InstallerSha -Value "$Hash  TaskbarStatsSetup.exe" -Encoding ASCII

Write-Host "NSIS installer output:"
Get-Item $InstallerOutput, $InstallerSha | Select-Object FullName, Length, LastWriteTime
