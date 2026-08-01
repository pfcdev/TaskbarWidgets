# Focus Island

The reference schema v3 web widget for Taskbar Widgets 0.5.0. It stays idle at
one update per second, expands on hover, and persists the focus timer through
the quota-limited `window.taskbarWidget.storage` API.

```powershell
twdev validate .
twdev pack .
```

The package does not request network, file, shell, media, WebGL, or continuous
animation permissions. The existing native Media Player widget is unaffected.
