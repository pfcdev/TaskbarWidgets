param(
    [ValidateSet("Release", "Debug")]
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")

& (Join-Path $PSScriptRoot "sync-version.ps1") -Check
& (Join-Path $PSScriptRoot "generate-widget-catalog.ps1") -Check

dotnet build (Join-Path $RepoRoot "src\loader\TaskbarWidgets.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Loader build failed." }
dotnet run --project (Join-Path $RepoRoot "tests\TaskbarWidgets.Tests\TaskbarWidgets.Tests.csproj") -c $Configuration
if ($LASTEXITCODE -ne 0) { throw "Contract tests failed." }

$cargoTarget = Join-Path ([IO.Path]::GetTempPath()) "taskbarwidgets-cargo-check"
cargo check --manifest-path (Join-Path $RepoRoot "src\settings\src-tauri\Cargo.toml") --target-dir $cargoTarget
if ($LASTEXITCODE -ne 0) { throw "Settings check failed." }

$cmake = Get-Command cmake -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source
if (-not $cmake) { throw "CMake/MSVC is required for native verification." }
$nativeBuild = Join-Path $RepoRoot "artifacts\native-verify"
& $cmake -S (Join-Path $RepoRoot "src\native") -B $nativeBuild -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { throw "Native configure failed." }
& $cmake --build $nativeBuild --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { throw "Native build failed." }
& $cmake --build $nativeBuild --target RUN_TESTS --config $Configuration
if ($LASTEXITCODE -ne 0) { throw "Native tests failed." }

$settingsFiles = Get-ChildItem (Join-Path $RepoRoot "src\settings\dist") -File -Recurse |
    Where-Object Extension -in @(".html", ".css", ".js")
if ($settingsFiles | Select-String -Pattern '<(script|link)[^>]+https?://|@import\s+url|fetch\s*\(\s*["'']https?://') {
    throw "Settings UI contains a runtime CDN/network dependency."
}
Write-Host "Taskbar Widgets verification completed."
