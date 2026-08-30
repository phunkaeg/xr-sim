# Usage

## Select the runtime per process

Do not replace the machine-wide active OpenXR runtime. Generate a manifest for
the target application's architecture and set `XR_RUNTIME_JSON` only in the
process that launches the application.

```powershell
$install = .\tools\install-runtime.ps1 -Architecture x64
$env:XR_RUNTIME_JSON = $install.Manifest
$env:XRSIM_DIR = "$env:LOCALAPPDATA\xr-sim\my-test"
& "C:\path\to\application.exe"
```

For a Windows executable, the launcher detects x86 versus x64 from its PE
header and selects the matching runtime automatically:

```powershell
.\tools\run-with-xrsim.ps1 -Executable "C:\path\to\application.exe"
```

The script restores its own environment immediately after starting the child.
Other applications and the system's configured headset runtime are unaffected.

`XR_RUNTIME_JSON` is a secure loader environment variable. Avoid launching
from an elevated shell; security-sensitive environments may intentionally
ignore it.

## Control and state channel

The runtime directory contains:

- `command.txt`: commands written by the driver
- `state.json`: current rig, session, renderer, frame, input, layer, and error state
- `ack.txt`: last applied command sequence
- `xrsim.log` and `xrsim.prev.log`
- `capture/`: D3D11 image and JSON captures

Send commands and wait for acknowledgement:

```powershell
.\tools\xrsim-cmd.ps1 "head rot 30 0 0" "hand r point 0 -5"
.\tools\xrsim-state.ps1 -For "frame+30"
```

Run a scenario:

```powershell
.\tools\xrsim-run.ps1 -Path .\scenarios\smoke.xrs
```

Some inherited BioShock scenarios contain `@mod` lines. Supply the consuming
project's command script through `-ExternalCommandScript`; `@external` is the
generic spelling for new scenarios.

See [CONTROL.md](CONTROL.md) for the command reference.

## Capture

High-fidelity projection and quad-layer capture currently uses the D3D11
compositor:

```powershell
$capture = .\tools\xrsim-shot.ps1 -Out "$env:TEMP\xrsim\test-a"
$capture | Format-List
```

Other renderers continue to publish layer telemetry in `state.json` but reject
the image-capture helper with a clear backend message.
