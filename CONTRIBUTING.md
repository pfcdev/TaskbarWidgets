# Contributing

Thank you for contributing to Taskbar Widgets.

1. Open an issue for behavior changes or new widget concepts before a large
   implementation.
2. Create a focused branch and keep unrelated formatting or generated files out
   of the change.
3. Run `.\build.ps1 -Target Verify` on Windows 11 x64.
4. If a manifest changed, run `.\build\generate-widget-catalog.ps1` and commit
   all three generated catalogs.
5. Explain Explorer/private-API risk, migration impact, and manual smoke tests in
   the pull request.

New widgets must be source contributions under `widgets/<widget-id>` and follow
[Adding a widget](docs/adding-a-widget.md). Binary plugin submissions are not
accepted in manifest v1. Do not add remote fonts, CDN scripts, unlicensed assets,
secrets, account data, logs, or build output.

Code running inside Explorer receives extra scrutiny: catch all boundary errors,
validate sizes and JSON, keep network/account work in the loader, and leave the
taskbar unchanged when compatibility is uncertain.
