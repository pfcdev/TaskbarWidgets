param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if ([string]::IsNullOrWhiteSpace($Version)) {
    $Version = (Get-Content (Join-Path $RepoRoot "VERSION") -Raw).Trim()
}
if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Invalid version: $Version" }

$NativeSource = Join-Path $RepoRoot "src\native"
$NativeBuild = Join-Path $RepoRoot "artifacts\native-build"
$LoaderProject = Join-Path $RepoRoot "src\loader\TaskbarWidgets.csproj"
$VoiceCaptureProject = Join-Path $RepoRoot "src\voice-capture\TaskbarWidgets.VoiceCapture.csproj"
$WidgetHostProject = Join-Path $RepoRoot "src\widget-host\TaskbarWidgets.WidgetHost.csproj"
$TwDevProject = Join-Path $RepoRoot "src\twdev\twdev.csproj"
$SettingsProject = Join-Path $RepoRoot "src\settings\src-tauri"
$SettingsTargetDir = if ($env:TASKBARWIDGETS_TAURI_TARGET_DIR) {
    $env:TASKBARWIDGETS_TAURI_TARGET_DIR
} elseif ($env:BUILDAGENT_PROJECT_CACHE) {
    Join-Path $env:BUILDAGENT_PROJECT_CACHE "cargo-target"
} else {
    Join-Path $env:TEMP "taskbarwidgets-tauri-target"
}
$ResourceDir = Join-Path $RepoRoot "src\loader\Resources"
$PublishDir = Join-Path $RepoRoot "artifacts\TaskbarWidgets"
$AssemblyVersion = if ($Version.Split('.').Count -eq 3) { "$Version.0" } else { $Version }
$WebView2SdkRoot = (& (Join-Path $PSScriptRoot "restore-webview2.ps1") -PrintPath | Select-Object -Last 1).Trim()

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $RepoRoot "artifacts\tools\python-cmake\cmake\data\bin\cmake.exe"),
        (Join-Path $RepoRoot "artifacts\tools\cmake-python\cmake\data\bin\cmake.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    $roots = @(
        "C:\Program Files\Microsoft Visual Studio\2022",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022"
    )
    foreach ($root in $roots) {
        $candidate = Get-ChildItem $root -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) { return $candidate }
    }
    throw "CMake/MSVC was not found. Install Visual Studio 2022 Build Tools with Desktop development with C++."
}

& (Join-Path $PSScriptRoot "generate-widget-catalog.ps1")

Remove-Item -Recurse -Force $PublishDir -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $ResourceDir, $PublishDir, $NativeBuild | Out-Null
Get-Process "TaskbarWidgets.MediaHelper" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue
Get-Process "TaskbarWidgets.RenderHost" -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$cmake = Find-CMake
& $cmake -S $NativeSource -B $NativeBuild -A x64 -DBUILD_TESTING=ON "-DWEBVIEW2_SDK_ROOT=$WebView2SdkRoot"
if ($LASTEXITCODE -ne 0) { throw "Native configure failed." }
& $cmake --build $NativeBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }

$NativeOutput = Join-Path $NativeBuild $Configuration
$HookOutput = Join-Path $NativeOutput "TaskbarWidgets.Hook.dll"
$MediaHelperOutput = Join-Path $NativeOutput "TaskbarWidgets.MediaHelper.exe"
$RenderHostOutput = Join-Path $NativeOutput "TaskbarWidgets.RenderHost.exe"
if (-not (Test-Path $HookOutput)) { throw "Native hook output missing: $HookOutput" }
if (-not (Test-Path $MediaHelperOutput)) { throw "Media helper output missing: $MediaHelperOutput" }
if (-not (Test-Path $RenderHostOutput)) { throw "RenderHost output missing: $RenderHostOutput" }
Copy-Item -Force $HookOutput (Join-Path $ResourceDir "TaskbarWidgets.Hook.dll")

Write-Host "Building Taskbar Widgets Settings..."
cargo build --manifest-path (Join-Path $SettingsProject "Cargo.toml") --release --target-dir $SettingsTargetDir
if ($LASTEXITCODE -ne 0) { throw "Settings build failed." }
$SettingsBuildOutput = Join-Path $SettingsTargetDir "release\taskbar_widgets_settings.exe"
if (-not (Test-Path $SettingsBuildOutput)) { throw "Settings output missing: $SettingsBuildOutput" }
Copy-Item -Force $SettingsBuildOutput (Join-Path $ResourceDir "TaskbarWidgets.Settings.exe")

$VoiceCapturePublish = Join-Path $RepoRoot "artifacts\voice-capture-publish"
Remove-Item -Recurse -Force $VoiceCapturePublish -ErrorAction SilentlyContinue
Write-Host "Publishing optional Discord voice capture helper..."
dotnet publish $VoiceCaptureProject `
    -c $Configuration -r win-x64 --self-contained true `
    -p:Version=$Version -p:AssemblyVersion=$AssemblyVersion -p:FileVersion=$AssemblyVersion `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true -o $VoiceCapturePublish
if ($LASTEXITCODE -ne 0) { throw "Voice capture helper publish failed." }
$VoiceCaptureOutput = Join-Path $VoiceCapturePublish "TaskbarWidgets.VoiceCapture.exe"
if (-not (Test-Path $VoiceCaptureOutput)) { throw "Voice capture helper output missing: $VoiceCaptureOutput" }
Copy-Item -Force $VoiceCaptureOutput (Join-Path $ResourceDir "TaskbarWidgets.VoiceCapture.exe")

Write-Host "Publishing Taskbar Widgets loader..."
dotnet publish $LoaderProject `
    -c $Configuration -r win-x64 --self-contained true `
    -p:Version=$Version -p:AssemblyVersion=$AssemblyVersion -p:FileVersion=$AssemblyVersion `
    -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true -o $PublishDir
