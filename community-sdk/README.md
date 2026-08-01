# Taskbar Widgets Community SDK

Community widgets are folders during development and `.twidget` ZIP packages
when distributed. Taskbar Widgets 0.5.4 supports three manifest generations:

- schema v2: declarative layout and sandbox provider; unchanged and supported.
- schema v3: sandboxed WebView2 renderer; unchanged and supported.
- schema v4: required/optional permission declarations and an optional
  unrestricted Windows process runtime.

## Create and validate

```powershell
# Existing v2 and v3 starters remain available.
twdev init com.example.clock --author "Your Name"
twdev init com.example.island --renderer web --author "Your Name"

# New v4 native XAML + full-access PowerShell runtime starter.
twdev init com.example.native --runtime process --renderer native --author "Your Name"

twdev validate .\com.example.native
twdev pack .\com.example.native
```

Use `schemas/widget-v2.schema.json` and `schemas/layout-v1.schema.json` for
declarative widgets, `schemas/widget-v3.schema.json` for legacy sandboxed web
widgets, and `schemas/widget-v4.schema.json` for the 0.5.4 native
compact/expanded UI and permission/process contract.
`web-widget.d.ts`, `provider.d.ts`, and `process-runtime.md` document the runtime
APIs.

## Native layout DSL

Native layouts use `row`, `column`, `card`, `button`, `spacer`, `divider`,
`text`, `icon`, `image`, `progress`, `bar`, `pie`, and `sparkline` nodes. Values
coming from a provider use `{ "bind": "data.name", "fallback": "..." }`.
Bindings are supported by text, image paths, icon glyphs, foreground colors,
background colors, and gradient stops.

Cards support uniform padding or `{ left, top, right, bottom }`, corner radius,
and `backgroundGradient`. Text supports width/height, font weight, ellipsis and
native marquee animation. Images accept local provider/package paths, clipping
radius and stretch mode. Buttons support bound MDL2 glyphs, native actions,
size, padding, radius and bound foreground/background colors. These features
stay in Explorer's XAML tree and do not start WebView2.

`examples/com.pfc.community-media` is an end-to-end reference: its PowerShell
process reads `Windows.Media.Control`, caches session cover art, derives an
adaptive palette, publishes JSON-line snapshots, and handles play/pause. It
uses no built-in Media provider or private host command.

## Permission review

Settings extracts a selected package into a sealed review directory and reads
its authoritative `widget.json` before anything is installed or executed. The
review shows only that package's required and optional permissions, including
scope, developer reason, risk, scripts/executables, run-as level, and SHA-256.
Remote-library metadata must exactly match the packaged permission declaration.

Installation consumes a short-lived review token for those exact bytes. The
approval records the installed content hash and permission set. Changing a v4
package, its version, or its permissions invalidates approval and prevents its
provider from starting until it is reviewed again. Optional permissions are
passed to sandbox brokers only when the user selects them.

Permission IDs and their user-facing risk meaning are in
`permission-catalog.json`. A process runtime must include
`system.fullAccess` in `permissions.required`; an elevated runtime must also
include `system.administrator`. The review therefore explicitly says when the
package can access all files, accounts, processes, Registry data, network
resources, devices, or other resources available to that Windows identity.

## Execution models

New packages should use `renderer.type: "native"`. Their compact and expanded
JSON layouts are rendered as XAML inside the taskbar without Chromium.
Sandbox providers remain isolated in `TaskbarWidgets.WidgetHost.exe`. They have
short execution/resource limits and can access only the existing HTTPS and
system-metrics brokers declared by their effective permissions. Web UI runs in
`TaskbarWidgets.RenderHost.exe`; direct browser network and Windows access stay
blocked.

A schema v4 `runtime.type: "process"` entry is intentionally unrestricted. It
may be an EXE, PowerShell, CMD/BAT, Python, or Node script and runs with the
installing user's normal Windows rights, or with UAC when `runAs` is
`administrator`. It can use Win32, WinRT, COM, WMI, Steam files, media sessions,
shells, local servers, devices, and third-party SDKs just like any program
started by that user. This is what allows community implementations comparable
to the built-in Media and Steam widgets. The package author must declare the
specific capabilities for an understandable review, while `system.fullAccess`
makes the actual security boundary clear.

Optional permissions are not allowed on a full-access process because the host
cannot technically revoke a subset of Windows rights from an unrestricted
process. Declare every process capability as required; optional grants are for
enforceable sandbox brokers.

The web API's `invoke(action, args)` sends a validated action to its package's
running process. No v2/v3 widget can gain process access through this route.
See `examples/com.pfc.full-access-process`.
For a native media-session implementation, see
`examples/com.pfc.community-media`.

Expandable web widgets use
`taskbarWidget.requestSurface("expanded" | "collapsed")`. For critical surface
controls, add `data-taskbar-widget-surface="expanded"` or
`data-taskbar-widget-surface="collapsed"` to the button. The host handles these
controls on pointer-down, so they keep working even if the widget's own script
fails before attaching click listeners.

## Compatibility and limits

Existing v2/v3 packages, `_permissionsApproved` settings, declarative layouts,
JavaScript providers, and web widgets continue to load with their previous
contracts. v4 approvals use a separate hash-bound store and do not alter legacy
package data.

Package limits are 250 MB and 5000 files. Declarative layouts remain limited to
64 nodes and 8 levels. Sandboxed JavaScript results are 64 KB, HTTPS responses
are 256 KB, script execution is 200 ms, wall time is 5 seconds, and the isolated
provider process is limited to 64 MB. Full-access processes are not placed in
that sandbox and are the package installer's responsibility.

For remote publication and updates, see `remote-library/README.md`.
