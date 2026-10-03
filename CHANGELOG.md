# Changelog

## Unreleased

- Added strict, versioned boot-time headset profiles with per-eye recommended
  and maximum image dimensions, system identity, and asymmetric FOV.
- Added x86/x64 regressions for default, valid, malformed, non-finite,
  degenerate, and invalid-size headset profiles.
- Documented the three-leg stereo acceptance topology: app-owned replay rate,
  upstream submission identity, and downstream image-content comparison.
- Fixed `changedSinceLastSync` and `lastChangeTime` for boolean, float, and
  vector input actions by sampling state at `xrSyncActions` instead of reading
  live controls in `xrGetActionState*`.
- Added menu down/up regression coverage for first-sync, pre-sync isolation,
  repeated reads, steady-state syncs, release timing, and inactive action sets.
- Stabilized D3D10/D3D11/D3D12 adapter selection across equal-memory DXGI
  aliases so D3D9 interop clients receive the first hardware adapter's LUID.
- Added runtime-versus-device LUID assertions and DishonoredVR's 90-frame
  D3D9-to-D3D11 stereo submission probe to the regression suite.
- Fixed control commands that were silently lost: `command.txt`'s write time
  was marked seen before the file was opened, so one failed open, or one read
  of a file caught empty mid-rewrite, dropped the command for good. The time is
  now consumed only after a successful read of a non-empty file, and failed
  opens, read errors, and empty reads are retried on the next poll and logged
  with rate limits. The file is also opened with `FILE_SHARE_DELETE`, so a
  rename-over writer's own DELETE handle no longer blocks the read, and a batch
  is applied only if the file still carries the write time it was noticed
  under, so a rewrite that lands mid-read is not applied twice.
- Added x86/x64 control-channel regressions for an exclusive hold, an in-place
  truncate-then-write, and a rename-over writer still holding DELETE access,
  each with stale-file suppression and exactly-once application checks.

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
