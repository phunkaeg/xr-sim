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
5. Format enumeration and a triple-buffered 64x64 swapchain.
6. Native swapchain-image enumeration with the correct structure type.
7. READY transition, session begin, and three complete frames.
8. Acquire, finite wait, release, frame end, and teardown ordering.

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
