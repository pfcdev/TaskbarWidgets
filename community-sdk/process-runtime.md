# Schema v4 process runtime

The process runtime is an opt-in, full-trust provider for packages whose needs
cannot be met by the sandbox.

```json
{
  "runtime": {
    "type": "process",
    "entry": "provider.ps1",
    "arguments": [],
    "protocol": "json-lines-v1",
    "lifetime": "persistent",
    "runAs": "user",
    "workingDirectory": "package"
  }
}
```

`entry` must remain inside the package. Supported launch forms are `.exe`,
`.ps1`, `.cmd`, `.bat`, `.py`, and `.js`. Other extensions are launched
directly. `workingDirectory` may be `package` or the Taskbar Widgets data
directory.

For `runAs: "user"`, `json-lines-v1` uses one compact JSON object per UTF-8
stdin/stdout line. The host sends:

- `initialize`: widget identity, paths, and every enabled instance.
- `instancesChanged`: the current instances and their settings.
- `action`: an action and arguments sent by `taskbarWidget.invoke()`.
- `shutdown`: request to exit cleanly.

The provider may emit:

```json
{"type":"snapshot","instanceId":"com.example.widget","data":{"value":42}}
{"type":"log","message":"Provider started"}
```

Snapshot data is delivered to the matching web, declarative, or native instance. An input
or output line may not exceed 1 MB. Invalid lines are logged and ignored.

`examples/com.pfc.community-media` demonstrates a persistent PowerShell
runtime that consumes `initialize`, `instancesChanged`, `action`, and
`shutdown`, reads Windows media sessions through WinRT, emits per-instance
snapshots, and responds to a native layout button without any built-in widget
API.

`runAs: "administrator"` uses Windows UAC and currently requires
`protocol: "none"` because standard-stream IPC is not available across that
launch mode. Such a process is suitable for a standalone service/UI rather than
interactive widget actions.

Every process runtime requires `system.fullAccess` as a required permission.
Administrator launch additionally requires `system.administrator`. The
Settings review displays the entry file, every executable/script in the
package, requested scope/reasons, hash, and run-as level before installation.
Process manifests cannot contain optional permissions: after full access is
granted, a subset cannot be reliably revoked. Optional permissions are reserved
for sandbox brokers that can enforce them.
