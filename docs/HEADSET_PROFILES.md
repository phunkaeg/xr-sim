# Boot-time headset profiles

Some OpenXR clients call `xrEnumerateViewConfigurationViews` once, use the
recommended dimensions to allocate their swapchains, and never ask again.
Commands applied later at `xrWaitFrame` are too late to test that bootstrap
path. xr-sim therefore supports an optional headset profile that is loaded
before the first `xrCreateInstance` succeeds and remains immutable for the
process.

Select a profile explicitly:

```powershell
.\tools\run-with-xrsim.ps1 `
    -Executable C:\path\to\application.exe `
    -HeadsetConfig .\headsets\prey-quest3-vdxr.json
```

Alternatively, set `XRSIM_HEADSET_CONFIG` or place `headset.json` in
`XRSIM_DIR`. Resolution order is:

1. `XRSIM_HEADSET_CONFIG`, when set. A missing explicit file is an error.
2. `XRSIM_DIR\headset.json`, when present.
3. xr-sim's built-in Quest-shaped defaults.

The selected source and enumerated dimensions appear in `state.json` as
`headsetConfig` and `recommendedViews`.

## Schema version 1

Angles use radians and follow `XrFovf`: left and down are normally negative;
right and up are normally positive. Each eye is explicit—xr-sim does not
silently mirror the first eye.

```json
{
  "schemaVersion": 1,
  "systemName": "Example HMD",
  "views": [
    {
      "recommendedImageRectWidth": 1832,
      "recommendedImageRectHeight": 1920,
      "maxImageRectWidth": 4096,
      "maxImageRectHeight": 4096,
      "fov": {
        "angleLeft": -0.91,
        "angleRight": 0.67,
        "angleUp": 0.79,
        "angleDown": -0.83
      }
    },
    {
      "recommendedImageRectWidth": 1840,
      "recommendedImageRectHeight": 1936,
      "maxImageRectWidth": 4096,
      "maxImageRectHeight": 4096,
      "fov": {
        "angleLeft": -0.63,
        "angleRight": 0.94,
        "angleUp": 0.81,
        "angleDown": -0.82
      }
    }
  ]
}
```

`maxImageRectWidth` and `maxImageRectHeight` are optional and default to
16384. Unknown fields are ignored so version-1 files can gain annotations.

The runtime rejects the entire profile and returns
`XR_ERROR_INITIALIZATION_FAILED` when JSON is malformed, the schema is not
version 1, required fields are absent, dimensions are zero or exceed their
maximum, a maximum exceeds 16384, an angle is non-finite, or an FOV is
degenerate/inverted. It never partially applies or silently repairs a supplied
profile.

## Boot geometry versus runtime fault injection

The profile controls the system name, enumerated image dimensions, and the FOV
initially returned by `xrLocateViews`. Runtime commands remain frame-atomic:

- `fov profile` restores the selected boot profile's FOV.
- `fov quest3` selects xr-sim's built-in measured Quest 3 FOV.
- `fov eye ...` and the numeric shorthand inject later FOV changes.
- `reset` restores the selected boot profile, not an unrelated hard-coded rig.

Recommended image dimensions are deliberately boot-frozen. Changing them after
a client has allocated swapchains would create a test condition OpenXR clients
are not required to handle.
