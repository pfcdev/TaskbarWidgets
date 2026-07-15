# Private Windows API risks

Taskbar Widgets integrates with undocumented Windows 11 XAML Diagnostics and
taskbar visual-tree details. Microsoft can rename, remove, or restructure these
surfaces in any cumulative or feature update.

The native boundary therefore follows these rules:

- support Windows 11 x64 only;
- validate expected taskbar types and parents before mutation;
- perform no network, account, provider, or update work inside Explorer;
- catch malformed config, state, layout, and renderer failures;
- clamp all user-controlled size and position inputs;
- leave the taskbar unchanged when compatibility is uncertain;
- remove inserted elements on detach and reinject only after Explorer is ready;
- log diagnostics outside Explorer's normal execution path.

Contributors should test taskbar alignment, DPI, multiple monitors, Explorer
restart, malformed JSON, and missing state on a disposable VM. A successful
compile is not sufficient evidence of compatibility with a new Windows build.
