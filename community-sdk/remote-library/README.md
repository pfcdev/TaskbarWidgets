# PFC remote widget library contract

Settings reads this library from:

`https://pfcsoft.com/twidget_library`

The directory must contain a root `index.json`. Discovery cannot be done safely by listing web-server folders, so every published widget ID must appear in this file.

```text
twidget_library/
├── index.json
└── com.example.clock/
    ├── info.json
    ├── com.example.clock.twidget
    └── preview.png              # optional
```

## 1. Root index

Upload a copy of `index.example.json` as `index.json` and add each published reverse-domain widget ID to `widgets`.

## 2. Widget metadata

Each widget folder contains an `info.json` based on `info.example.json`.

- `id` must exactly match the folder and the packaged `widget.json` ID.
- `package` is a file name in the same folder. Paths and subfolders are rejected.
- `sha256` is the lowercase or uppercase 64-character SHA-256 of the `.twidget` file.
- `author.name` is displayed in Explore and Installed.
- `permissions` is shown in Explore. The installer independently reads the authoritative permissions from the downloaded package.
- `preview` is optional and, when present, must be a PNG file name in the same folder.

Generate the hash on Windows with:

```powershell
(Get-FileHash .\com.example.clock.twidget -Algorithm SHA256).Hash.ToLowerInvariant()
```

The server should return JSON and package files over HTTPS. Directory listing is not required. A missing or invalid `index.json` produces a friendly “library not ready” screen; it does not affect installed widgets.

Downloads are limited to 10 MB. Settings re-fetches `info.json`, downloads only from this fixed PFC origin, verifies SHA-256, validates the `.twidget`, and then opens the permission review screen. Nothing is installed merely by opening Explore.

## Updates and removal

Settings checks the registry shortly after opening, every 30 minutes while it remains open, and whenever **Installed > Check Updates** is pressed. An update appears only when the remote `version` is numerically higher than the installed `MAJOR.MINOR.PATCH` version.

Updates download and verify the package again, then show the new package's author and permissions for explicit approval. Replacement is atomic: if the new folder cannot be installed, the previous version is restored. Publishing a new package therefore requires all three values to change together:

1. Increase `version` in the packaged `widget.json` and remote `info.json`.
2. Upload the new `.twidget` file.
3. Replace `sha256` in `info.json` with the new package hash.

Installed community widgets have a **Remove** action. Removal deletes every instance and its saved settings/rotation entries; it does not affect built-in widgets.
