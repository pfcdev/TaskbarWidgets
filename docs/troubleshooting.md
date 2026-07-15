# Troubleshooting

## Widgets do not appear

Confirm Windows 11 x64, restart Explorer once, and inspect
`Data\Logs\loader.log` and `Data\Logs\hook.log`. An unsupported taskbar XAML
layout disables injection rather than forcing it.

## One widget is unavailable

Inspect `Data\State\<widget-id>.json` and its `status`/`error` fields. Provider
backoff is isolated, so another widget continuing to work is expected. Delete
neither account nor configuration data while diagnosing.

## Settings does not open

Run `TaskbarWidgets.exe --settings`. Verify that
`TaskbarWidgets.Settings.exe` exists beside the loader. Settings is fully local
and should open without internet access.

## Build says CMake/MSVC is missing

Install the Visual Studio 2022 Build Tools workload "Desktop development with
C++" including the Windows SDK and CMake tools, then open a new terminal. The
project does not require Windhawk.

## SmartScreen warns during installation

Unsigned beta builds can trigger SmartScreen. Compare the downloaded file with
the published SHA-256 value. Official CI signs artifacts only when project
signing secrets are configured.