if ($LASTEXITCODE -ne 0) { throw "Loader publish failed." }

Copy-Item -Force $SettingsBuildOutput (Join-Path $PublishDir "TaskbarWidgets.Settings.exe")
Copy-Item -Force $MediaHelperOutput (Join-Path $PublishDir "TaskbarWidgets.MediaHelper.exe")
Copy-Item -Force $RenderHostOutput (Join-Path $PublishDir "TaskbarWidgets.RenderHost.exe")
Copy-Item -Force $VoiceCaptureOutput (Join-Path $PublishDir "TaskbarWidgets.VoiceCapture.exe")
Remove-Item -Recurse -Force $VoiceCapturePublish

$WebViewBootstrapper = Join-Path $PublishDir "MicrosoftEdgeWebview2Setup.exe"
if (-not (Test-Path $WebViewBootstrapper)) {
    $WebViewCacheRoot = if ($env:BUILDAGENT_PROJECT_CACHE) {
        Join-Path $env:BUILDAGENT_PROJECT_CACHE "webview2"
    } else {
        Join-Path $RepoRoot "artifacts\tools\webview2"
    }
    $CachedBootstrapper = Join-Path $WebViewCacheRoot "MicrosoftEdgeWebview2Setup.exe"
    New-Item -ItemType Directory -Path $WebViewCacheRoot -Force | Out-Null
    if (-not (Test-Path $CachedBootstrapper)) {
        Write-Host "Downloading the Microsoft WebView2 Evergreen Bootstrapper to the persistent cache..."
        Invoke-WebRequest `
            -Uri "https://go.microsoft.com/fwlink/p/?LinkId=2124703" `
            -OutFile $CachedBootstrapper `
            -UseBasicParsing
    }
    Copy-Item -LiteralPath $CachedBootstrapper -Destination $WebViewBootstrapper -Force
}
if ((Get-Item $WebViewBootstrapper).Length -lt 500000) {
    throw "WebView2 Evergreen Bootstrapper download is unexpectedly small."
}

Write-Host "Publishing sandboxed community WidgetHost..."
dotnet publish $WidgetHostProject -c $Configuration -r win-x64 --self-contained true `
    -p:Version=$Version -p:PublishSingleFile=true -o (Join-Path $PublishDir "widget-host-publish")
if ($LASTEXITCODE -ne 0) { throw "WidgetHost publish failed." }
Copy-Item -Force (Join-Path $PublishDir "widget-host-publish\TaskbarWidgets.WidgetHost.exe") (Join-Path $PublishDir "TaskbarWidgets.WidgetHost.exe")
Remove-Item -Recurse -Force (Join-Path $PublishDir "widget-host-publish")

Write-Host "Publishing twdev community widget CLI..."
dotnet publish $TwDevProject -c $Configuration -r win-x64 --self-contained true `
    -p:Version=$Version -p:PublishSingleFile=true -o (Join-Path $PublishDir "twdev-publish")
if ($LASTEXITCODE -ne 0) { throw "twdev publish failed." }
Copy-Item -Force (Join-Path $PublishDir "twdev-publish\twdev.exe") (Join-Path $PublishDir "twdev.exe")
Remove-Item -Recurse -Force (Join-Path $PublishDir "twdev-publish")

$AssetsOutput = Join-Path $PublishDir "Assets"
Remove-Item -Recurse -Force $AssetsOutput -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $AssetsOutput | Out-Null
Copy-Item -Path (Join-Path $RepoRoot "assets\*") -Destination $AssetsOutput -Recurse -Force
Copy-Item -Path (Join-Path $RepoRoot "widgets\weather-static\assets\weather") -Destination $AssetsOutput -Recurse -Force
New-Item -ItemType Directory -Force (Join-Path $AssetsOutput "widgets") | Out-Null
Copy-Item -Force (Join-Path $RepoRoot "widgets\media-player\assets\media_cover.png") (Join-Path $AssetsOutput "widgets\media_cover.png")

$ManifestOutput = Join-Path $PublishDir "Widgets"
Remove-Item -Recurse -Force $ManifestOutput -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $ManifestOutput | Out-Null
Get-ChildItem (Join-Path $RepoRoot "widgets") -Filter widget.json -Recurse -File | ForEach-Object {
    $id = (Get-Content $_.FullName -Raw | ConvertFrom-Json).id
    Copy-Item $_.FullName (Join-Path $ManifestOutput "$id.json")
}

Copy-Item -Path (Join-Path $RepoRoot "community-sdk") -Destination (Join-Path $PublishDir "CommunitySDK") -Recurse -Force

$PortableReadme = Join-Path $PublishDir "README-PORTABLE.txt"
Set-Content $PortableReadme "Run TaskbarWidgets.exe. User data is stored in the Data folder beside the executable." -Encoding UTF8

$ExePath = Join-Path $PublishDir "TaskbarWidgets.exe"
if (-not (Test-Path $ExePath)) { throw "Loader output missing: $ExePath" }
Write-Host "Product output: $PublishDir"
Get-ChildItem $PublishDir | Select-Object Name, Length, LastWriteTime
