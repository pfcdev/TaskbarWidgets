param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = "0.1.0",
    [switch]$SkipProductBuild
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Version must be numeric SemVer-like text such as 0.1.0 or 0.1.0.1."
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$ProductBuildScript = Join-Path $RepoRoot "scripts\build-product.ps1"
$ProductDir = Join-Path $RepoRoot "artifacts\TaskbarStats"
$MsiProject = Join-Path $RepoRoot "product\TaskbarStatsMsi\TaskbarStatsMsi.wixproj"
$GeneratedDir = Join-Path $RepoRoot "product\TaskbarStatsMsi\Generated"
$GeneratedWxs = Join-Path $GeneratedDir "Product.wxs"
$ArtifactDir = Join-Path $RepoRoot "artifacts"
$StagingRoot = Join-Path $RepoRoot "artifacts\msi"
$PackageRoot = Join-Path $StagingRoot "package"
$BuildOutputDir = Join-Path $StagingRoot "build"
$MsiOutput = Join-Path $ArtifactDir "TaskbarStats.msi"
$MsiSha = Join-Path $ArtifactDir "TaskbarStats.msi.sha256"

function Stop-TaskbarStatsProcesses {
    foreach ($ProcessName in @("TaskbarStats", "TaskbarStatsMediaHelper", "TaskbarStatsSettings")) {
        Get-Process $ProcessName -ErrorAction SilentlyContinue |
            Stop-Process -Force -ErrorAction SilentlyContinue
    }
}

function ConvertTo-WixXmlText {
    param([string]$Value)
    return [Security.SecurityElement]::Escape($Value)
}

function Get-StableWixId {
    param(
        [string]$Prefix,
        [string]$Value
    )

    $Sha = [Security.Cryptography.SHA1]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes($Value.ToLowerInvariant())
        $Hash = [BitConverter]::ToString($Sha.ComputeHash($Bytes)).Replace("-", "").Substring(0, 16)
        return "$Prefix$Hash"
    } finally {
        $Sha.Dispose()
    }
}

