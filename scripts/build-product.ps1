param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release",
    [string]$Version = "0.1.0"
)

$ErrorActionPreference = "Stop"

if ($Version -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') {
    throw "Version must be numeric SemVer-like text such as 0.1.0 or 0.1.0.1."
}

$AssemblyVersion = if ($Version.Split('.').Count -eq 3) { "$Version.0" } else { $Version }

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$HookSource = Join-Path $RepoRoot "product\TaskbarStatsHook\taskbar_stats_hook.cpp"
$MediaHelperSource = Join-Path $RepoRoot "product\TaskbarStatsMediaHelper\media_helper.cpp"
$LoaderProject = Join-Path $RepoRoot "product\TaskbarStats\TaskbarStats.csproj"
$SettingsProject = Join-Path $RepoRoot "product\TaskbarStatsSettingsTauri\src-tauri"
$SettingsTargetDir = Join-Path $env:TEMP "taskbarstats-tauri-target"
$ResourceDir = Join-Path $RepoRoot "product\TaskbarStats\Resources"
$HookOutput = Join-Path $ResourceDir "TaskbarStatsHook.dll"
$MediaHelperOutput = Join-Path $ResourceDir "TaskbarStatsMediaHelper.exe"
$SettingsResourceOutput = Join-Path $ResourceDir "TaskbarStatsSettings.exe"
$PublishDir = Join-Path $RepoRoot "artifacts\TaskbarStats"

New-Item -ItemType Directory -Force $ResourceDir | Out-Null
New-Item -ItemType Directory -Force $PublishDir | Out-Null

Get-Process TaskbarStatsMediaHelper -ErrorAction SilentlyContinue |
    Stop-Process -Force -ErrorAction SilentlyContinue

$CompileArgs = @()
$WindhawkCompiler = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
$Compiler = $env:TASKBARSTATS_CLANGXX
$UseWindhawkArgs = $false
if ($Compiler) {
    $UseWindhawkArgs = (Resolve-Path $Compiler -ErrorAction SilentlyContinue) -eq
        (Resolve-Path $WindhawkCompiler -ErrorAction SilentlyContinue)
} elseif (Test-Path $WindhawkCompiler) {
    $Compiler = $WindhawkCompiler
    $UseWindhawkArgs = $true
} else {
    $clang = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($clang) {
        $Compiler = $clang.Source
    }
}

if ($UseWindhawkArgs) {
    $CompileArgs += @(
        "-x", "c++",
        "-std=c++23",
        "-target", "x86_64-w64-mingw32",
        "-DUNICODE",
        "-D_UNICODE",
        "-DWINVER=0x0A00",
        "-D_WIN32_WINNT=0x0A00",
        "-D_WIN32_IE=0x0A00",
        "-DNTDDI_VERSION=0x0A000008",
        "-D__USE_MINGW_ANSI_STDIO=0",
        "-I", "C:\Program Files\Windhawk\Compiler\include",
        "-I", "C:\Program Files\Windhawk\Compiler\include\winrt"
    )
    Write-Warning "Using Windhawk bundled clang++ for native Explorer hook build."
}

if (-not $Compiler) {
    throw "No clang++ compiler found. Install LLVM clang++ or set TASKBARSTATS_CLANGXX."
}

$NativeLinkArgs = @(
    "-lole32",
    "-loleaut32",
    "-lruntimeobject",
    "-lgdi32",
    "-lgdiplus",
    "-static-libstdc++",
    "-static-libgcc"
)

$HookArgs = @($CompileArgs) + @(
    $HookSource,
    "-shared",
    "-o", $HookOutput,
    $NativeLinkArgs,
    "-Wl,--export-all-symbols"
)

Write-Host "Building native hook..."
& $Compiler @HookArgs
if ($LASTEXITCODE -ne 0) {
    throw "Native hook build failed with exit code $LASTEXITCODE."
}

if (Test-Path $MediaHelperSource) {
    $MediaHelperArgs = @($CompileArgs) + @(
        $MediaHelperSource,
        "-municode",
        "-o", $MediaHelperOutput,
        $NativeLinkArgs
    )

    Write-Host "Building media helper..."
    & $Compiler @MediaHelperArgs
    if ($LASTEXITCODE -ne 0) {
        throw "Media helper build failed with exit code $LASTEXITCODE."
    }
}

Write-Host "Building settings app..."
cargo build --manifest-path (Join-Path $SettingsProject "Cargo.toml") --release -j 1 --target-dir $SettingsTargetDir
if ($LASTEXITCODE -ne 0) {
    throw "Settings app build failed with exit code $LASTEXITCODE."
}

$SettingsBuildOutput = Join-Path $SettingsTargetDir "release\TaskbarStatsSettings.exe"
if (-not (Test-Path $SettingsBuildOutput)) {
    throw "Settings app output was not found: $SettingsBuildOutput"
}
Copy-Item -Force $SettingsBuildOutput $SettingsResourceOutput

Write-Host "Publishing loader..."
dotnet publish $LoaderProject `
    -c $Configuration `
    -r win-x64 `
    --self-contained true `
    -p:Version=$Version `
    -p:AssemblyVersion=$AssemblyVersion `
    -p:FileVersion=$AssemblyVersion `
    -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true `
    -o $PublishDir
if ($LASTEXITCODE -ne 0) {
    throw "Loader publish failed with exit code $LASTEXITCODE."
}

Copy-Item -Force $SettingsBuildOutput (Join-Path $PublishDir "TaskbarStatsSettings.exe")
if (Test-Path $MediaHelperOutput) {
    Get-Process TaskbarStatsMediaHelper -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Copy-Item -Force $MediaHelperOutput (Join-Path $PublishDir "TaskbarStatsMediaHelper.exe")
}

$AssetsSource = Join-Path $RepoRoot "assets"
$AssetsOutput = Join-Path $PublishDir "Assets"
if (Test-Path $AssetsSource) {
    New-Item -ItemType Directory -Force $AssetsOutput | Out-Null
    Copy-Item -Path (Join-Path $AssetsSource "*") -Destination $AssetsOutput -Recurse -Force
}

$ExePath = Join-Path $PublishDir "TaskbarStats.exe"
$HashPath = Join-Path $PublishDir "TaskbarStats.exe.sha256"
if (Test-Path $ExePath) {
    $Hash = (Get-FileHash $ExePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Path $HashPath -Value "$Hash  TaskbarStats.exe" -Encoding ASCII
}

Write-Host "Product output:"
Get-ChildItem $PublishDir | Select-Object Name, Length, LastWriteTime
