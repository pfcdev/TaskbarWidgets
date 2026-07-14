# TaskbarStats

Experimental Windows 11 taskbar integration prototype.

This repository intentionally targets the real Windows 11 taskbar surface from a
Windhawk mod loaded into `explorer.exe`. It does not implement a tray icon,
floating overlay, always-on-top window, desktop widget, Widgets Board widget, or
normal app taskbar button.

## Milestones

1. Real taskbar module: insert a small XAML module inside the Windows 11
   taskbar.
2. Codex status agent: collect Codex account rate-limit and local 30-day usage
   outside Explorer.
3. Taskbar slider: rotate compact Codex status views in the taskbar module.
4. Packaging: replace the Windhawk development harness with the product loader.
5. Hardening: multi-monitor, DPI, build compatibility, diagnostics.

## Current State

Milestones 1-3 are implemented as a prototype:

- `windhawk/taskbar-stats.wh.cpp` injects the taskbar XAML module.
- `agent/CodexStatusAgent` collects Codex status and writes a local JSON file.

The mod uses Windows XAML Diagnostics from inside `explorer.exe` to inspect
taskbar XAML elements and insert a small module next to the system tray frame.
The code is guarded so that unsupported/private taskbar structures log and fail
closed instead of crashing Explorer intentionally.

The agent runs outside Explorer. It starts Codex app-server on a temporary
loopback WebSocket port, requests `account/rateLimits/read`, reads local 30-day
token totals from `%USERPROFILE%\.codex\state_5.sqlite` when `sqlite3.exe` is
available, and writes:

```text
%LOCALAPPDATA%\TaskbarStats\codex-status.json
```

## Manual Test Summary

Build/run the status agent once:

```powershell
dotnet run --project .\agent\CodexStatusAgent\CodexStatusAgent.csproj -- --once
```

Then install Windhawk, create a new local mod, paste the contents of
`windhawk/taskbar-stats.wh.cpp`, compile/enable it, and verify that the taskbar
rotates through compact Codex status slides such as `LIMIT`, `WEEK`, `30D`, and
`PLAN`.

Copy SVG assets to:

```text
%LOCALAPPDATA%\TaskbarStats\Assets\
```

before testing if you want the SVG icons to load. If they are missing, the mod
falls back to Segoe MDL2 glyphs.

## Product Loader Build

The Windhawk-free product build creates a single-file loader that embeds the
native Explorer hook DLL and extracts it at runtime:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-product.ps1 -Configuration Release
```

Output:

```text
artifacts\TaskbarStats\TaskbarStats.exe
```

Useful commands:

```powershell
.\artifacts\TaskbarStats\TaskbarStats.exe --console
.\artifacts\TaskbarStats\TaskbarStats.exe --detach
.\artifacts\TaskbarStats\TaskbarStats.exe --check-updates
.\artifacts\TaskbarStats\TaskbarStats.exe --update
.\artifacts\TaskbarStats\TaskbarStats.exe --install-startup
.\artifacts\TaskbarStats\TaskbarStats.exe --uninstall-startup
```

Logs are written under:

```text
%LOCALAPPDATA%\TaskbarStats\Logs\
```

## GitHub Releases Updates

The product loader checks GitHub Releases for updates from:

```text
pfcdev/TaskWidgets
```

The latest release must include these assets:

```text
TaskbarStats.exe
TaskbarStats.exe.sha256
TaskbarStatsSetup.exe
TaskbarStatsSetup.exe.sha256
```

`TaskbarStatsSetup.exe` is the primary update package. The loader prefers it
over raw executable assets so updates use the same NSIS install/uninstall flow as
normal distribution.

Create or update a release from a shell where GitHub CLI is authenticated:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\create-github-release.ps1 -Tag v0.1.0
```

GitHub Actions can also build and publish the release on GitHub-hosted Windows
runners:

```powershell
gh workflow run release.yml --repo pfcdev/TaskWidgets -f version=0.1.0
```

Pushing a numeric version tag such as `v0.1.0` runs the same release workflow.

Normal startup checks for updates in the background. Use `--no-update-check`
to disable the startup check for development runs.
