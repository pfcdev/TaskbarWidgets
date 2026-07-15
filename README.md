<p align="center">
  <img src="assets/branding/logo.png" alt="Taskbar Widgets logo" width="96" height="96" />
</p>

# Taskbar Widgets

Taskbar Widgets is an open-source Windows 11 x64 application that places small,
live widgets directly on the taskbar. The first catalog contains Codex Status,
Weather, Discord Voice, Media Player, and Steam Downloads. Widgets can be shown
side by side or in an optional rotation layout.

> [!WARNING]
> The Explorer integration uses private Windows 11 XAML surfaces. Unsupported
> Windows builds are detected at runtime and the hook fails closed, but this is
> still beta software. See [Private API risks](docs/windows-private-api-risks.md).

## Product model

Users install one product and launch `TaskbarWidgets.exe`. The loader owns data
providers, lifecycle, migration, updates, and account management. Settings is an
internal process opened with `TaskbarWidgets.exe --settings`. A native hook does
only taskbar hosting and rendering; provider failures never run inside Explorer.

User data is stored beside the installed program under `Data`:

```text
%LOCALAPPDATA%\Programs\TaskbarWidgets\
  TaskbarWidgets.exe
  TaskbarWidgets.Settings.exe
  TaskbarWidgets.MediaHelper.exe
  Data\
    config.json
    State\
    Commands\
    Accounts\
    Logs\
```

## Build

Requirements: Windows 11 x64, PowerShell 7 or Windows PowerShell 5.1, .NET 8
SDK, Rust stable, Visual Studio 2022 Build Tools with C++/Windows SDK, CMake, and
NSIS for packaging.

```powershell
.\build.ps1 -Target Verify
.\build.ps1 -Target Build
.\build.ps1 -Target Package -InstallDependencies
```

`VERSION` is the single version source. Package output contains only the full
installer, portable ZIP, SHA-256 files, and a release manifest. Detailed setup
is in [Building](docs/building.md).

## Repository

```text
src/loader/       .NET runtime, providers, accounts, commands, updates
src/settings/     Tauri settings application and offline web UI
src/native/       Explorer hook, taskbar renderer, media helper
widgets/<id>/     manifest, provider source, assets
installer/        NSIS installer
build/            validation, build, package, signing, release scripts
docs/             architecture and contributor documentation
tests/            Explorer-independent contract tests
```

Read [Architecture](docs/architecture.md), [Widget protocol](docs/protocol.md),
and [Adding a widget](docs/adding-a-widget.md) before changing runtime contracts.

## Contributing and security

Contributions are welcome under [CONTRIBUTING.md](CONTRIBUTING.md). Do not post
security vulnerabilities in public issues; follow [SECURITY.md](SECURITY.md).
Taskbar Widgets is licensed under the [MIT License](LICENSE).

Türkçe kurulum özeti için [README.tr.md](README.tr.md) dosyasına bakın.
