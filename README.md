# xr-sim

`xr-sim` is a deterministic, headset-free OpenXR runtime for testing VR
applications and mods. It provides simulated head and controller poses,
actions, session-state transitions, frame pacing, fault injection, scripted
input, layer telemetry, and capture support.

The runtime is built for both x86 and x64 Windows applications. It accepts the
standard OpenXR graphics bindings for D3D11, D3D12, OpenGL, and Vulkan, plus the
standard `XR_MND_headless` path. Opt-in private bindings cover D3D9 and D3D10,
for which OpenXR has no Khronos graphics-binding extension.

See [docs/BUILDING.md](docs/BUILDING.md), [docs/USAGE.md](docs/USAGE.md), and
[docs/RENDERERS.md](docs/RENDERERS.md).

## Renderer support

| Application renderer | OpenXR binding | x86 | x64 | Notes |
| --- | --- | :---: | :---: | --- |
| Direct3D 11 | `XR_KHR_D3D11_enable` | Yes | Yes | Full layer compositing and PNG/JSON capture |
| Direct3D 12 | `XR_KHR_D3D12_enable` | Yes | Yes | Native swapchain images and telemetry |
| OpenGL | `XR_KHR_opengl_enable` | Yes | Yes | Win32 OpenGL binding |
| Vulkan | `XR_KHR_vulkan_enable` / `enable2` | Yes | Yes | Legacy and runtime-assisted bootstrap |
| Headless | `XR_MND_headless` | Yes | Yes | No graphics device required |
| Direct3D 10 | `XR_XRSIM_d3d10_enable` | Yes | Yes | Private opt-in compatibility binding |
| Direct3D 9 | `XR_XRSIM_d3d9_enable` | Yes | Yes | Private opt-in compatibility binding |

D3D9 and D3D10 use xr-sim-specific extensions because OpenXR defines no
standard Khronos graphics bindings for those APIs. Applications opt into them
with [the public compatibility header](include/xrsim/xrsim_extensions.h).

## Quick start

```powershell
.\tools\build.ps1 -Architecture all
.\tools\test-backends.ps1 -Architecture all
.\tools\run-with-xrsim.ps1 -Architecture x64 -Executable C:\path\to\app.exe
```

The launcher selects xr-sim only for the child process using
`XR_RUNTIME_JSON`; it does not replace the machine-wide active runtime. Runtime
state, commands, and captures live under `%LOCALAPPDATA%\xr-sim` by default.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the backend boundary,
[docs/CONTROL.md](docs/CONTROL.md) for deterministic input and fault injection,
and [docs/TESTING.md](docs/TESTING.md) for the verified matrix and limitations.

## Status

This is a Windows developer runtime, not a conformant consumer OpenXR runtime.
It is intended for deterministic automated testing without a headset. The
D3D11 backend additionally supplies the rich visual compositor inherited from
the original BioShock VR test runtime; the other renderers currently expose
native swapchain images, lifecycle validation, state telemetry, and layer
metadata without renderer-independent pixel capture.

Licensed under MIT. Bundled OpenXR and Vulkan headers retain their upstream
licenses; see [NOTICE.md](NOTICE.md).
