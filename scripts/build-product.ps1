param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$HookSource = Join-Path $RepoRoot "product\TaskbarStatsHook\taskbar_stats_hook.cpp"
$LoaderProject = Join-Path $RepoRoot "product\TaskbarStats\TaskbarStats.csproj"
$SettingsProject = Join-Path $RepoRoot "product\TaskbarStatsSettings"
$ResourceDir = Join-Path $RepoRoot "product\TaskbarStats\Resources"
$HookOutput = Join-Path $ResourceDir "TaskbarStatsHook.dll"
$SettingsResourceOutput = Join-Path $ResourceDir "TaskbarStatsSettings.exe"
$PublishDir = Join-Path $RepoRoot "artifacts\TaskbarStats"

New-Item -ItemType Directory -Force $ResourceDir | Out-Null
New-Item -ItemType Directory -Force $PublishDir | Out-Null

$Compiler = $env:TASKBARSTATS_CLANGXX
if (-not $Compiler) {
    $clang = Get-Command clang++ -ErrorAction SilentlyContinue
    if ($clang) {
        $Compiler = $clang.Source
    }
}

$CompileArgs = @()
if (-not $Compiler) {
    $WindhawkCompiler = "C:\Program Files\Windhawk\Compiler\bin\clang++.exe"
    if (Test-Path $WindhawkCompiler) {
        $Compiler = $WindhawkCompiler
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
        Write-Warning "Using Windhawk bundled clang++ because no system clang++ was found. Install LLVM/C++ WinRT toolchain or set TASKBARSTATS_CLANGXX for a Windhawk-independent build."
    }
}

if (-not $Compiler) {
    throw "No clang++ compiler found. Install LLVM clang++ or set TASKBARSTATS_CLANGXX."
}

$CompileArgs += @(
    $HookSource,
    "-shared",
    "-o", $HookOutput,
    "-lole32",
    "-loleaut32",
    "-lruntimeobject",
    "-lgdi32",
    "-static-libstdc++",
    "-static-libgcc",
    "-Wl,--export-all-symbols"
)

Write-Host "Building native hook..."
& $Compiler @CompileArgs
if ($LASTEXITCODE -ne 0) {
    throw "Native hook build failed with exit code $LASTEXITCODE."
}

Write-Host "Building settings app..."
cargo build --manifest-path (Join-Path $SettingsProject "Cargo.toml") --release -j 1
if ($LASTEXITCODE -ne 0) {
    throw "Settings app build failed with exit code $LASTEXITCODE."
}

$SettingsBuildOutput = Join-Path $SettingsProject "target\release\TaskbarStatsSettings.exe"
if (-not (Test-Path $SettingsBuildOutput)) {
    throw "Settings app output was not found: $SettingsBuildOutput"
}
Copy-Item -Force $SettingsBuildOutput $SettingsResourceOutput

Write-Host "Publishing loader..."
dotnet publish $LoaderProject `
    -c $Configuration `
    -r win-x64 `
    --self-contained true `
    -p:PublishSingleFile=true `
    -p:IncludeNativeLibrariesForSelfExtract=true `
    -p:EnableCompressionInSingleFile=true `
    -o $PublishDir
if ($LASTEXITCODE -ne 0) {
    throw "Loader publish failed with exit code $LASTEXITCODE."
}

Copy-Item -Force $SettingsBuildOutput (Join-Path $PublishDir "TaskbarStatsSettings.exe")

$ExePath = Join-Path $PublishDir "TaskbarStats.exe"
$HashPath = Join-Path $PublishDir "TaskbarStats.exe.sha256"
if (Test-Path $ExePath) {
    $Hash = (Get-FileHash $ExePath -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -Path $HashPath -Value "$Hash  TaskbarStats.exe" -Encoding ASCII
}

Write-Host "Product output:"
Get-ChildItem $PublishDir | Select-Object Name, Length, LastWriteTime