function Get-StableGuid {
    param([string]$Value)

    $Md5 = [Security.Cryptography.MD5]::Create()
    try {
        $Bytes = [Text.Encoding]::UTF8.GetBytes("TaskbarStats MSI|$($Value.ToLowerInvariant())")
        $Hash = [BitConverter]::ToString($Md5.ComputeHash($Bytes)).Replace("-", "")
        return "{0}-{1}-{2}-{3}-{4}" -f `
            $Hash.Substring(0, 8),
            $Hash.Substring(8, 4),
            $Hash.Substring(12, 4),
            $Hash.Substring(16, 4),
            $Hash.Substring(20, 12)
    } finally {
        $Md5.Dispose()
    }
}

function ConvertTo-WixSourcePath {
    param(
        [string]$Root,
        [string]$RelativePath
    )

    return [IO.Path]::GetFullPath((Join-Path $Root $RelativePath))
}

function Get-PortableRelativePath {
    param(
        [string]$BasePath,
        [string]$TargetPath
    )

    $BaseFullPath = [IO.Path]::GetFullPath($BasePath).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    $TargetFullPath = [IO.Path]::GetFullPath($TargetPath)
    $BaseUri = [Uri]$BaseFullPath
    $TargetUri = [Uri]$TargetFullPath
    return [Uri]::UnescapeDataString($BaseUri.MakeRelativeUri($TargetUri).ToString()).Replace('/', '\')
}

function Write-PackageWxs {
    param(
        [string]$Root,
        [string]$OutputPath,
        [string]$ProductVersion
    )

    $Lines = New-Object System.Collections.Generic.List[string]
    $ComponentIds = New-Object System.Collections.Generic.List[string]
    $ExeFileId = "fil_TaskbarStatsExe"
    $SettingsFileId = "fil_TaskbarStatsSettingsExe"

    function Add-Line {
        param([int]$Level, [string]$Text)
        $Lines.Add(("  " * $Level) + $Text)
    }

    function Add-RemoveFolderComponent {
        param(
            [string]$DirectoryToken,
            [string]$DirectoryId,
            [int]$Level
        )

        $ComponentId = Get-StableWixId "cmp_remove_" $DirectoryToken
        $ComponentGuid = Get-StableGuid "remove|$DirectoryToken"
        $RegistryName = Get-StableWixId "dir_" $DirectoryToken
        $RemoveFolderId = Get-StableWixId "rm_" $DirectoryToken
        Add-Line $Level "<Component Id=""$ComponentId"" Guid=""$ComponentGuid"">"
        Add-Line ($Level + 1) "<RemoveFolder Id=""$RemoveFolderId"" Directory=""$DirectoryId"" On=""uninstall"" />"
        Add-Line ($Level + 1) "<RegistryValue Root=""HKCU"" Key=""Software\TaskbarStats\InstalledDirectories"" Name=""$RegistryName"" Type=""integer"" Value=""1"" KeyPath=""yes"" />"
        Add-Line $Level "</Component>"
        $ComponentIds.Add($ComponentId)
    }

    function Add-DirectoryTree {
        param(
            [string]$DirectoryPath,
            [string]$DirectoryId,
            [string]$DirectoryToken,
            [int]$Level
        )

        $ChildDirectories = Get-ChildItem -LiteralPath $DirectoryPath -Directory | Sort-Object Name
        foreach ($ChildDirectory in $ChildDirectories) {
            $RelativeDirectory = Get-PortableRelativePath -BasePath $Root -TargetPath $ChildDirectory.FullName
            $ChildId = Get-StableWixId "dir_" $RelativeDirectory
            $Name = ConvertTo-WixXmlText $ChildDirectory.Name
            Add-Line $Level "<Directory Id=""$ChildId"" Name=""$Name"">"
            Add-DirectoryTree -DirectoryPath $ChildDirectory.FullName -DirectoryId $ChildId -DirectoryToken $RelativeDirectory -Level ($Level + 1)
            Add-Line $Level "</Directory>"
        }

        $Files = Get-ChildItem -LiteralPath $DirectoryPath -File | Sort-Object Name
        foreach ($File in $Files) {
            $RelativeFile = Get-PortableRelativePath -BasePath $Root -TargetPath $File.FullName
            $ComponentId = Get-StableWixId "cmp_" $RelativeFile
            $FileId = switch ($RelativeFile.Replace('\', '/').ToLowerInvariant()) {
                "taskbarstats.exe" { $ExeFileId }
                "taskbarstatssettings.exe" { $SettingsFileId }
                default { Get-StableWixId "fil_" $RelativeFile }
            }
            $Source = ConvertTo-WixXmlText (ConvertTo-WixSourcePath -Root $Root -RelativePath $RelativeFile)
            $ComponentGuid = Get-StableGuid "file|$RelativeFile"
            Add-Line $Level "<Component Id=""$ComponentId"" Guid=""$ComponentGuid"">"
            Add-Line ($Level + 1) "<File Id=""$FileId"" Source=""$Source"" />"
            Add-Line ($Level + 1) "<RegistryValue Root=""HKCU"" Key=""Software\TaskbarStats\InstalledFiles"" Name=""$ComponentId"" Type=""string"" Value=""$([Security.SecurityElement]::Escape($RelativeFile))"" KeyPath=""yes"" />"
            Add-Line $Level "</Component>"
            $ComponentIds.Add($ComponentId)
        }

        Add-RemoveFolderComponent -DirectoryToken $DirectoryToken -DirectoryId $DirectoryId -Level $Level
    }

    Add-Line 0 "<?xml version=""1.0"" encoding=""utf-8""?>"
    Add-Line 0 "<Wix xmlns=""http://wixtoolset.org/schemas/v4/wxs"">"
    Add-Line 1 "<Package Name=""TaskbarStats"" Manufacturer=""TaskbarStats"" Version=""$ProductVersion"" UpgradeCode=""{5DBBE4B8-FA55-4E35-9F1D-4B68D8E5D1E6}"" Scope=""perUser"" InstallerVersion=""500"" Compressed=""yes"">"
    Add-Line 2 "<MajorUpgrade DowngradeErrorMessage=""A newer version of TaskbarStats is already installed."" />"
    Add-Line 2 "<MediaTemplate EmbedCab=""yes"" />"
    Add-Line 2 "<CustomAction Id=""DetachTaskbarStats"" FileRef=""$ExeFileId"" ExeCommand=""--detach"" Execute=""deferred"" Return=""ignore"" Impersonate=""yes"" />"
    Add-Line 2 "<StandardDirectory Id=""LocalAppDataFolder"">"
    Add-Line 3 "<Directory Id=""ProgramsFolderTaskbarStats"" Name=""Programs"">"
    Add-Line 4 "<Directory Id=""INSTALLFOLDER"" Name=""TaskbarStats"">"
    Add-DirectoryTree -DirectoryPath $Root -DirectoryId "INSTALLFOLDER" -DirectoryToken "INSTALLFOLDER" -Level 5
    Add-Line 5 "<Component Id=""cmp_TaskbarStatsStartup"" Guid=""$(Get-StableGuid 'startup')"">"
    Add-Line 6 "<RegistryValue Root=""HKCU"" Key=""Software\Microsoft\Windows\CurrentVersion\Run"" Name=""TaskbarStats"" Type=""string"" Value=""[#${ExeFileId}]"" KeyPath=""yes"" />"
    Add-Line 5 "</Component>"
    $ComponentIds.Add("cmp_TaskbarStatsStartup")
    Add-Line 4 "</Directory>"
    Add-RemoveFolderComponent -DirectoryToken "ProgramsFolderTaskbarStats" -DirectoryId "ProgramsFolderTaskbarStats" -Level 4
    Add-Line 3 "</Directory>"
    Add-Line 2 "</StandardDirectory>"
    Add-Line 2 "<StandardDirectory Id=""ProgramMenuFolder"">"
    Add-Line 3 "<Directory Id=""ProgramMenuTaskbarStats"" Name=""TaskbarStats"">"
    Add-Line 4 "<Component Id=""cmp_TaskbarStatsStartMenu"" Guid=""$(Get-StableGuid 'start-menu')"">"
    Add-Line 5 "<Shortcut Id=""TaskbarStatsShortcut"" Name=""TaskbarStats"" Target=""[#${ExeFileId}]"" WorkingDirectory=""INSTALLFOLDER"" />"
    Add-Line 5 "<Shortcut Id=""TaskbarStatsSettingsShortcut"" Name=""TaskbarStats Settings"" Target=""[#${SettingsFileId}]"" WorkingDirectory=""INSTALLFOLDER"" />"
    Add-Line 5 "<RemoveFolder Id=""RemoveProgramMenuTaskbarStats"" On=""uninstall"" />"
    Add-Line 5 "<RegistryValue Root=""HKCU"" Key=""Software\TaskbarStats"" Name=""StartMenuShortcuts"" Type=""integer"" Value=""1"" KeyPath=""yes"" />"
    Add-Line 4 "</Component>"
    Add-Line 3 "</Directory>"
    Add-Line 2 "</StandardDirectory>"
    $ComponentIds.Add("cmp_TaskbarStatsStartMenu")
    Add-Line 2 "<Feature Id=""MainFeature"" Title=""TaskbarStats"" Level=""1"">"
    foreach ($ComponentId in $ComponentIds) {
        Add-Line 3 "<ComponentRef Id=""$ComponentId"" />"
    }
    Add-Line 2 "</Feature>"
    Add-Line 2 "<InstallExecuteSequence>"
    Add-Line 3 "<Custom Action=""DetachTaskbarStats"" Before=""RemoveFiles"" Condition=""REMOVE=&quot;ALL&quot;"" />"
    Add-Line 2 "</InstallExecuteSequence>"
    Add-Line 1 "</Package>"
    Add-Line 0 "</Wix>"

    New-Item -ItemType Directory -Force (Split-Path $OutputPath -Parent) | Out-Null
    Set-Content -Path $OutputPath -Value $Lines -Encoding UTF8
}

Stop-TaskbarStatsProcesses

if (-not $SkipProductBuild) {
    powershell -ExecutionPolicy Bypass -File $ProductBuildScript -Configuration $Configuration -Version $Version
}

Stop-TaskbarStatsProcesses

if (-not (Test-Path (Join-Path $ProductDir "TaskbarStats.exe"))) {
    throw "Product artifact was not found. Run scripts\build-product.ps1 first."
}

Remove-Item -Recurse -Force $StagingRoot -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $PackageRoot | Out-Null
New-Item -ItemType Directory -Force $BuildOutputDir | Out-Null

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

Write-PackageWxs -Root $PackageRoot -OutputPath $GeneratedWxs -ProductVersion $Version

dotnet build $MsiProject `
    -c $Configuration `
    -p:ProductVersion=$Version `
    -p:PackageRoot=$PackageRoot `
    -o $BuildOutputDir
if ($LASTEXITCODE -ne 0) {
    throw "MSI build failed with exit code $LASTEXITCODE."
}

$BuiltMsi = Get-ChildItem -Path $BuildOutputDir -Filter "*.msi" -File |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if ($null -eq $BuiltMsi) {
    throw "MSI output was not found in $BuildOutputDir."
}

Copy-Item -Force $BuiltMsi.FullName $MsiOutput
$Hash = (Get-FileHash $MsiOutput -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -Path $MsiSha -Value "$Hash  TaskbarStats.msi" -Encoding ASCII

Write-Host "MSI output:"
Get-Item $MsiOutput, $MsiSha | Select-Object FullName, Length, LastWriteTime
