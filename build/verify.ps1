param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

function Find-CMake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($command) { return $command.Source }
    foreach ($candidate in @(
        (Join-Path $RepoRoot "artifacts\tools\python-cmake\cmake\data\bin\cmake.exe"),
        (Join-Path $RepoRoot "artifacts\tools\cmake-python\cmake\data\bin\cmake.exe")
    )) {
        if (Test-Path $candidate) { return $candidate }
    }
    foreach ($root in @("C:\Program Files\Microsoft Visual Studio\2022", "C:\Program Files (x86)\Microsoft Visual Studio\2022")) {
        $candidate = Get-ChildItem $root -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($candidate) { return $candidate }
    }
    throw "CMake/MSVC is required for native verification."
}

& (Join-Path $PSScriptRoot "sync-version.ps1") -Check
& (Join-Path $PSScriptRoot "generate-widget-catalog.ps1") -Check

dotnet build (Join-Path $RepoRoot "src\loader\TaskbarWidgets.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Loader build failed." }
dotnet build (Join-Path $RepoRoot "src\widget-host\TaskbarWidgets.WidgetHost.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "WidgetHost build failed." }
dotnet build (Join-Path $RepoRoot "src\twdev\twdev.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "twdev build failed." }
dotnet run --project (Join-Path $RepoRoot "tests\TaskbarWidgets.Tests\TaskbarWidgets.Tests.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Contract tests failed." }

$cmake = Find-CMake
$nativeBuild = Join-Path $RepoRoot "artifacts\native-verify"
& $cmake -S (Join-Path $RepoRoot "src\native") -B $nativeBuild -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "Native configure failed." }
& $cmake --build $nativeBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }
& $cmake --build $nativeBuild --target RUN_TESTS --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

$cargoTarget = if ($env:TASKBARWIDGETS_TAURI_TARGET_DIR) {
    $env:TASKBARWIDGETS_TAURI_TARGET_DIR
} else {
    Join-Path ([IO.Path]::GetTempPath()) "taskbarwidgets-cargo-check"
}
cargo test --manifest-path (Join-Path $RepoRoot "src\settings\src-tauri\Cargo.toml") --target-dir $cargoTarget
if ($LASTEXITCODE -ne 0) { throw "Settings tests failed." }
node --check (Join-Path $RepoRoot "src\settings\dist\app.js")
if ($LASTEXITCODE -ne 0) { throw "Settings JavaScript validation failed." }

$settingsFiles = Get-ChildItem (Join-Path $RepoRoot "src\settings\dist") -File -Recurse |
    Where-Object Extension -in @(".html", ".css", ".js")
if ($settingsFiles | Select-String -Pattern '<(script|link)[^>]+https?://|@import\s+url|fetch\s*\(\s*["'']https?://') {
    throw "Settings UI contains a runtime CDN/network dependency."
}
Write-Host "Taskbar Widgets verification completed."
