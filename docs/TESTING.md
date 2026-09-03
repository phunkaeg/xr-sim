# Verification

## Direct runtime probe

`xrsim_probe` deliberately does not use the machine's active OpenXR runtime. It
loads the candidate DLL, negotiates the loader/runtime interface, and calls the
returned dispatch table. This isolates runtime defects from manifest, registry,
or headset configuration defects.

Each renderer test verifies:

1. Loader negotiation and an architecture-correct export.
2. Instance creation with the selected graphics extension.
3. System discovery and graphics-requirements query.
4. Session creation with the native binding.
5. Format enumeration and a triple-buffered 64x64 swapchain. D3D10, D3D11,
   D3D12, and Vulkan use a two-slice array to cover stereo-array clients.
6. Native swapchain-image enumeration with the correct structure type.
7. READY transition, session begin, and three complete frames.
8. Acquire, finite wait, release, frame end, and teardown ordering.
9. A command-channel menu up/down sequence across `xrSyncActions`, including
   first-sync behavior, no pre-sync leakage, stable repeated reads, edge
   clearing on a no-change sync, release-edge timing, and the zero-state rule
   for inactive action sets.

The headless test omits swapchain steps by design.

## Current validated matrix

On the development host, both x86 and x64 passed:

- headless
- D3D10 private binding
- D3D11
- D3D12
- OpenGL Win32
- Vulkan

D3D9 compiles in both architectures and its runtime path is included in the
same probe. The development host's non-interactive graphics session did not
provide a D3D9 device, so that row reports a device skip rather than a pass.
Run `tools/test-backends.ps1` in an interactive desktop session on an older
D3D9-capable machine before treating that driver path as release-qualified.

## Manual application check

After the direct matrix passes, run an actual OpenXR application through
`run-with-xrsim.ps1`. Confirm that `state.json` reports:

- `runtime` equal to `xr-sim`
- the expected `graphics` backend
- a running session that reaches `FOCUSED`
- an advancing `frame` counter
- zero `errors`

This second stage tests the Khronos loader, manifest selection, and the
application's renderer integration rather than only the runtime DLL.

## Catalog contracts and client probes

`tools/test-catalog.ps1` maps every supported mod to the architecture and
renderer it presents to OpenXR. Its default run exercises all seven internal
contracts. `-External` additionally discovers and runs standalone probe
executables from the mod repositories without launching any game.

```powershell
.\tools\test-catalog.ps1
.\tools\test-catalog.ps1 -External -CatalogRoot 'D:\Dev Debug'
```

See [CATALOG.md](CATALOG.md) for the exact routes and the distinction between
client-probe and contract-level validation.

The 2026-08-30 development-host run passed all seven internal contracts plus
the available SomaVR, PreyVR, SS2VR, and DishonoredVR external client probes.
FarCry2VR, Sims4VR, and Swat4VR did not expose a standalone client executable
in their current repositories and therefore remain contract-level checks.
