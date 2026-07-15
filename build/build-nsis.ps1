param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = "",
    [switch]$SkipProductBuild,
    [switch]$InstallNsisIfMissing
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
}

if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Version must be numeric SemVer-like text such as 0.1.0 or 0.1.0.1."
}

$ProductBuildScript = Join-Path $RepoRoot "build\build-product.ps1"
$ProductDir = Join-Path $RepoRoot "artifacts\TaskbarWidgets"
$ArtifactDir = Join-Path $RepoRoot "artifacts"
$StagingRoot = Join-Path $RepoRoot "artifacts\nsis"
$PackageRoot = Join-Path $StagingRoot "package"
$InstallerOutput = Join-Path $ArtifactDir "TaskbarWidgetsSetup-x64.exe"
$InstallerSha = Join-Path $ArtifactDir "TaskbarWidgetsSetup-x64.exe.sha256"
$LegacyInstallerOutput = Join-Path $ArtifactDir "TaskbarStatsSetup.exe"
$LegacyInstallerSha = Join-Path $ArtifactDir "TaskbarStatsSetup.exe.sha256"
$PortableOutput = Join-Path $ArtifactDir "TaskbarWidgets-portable-x64.zip"
$PortableSha = Join-Path $ArtifactDir "TaskbarWidgets-portable-x64.zip.sha256"
$NsisScript = Join-Path $RepoRoot "installer\nsis\TaskbarWidgets.nsi"
$IconFile = Join-Path $RepoRoot "src\settings\src-tauri\icons\icon.ico"

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
    if ($LASTEXITCODE -ne 0) { throw "Product build failed with exit code $LASTEXITCODE." }
}

$Makensis = Find-Makensis
if (-not $Makensis -and $InstallNsisIfMissing) {
    Install-Nsis
    $Makensis = Find-Makensis
}
if (-not $Makensis) {
    throw "makensis.exe was not found. Install NSIS or run this script with -InstallNsisIfMissing."
}

foreach ($ProcessName in @("TaskbarWidgets", "TaskbarWidgets.MediaHelper", "TaskbarWidgets.Settings")) {
    Get-Process $ProcessName -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path (Join-Path $ProductDir "TaskbarWidgets.exe"))) {
    throw "Product artifact was not found. Run .\build.ps1 first."
}

& (Join-Path $PSScriptRoot "sign-artifacts.ps1") -Paths @(
    (Join-Path $ProductDir "TaskbarWidgets.exe"),
    (Join-Path $ProductDir "TaskbarWidgets.Settings.exe"),
    (Join-Path $ProductDir "TaskbarWidgets.MediaHelper.exe")
)

if (-not (Test-SubPath -BasePath $ArtifactDir -TargetPath $StagingRoot)) {
    throw "Refusing to clean staging path outside artifacts: $StagingRoot"
}

Remove-Item -Recurse -Force $StagingRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $PackageRoot | Out-Null

$RequiredFiles = @(
    "TaskbarWidgets.exe",
    "TaskbarWidgets.Settings.exe",
    "TaskbarWidgets.MediaHelper.exe",
    "README-PORTABLE.txt"
)

foreach ($FileName in $RequiredFiles) {
    $Source = Join-Path $ProductDir $FileName
    if (-not (Test-Path $Source)) {
        throw "Required product file is missing: $Source"
    }

    Copy-Item -Force $Source (Join-Path $PackageRoot $FileName)
}

Copy-Item -Path (Join-Path $ProductDir "Assets") -Destination (Join-Path $PackageRoot "Assets") -Recurse -Force
Copy-Item -Path (Join-Path $ProductDir "Widgets") -Destination (Join-Path $PackageRoot "Widgets") -Recurse -Force

Remove-Item -Force $InstallerOutput, $InstallerSha, $LegacyInstallerOutput, $LegacyInstallerSha, $PortableOutput, $PortableSha -ErrorAction SilentlyContinue

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

& (Join-Path $PSScriptRoot "sign-artifacts.ps1") -Paths @($InstallerOutput)

$Hash = (Get-FileHash $InstallerOutput -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $InstallerSha -Value "$Hash  TaskbarWidgetsSetup-x64.exe" -Encoding ASCII
Copy-Item -Force $InstallerOutput $LegacyInstallerOutput
Set-Content -Path $LegacyInstallerSha -Value "$Hash  TaskbarStatsSetup.exe" -Encoding ASCII

Compress-Archive -Path (Join-Path $ProductDir "*") -DestinationPath $PortableOutput -CompressionLevel Optimal
$PortableHash = (Get-FileHash $PortableOutput -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $PortableSha -Value "$PortableHash  TaskbarWidgets-portable-x64.zip" -Encoding ASCII

$Manifest = [ordered]@{
    schemaVersion = 1
    version = $Version
    architecture = "x64"
    unsigned = -not [bool]$env:WINDOWS_SIGNING_CERT_BASE64
    artifacts = @(
        [ordered]@{ name = "TaskbarWidgetsSetup-x64.exe"; sha256 = $Hash },
        [ordered]@{ name = "TaskbarWidgets-portable-x64.zip"; sha256 = $PortableHash }
    )
    compatibilityArtifacts = @(
        [ordered]@{
            name = "TaskbarStatsSetup.exe"
            sha256 = $Hash
            purpose = "TaskbarStats <= 0.2.7 update bridge"
        }
    )
}
$Manifest | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $ArtifactDir "release-manifest.json") -Encoding UTF8

Write-Host "NSIS installer output:"
Get-Item $InstallerOutput, $InstallerSha, $LegacyInstallerOutput, $LegacyInstallerSha, $PortableOutput, $PortableSha | Select-Object FullName, Length, LastWriteTime
