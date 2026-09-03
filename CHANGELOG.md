# Changelog

## Unreleased

- Fixed `changedSinceLastSync` and `lastChangeTime` for boolean, float, and
  vector input actions by sampling state at `xrSyncActions` instead of reading
  live controls in `xrGetActionState*`.
- Added menu down/up regression coverage for first-sync, pre-sync isolation,
  repeated reads, steady-state syncs, release timing, and inactive action sets.

## 0.1.0 - 2026-08-30

- Extracted the simulator from `bioshock-trilogy-vr` into a standalone runtime.
- Added architecture-neutral x86/x64 handle encoding and build presets.
- Added D3D11, D3D12, OpenGL Win32, Vulkan, Vulkan 2 bootstrap, and headless
  graphics support.
- Added private D3D9 and D3D10 compatibility bindings for older renderers.
- Preserved deterministic poses, actions, pacing, focus policies, hazards,
  control scripts, state telemetry, and the D3D11 layer compositor.
- Added direct runtime probes, renderer-matrix tooling, per-process manifests,
  packaging, CI, and integration documentation.
- Added texture-array swapchains for D3D10, D3D11, D3D12, and Vulkan, including
  per-slice D3D11 compositor capture and layer array-index validation.
- Added machine-readable profiles and automated validation for SomaVR, PreyVR,
  SS2VR, FarCry2VR, Sims4VR, DishonoredVR, and Swat4VR.
