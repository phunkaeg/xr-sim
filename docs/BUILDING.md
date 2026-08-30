# Building

## Requirements

- Windows 10 or newer for the complete renderer matrix
- Visual Studio 2022 with Desktop development with C++
- Windows 10/11 SDK
- CMake 3.24 or newer
- A 32-bit Vulkan loader/driver to execute Vulkan x86 tests; compilation does
  not require a Vulkan SDK

The OpenXR and Vulkan headers required to compile are vendored in
`third_party`; the runtime dynamically loads `vulkan-1.dll` only when a Vulkan
session is created.

## Build both architectures

```powershell
.\tools\build.ps1
```

Equivalent CMake commands:

```powershell
cmake --preset x86
cmake --build --preset x86-release
ctest --test-dir build/x86-vs -C RelWithDebInfo --output-on-failure

cmake --preset x64
cmake --build --preset x64-release
ctest --test-dir build/x64-vs -C RelWithDebInfo --output-on-failure
```

Outputs are under:

- `build/x86-vs/bin/RelWithDebInfo/`
- `build/x64-vs/bin/RelWithDebInfo/`

## Renderer matrix

```powershell
.\tools\test-backends.ps1
```

The probe loads the runtime directly through the same negotiation ABI used by
the OpenXR loader. For each available renderer it creates an instance, queries
graphics requirements, creates a session and swapchain, enumerates native
images, and executes frame acquire/wait/release ordering. Exit 77 is a device
or driver skip; any other nonzero exit is a failure.

## Package

```powershell
.\tools\package.ps1
```

This creates `dist/xr-sim-<version>/` and a zip containing both DLLs, symbols,
public compatibility headers, scripts, scenarios, licenses, and documentation.
