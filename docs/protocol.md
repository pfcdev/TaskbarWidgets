# Runtime contracts

All local transport is versioned JSON below the installation's `Data` folder.
Writers use a temporary file and atomic rename; consumers never observe a
partially written document.

## WidgetSnapshot v1

Providers write `Data/State/<widget-id>.json`:

```json
{
  "schemaVersion": 1,
  "widgetId": "weather-static",
  "sequence": 42,
  "updatedAtUnix": 1783785846,
  "status": "ok",
  "data": { "city": "Istanbul", "temperature": 24 },
  "error": null
}
```

`status` is `ok`, `loading`, `unavailable`, or `error`. `sequence` increases per
provider. The hook rejects a mismatched `widgetId`, unsupported schema, malformed
JSON, and invalid data, logging the condition without propagating it to Explorer.

## WidgetCommand v1

The hook writes one file per command to `Data/Commands`:

```json
{
  "schemaVersion": 1,
  "commandId": "7f71b4d3-5166-4423-85af-f945ab083d22",
  "widgetId": "media-player",
  "action": "mediaToggle",
  "createdAtUnix": 1783785846,
  "arguments": {}
}
```

The loader requires a unique command ID, a known schema, a recent timestamp,
and an allowlisted action. Unsupported, stale, or malformed commands are moved
out of the active queue or rejected; they are never dynamically dispatched.

## Configuration v2

Settings writes `Data/config.json`:

```json
{
  "configVersion": 2,
  "layout": { "mode": "row" },
  "widgets": [
    {
      "id": "codex-status",
      "enabled": true,
      "order": 0,
      "position": { "anchorPercent": 100, "offsetPx": 0 },
      "settings": {}
    }
  ],
  "rotation": {
    "intervalSeconds": 30,
    "widgetIds": ["codex-status", "weather-static"]
  }
}
```

`layout.mode` is `row` or `rotation`. Unknown widget records are preserved but
forced disabled so a configuration survives temporary catalog differences.
Legacy v1 data is migrated once; the removed Crypto Fees placeholder falls back
to the first valid widget or Codex Status.
