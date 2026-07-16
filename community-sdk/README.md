# Taskbar Widgets Community SDK

Community widgets are ordinary folders while developing and `.twidget` ZIP packages when sharing. Install a folder or package from **Settings > Developer**, drop it on that page, or copy it to:

`%LocalAppData%\TaskbarWidgets\CommunityWidgets\<reverse.domain.id>`

The loader validates every change and writes a normalized runtime catalog. Explorer only reads that normalized catalog and snapshot data; it never loads package code or native DLLs.

## Quick start

```powershell
twdev init com.example.clock --author "Your Name" --website https://example.com
twdev validate .\com.example.clock
twdev dev .\com.example.clock
twdev pack .\com.example.clock
```

Use `schemas/widget-v2.schema.json` and `schemas/layout-v1.schema.json` for editor IntelliSense. `provider.d.ts` documents the deliberately small JavaScript API.

Limits: 10 MB, 100 files, 64 layout nodes, 8 levels, 64 KB provider result, 256 KB HTTP response, 200 ms script execution, 5 seconds wall time and 64 MB provider-process memory. Only manifest-approved HTTPS hosts are available. Shells, Node APIs, arbitrary files, registry, process launch, camera and microphone are unavailable.

The Settings **Explore** page reads the PFC registry at `https://pfcsoft.com/twidget_library`. See `remote-library/README.md` for the exact server folder and JSON contracts. Locally installed packages are always labeled **Local / Unverified**; remote packages are still revalidated and require an explicit permission approval before installation.

For updates, increment the package manifest and remote `info.json` version together, rebuild the `.twidget`, and publish its new SHA-256. Settings performs periodic version checks and never replaces an installed widget without showing the new permissions again.
