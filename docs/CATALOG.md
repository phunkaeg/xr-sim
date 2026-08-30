# VR game catalog compatibility

xr-sim treats a game as an OpenXR client contract, not as a renderer hook. The
game-specific mod still owns camera discovery, engine hooks, stereo rendering,
and any legacy-to-modern texture transport. xr-sim owns the architecture-correct
OpenXR runtime, graphics binding, swapchains, frame lifecycle, input, telemetry,
fault injection, and—in D3D11—visual layer capture.

This boundary is what lets one runtime cover engines from Unreal Engine 2 to
HPL3 and modern CryEngine forks without embedding fragile game-specific code in
the runtime.

## Catalog matrix

| Mod | Engine | Process | Native renderer | OpenXR-facing binding | Integration route | Validation |
|---|---|---:|---|---|---|---|
| SomaVR | HPL3 | x64 | OpenGL | OpenGL | Direct | Client probe verified |
| PreyVR | CryEngine / Arkane fork | x64 | D3D11 | D3D11 | Direct | Client probe verified |
| SS2VR | KEX | x64 | D3D11 | D3D11 | Direct | Client probe verified |
| FarCry2VR | Dunia | x86 | D3D10.1 | D3D11 | Shared surface to a private D3D11 XR device | Contract verified |
| Sims4VR | Proprietary | x64 | D3D11 | D3D11 | Planned direct client | Contract verified |
| DishonoredVR | Unreal Engine 3 | x86 | D3D9 | D3D12 | D3D9On12 bridge | D3D11 client probe + x86/D3D12 contract verified |
| Swat4VR | Unreal Engine 2 / 2.5 | x86 | D3D9 | D3D12 | D3D9On12 bridge | Contract verified |

The machine-readable source for this table is
[`catalog/profiles.json`](../catalog/profiles.json). A catalog profile records
the renderer seen by the runtime. It does not pretend that a D3D9 game can pass
a D3D9 device to an ordinary OpenXR runtime: DishonoredVR and Swat4VR expose
D3D12 through their D3D9On12 adapters, while FarCry2VR exposes a private D3D11
device fed by a shared D3D10.1 surface.

## Validation levels

- **Client probe verified** means an executable from that mod repository has
  loaded xr-sim through the Khronos loader and completed the checks implemented
  by that probe.
- **Contract verified** means xr-sim's matching architecture and graphics
  binding pass the complete instance/session/swapchain/frame probe. The
  game-specific transport remains the responsibility of the mod repository.
- Neither label means the game itself was launched or that gameplay integration
  is complete.

The 2026-08-30 client run covered SOMA's 90-frame OpenGL loop, System Shock 2's
60-frame D3D11 session, Prey's required DXGI adapter LUID, and Dishonored's
32-bit D3D11 bootstrap with a two-slice swapchain and 60 tracked frames.
Dishonored's planned D3D9On12-to-D3D12 route is covered separately by the x86
D3D12 contract. Sims4VR does not yet contain an OpenXR client, so its entry
establishes the target contract rather than claiming game readiness.

## Run the catalog suite

Build both architectures, run every catalog contract, then optionally run only
the standalone probe executables found below the catalog root:

```powershell
.\tools\build.ps1 -Architecture all
.\tools\test-catalog.ps1
.\tools\test-catalog.ps1 -External -CatalogRoot 'D:\Dev Debug'
```

`-External` never launches a game. Missing probe binaries are reported as skips.
Use `-Profile somavr,prey2017` or `-Architecture x86` to narrow a run.

## Adding another game

1. Determine the process architecture and the graphics binding actually passed
   to `xrCreateSession`; do not infer it only from the game's native renderer.
2. Add a profile with one of xr-sim's backend names: `d3d9`, `d3d10`, `d3d11`,
   `d3d12`, `opengl`, `vulkan`, or `headless`.
3. Record any renderer bridge under `transport` and keep that bridge in the mod.
4. Add a standalone client probe path when the mod has one, then run both the
   internal contract and external probe suites.
5. Promote the status only from captured test evidence; a source-code match by
   itself remains contract-level coverage.
