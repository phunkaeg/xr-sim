# Renderer support

The standard bindings follow the OpenXR graphics extensions listed in the
[Khronos OpenXR registry](https://registry.khronos.org/OpenXR/). D3D9 and D3D10
have no Khronos OpenXR graphics-binding extension, so `xr-sim` provides
explicitly private compatibility extensions for applications and VR mods that
can opt into them.

| Renderer | Extension | x86 | x64 | Swapchains | Capture |
|---|---|---:|---:|---:|---:|
| Headless | `XR_MND_headless` | Yes | Yes | N/A | Telemetry only |
| Direct3D 9 | `XR_XRSIM_d3d9_enable` | Yes | Yes | Yes | Telemetry only |
| Direct3D 10 | `XR_XRSIM_d3d10_enable` | Yes | Yes | Yes | Telemetry only |
| Direct3D 11 | `XR_KHR_D3D11_enable` | Yes | Yes | Yes | Full layer PNG/JSON |
| Direct3D 12 | `XR_KHR_D3D12_enable` | Yes | Yes | Yes | Telemetry only |
| OpenGL Win32 | `XR_KHR_opengl_enable` | Yes | Yes | Yes | Telemetry only |
| Vulkan | `XR_KHR_vulkan_enable` | Yes | Yes | Yes | Telemetry only |
| Vulkan 2 bootstrap | `XR_KHR_vulkan_enable2` | Yes | Yes | Yes | Telemetry only |

"Telemetry only" means poses, actions, frame timing, layer counts/types,
swapchain state, hazards, and scripted control all work, but `xrsim-shot.ps1`
does not produce images. The original high-fidelity compositor is retained for
D3D11. Extending image capture does not require changing the simulation core;
it requires a backend readback/composition implementation.

## Direct3D 9 and 10

Include `include/xrsim/xrsim_extensions.h`, enable the private extension, call
its requirements function, and pass its binding structure to `xrCreateSession`.
Swapchain enumeration returns the matching private image structure.

```cpp
const char* extensions[] = {XR_XRSIM_D3D9_ENABLE_EXTENSION_NAME};

XrGraphicsRequirementsD3D9XRSIM requirements{
    XR_TYPE_GRAPHICS_REQUIREMENTS_D3D9_XRSIM};
getD3D9Requirements(instance, systemId, &requirements);

XrGraphicsBindingD3D9XRSIM binding{XR_TYPE_GRAPHICS_BINDING_D3D9_XRSIM};
binding.device = d3d9Device;

XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &binding;
sessionInfo.systemId = systemId;
```

D3D9 swapchain images are `IDirect3DTexture9*`. A shared handle is returned
when the supplied device supports it; classic non-Ex devices receive a
same-process texture with a null shared handle. D3D10 returns
`ID3D10Texture2D*`.

These extensions are for controlled testing. A stock OpenXR application does
not know their names or structures and therefore cannot use them without an
integration change. A D3D9/D3D10 game can alternatively use a renderer bridge
such as a D3D11 or Vulkan translation layer and use the corresponding standard
OpenXR binding.

## Other APIs and platforms

- D3D8 has neither an OpenXR binding nor a modern resource-sharing contract.
  Use a D3D8-to-D3D9/D3D11 bridge before integrating OpenXR.
- OpenGL ES is an Android binding. This repository currently targets Win32.
- Metal is a macOS binding. This repository currently targets Win32.
- EGL on Windows can be added as another backend; it is not advertised today.

The runtime advertises only extensions whose entry points and swapchain paths
it implements. It does not claim cross-platform bindings that cannot operate in
the current Windows process.
