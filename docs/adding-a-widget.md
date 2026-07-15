# Adding a widget

Create `widgets/<widget-id>/widget.json` using `widgets/widget.schema.json`.
IDs are stable lowercase kebab-case identifiers. Define metadata, default size,
local assets, and typed settings; do not encode visual layout in the manifest.

Data collection implements `IWidgetProvider` in C# and returns data through the
common `WidgetSnapshot` envelope. It must honor cancellation, avoid blocking
other providers, and expose a controlled retry/backoff path. Never put network,
credential, or database work in Explorer.

Rendering implements the native `IWidgetRenderer` contract. Treat snapshot data
as untrusted: validate types, clamp dimensions, catch errors at the renderer
boundary, and fail closed. Actions must use an existing allowlisted command or
add a narrowly scoped command validation change and tests.

Then run:

```powershell
.\build\generate-widget-catalog.ps1
.\build.ps1 -Target Verify
```

Commit the manifest, source/assets, and generated C#, C++, and JavaScript
catalogs together. External binary widget packages are intentionally unsupported
in v1.
