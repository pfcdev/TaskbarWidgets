# Architecture

Taskbar Widgets is one user-facing product made from three isolated processes.

```text
Settings UI -> Data/config.json -> Loader/providers -> Data/State/*.json
     ^                                      |                 |
     |                                      v                 v
Data/Commands/*.json <---------------- actions ---------- Explorer hook
```

## Components

- `src/loader` is the .NET host. It migrates legacy data once, supervises each
  provider independently, writes atomic snapshots, validates commands, manages
  Codex accounts/IDE profiles, updates the product, and reinjects after an
  Explorer restart.
- `src/settings` is a Tauri application with local HTML, CSS, JavaScript, icons,
  and fonts. It edits config v2 and preserves unknown widget records disabled.
- `src/native/taskbar-hook` is the Explorer-side boundary. It discovers the
  Windows 11 taskbar XAML tree, lays out enabled widgets, renders snapshots, and
  queues user actions. Every parse/render/injection boundary catches errors and
  fails closed.
- `src/native/media-helper` talks to Windows media sessions outside the hook.
- `widgets/<id>` owns a v1 manifest, optional C# provider, renderer assets, and
  widget-specific documentation. Source widgets are compiled with the product;
  v1 does not load arbitrary external DLLs or executable folders.

## Generated catalog

`build/generate-widget-catalog.ps1` validates all manifests, rejects duplicate
or invalid IDs, then generates catalogs for C#, C++, and Settings. Generated
files are committed so a diff shows catalog changes. `-Check` makes CI fail if
they are stale.

## Failure isolation

Each provider has its own retry/backoff loop and writes only its widget state.
State is written as a temporary file then atomically replaced. The hook treats
missing, malformed, oversized, or unsupported snapshots as unavailable and
does not execute provider code. Unknown commands and unsupported schema
versions are rejected by the loader.

## Installation and data

The default install root is `%LOCALAPPDATA%\Programs\TaskbarWidgets`. Packaged
assets are immutable product files; mutable files live under `Data`. Portable
packages use the same relative layout. Uninstall preserves `Data` unless the
user explicitly selects data removal.

The migration layer is the only production code allowed to refer to the legacy
product name. It copies user-owned settings, accounts, profiles, active account,
and libraries without deleting the source. Runtime binaries, caches, logs,
updates, and stale state are intentionally excluded.

## Private Windows boundary

The hook depends on undocumented XAML element names and layout behavior that
can change between Windows updates. Compatibility checks happen before layout
mutation, all injected UI is removable, and a named shutdown event permits a
clean detach. See [windows-private-api-risks.md](windows-private-api-risks.md).
