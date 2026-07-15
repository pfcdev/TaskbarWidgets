# Building

## Requirements

- Windows 11 x64
- PowerShell 5.1 or newer
- .NET 8 SDK
- Rust stable with Cargo
- Visual Studio 2022 Build Tools: Desktop development with C++, MSVC x64,
  Windows 11 SDK, and CMake tools
- NSIS 3 for installer packages
- Optional: GitHub CLI for releases and Windows SDK SignTool for signing

No Windhawk installation is required.

## Entry point

`build.ps1` reads the root `VERSION` and exposes four targets:

```powershell
.\build.ps1 -Target Verify   # manifests, .NET, Rust, CMake, native tests, offline UI
.\build.ps1 -Target Build    # full product directory
.\build.ps1 -Target Package  # installer, portable ZIP, checksums, manifest
.\build.ps1 -Target Release  # package and GitHub release
```

Use `-Configuration Debug` when needed. `Package -InstallDependencies` permits
the packaging script to install NSIS if it is missing. Release builds should run
from a clean checkout.

## Outputs

`artifacts/TaskbarWidgets` is the assembled application. Publishable artifacts
are `TaskbarWidgetsSetup-x64.exe`, `TaskbarWidgets-portable-x64.zip`, their
`.sha256` files, and `release-manifest.json`. A loader-only executable is never
published.

## Signing

CI signs all EXE/DLL files and the installer when
`WINDOWS_SIGNING_CERT_BASE64` and `WINDOWS_SIGNING_CERT_PASSWORD` secrets exist.
Without them, packaging remains reproducible but unsigned; release notes must
state that SmartScreen can warn users.
