# Architecture

`xr-sim` is an OpenXR runtime DLL. The Khronos loader selects it through an
`XR_RUNTIME_JSON` manifest and calls its single exported negotiation function.
The runtime then exposes OpenXR entry points through `xrGetInstanceProcAddr`.

The code is divided by ownership:

| Area | Files | Responsibility |
|---|---|---|
| Runtime ABI and system | `xrsim_instance.cpp` | Loader negotiation, extensions, instance, paths, events, system properties, graphics requirements |
| Renderer boundary | `xrsim_graphics.cpp` | Session graphics binding, per-API formats, swapchain allocation, native image enumeration, device lifetime |
| Session objects | `xrsim_session.cpp` | Session state machine, spaces, swapchain call order, handle tables |
| Frame model | `xrsim_frame.cpp` | Deterministic timing, snapshots, view location, frame lifecycle |
| Input | `xrsim_actions.cpp` | Action sets, bindings, controller profiles, action states, haptics telemetry |
| Automation | `xrsim_control.cpp` | File command channel, atomic rig updates, `state.json`, fault injection |
| D3D11 compositor | `xrsim_compositor.cpp` | Projection/quad layer composition and PNG/JSON capture |
| Common support | `xrsim_common.h`, `xrsim_internal.h`, `xrsim_math.h`, `xrsim_log.cpp` | Handles, state, math, bounded waits, logging |

## Renderer isolation

The simulation core stores poses, timing, actions, spaces, and submitted layer
metadata without graphics-API types. Only `xrsim_graphics.cpp` touches the
application's renderer. A `SimSwapchain` owns typed image slots for each API,
but exactly one set is active for a session.

Session creation selects one backend from the structure chain:

1. The application enables a graphics extension while creating the instance.
2. It queries that extension's graphics requirements.
3. It places the matching graphics binding in `XrSessionCreateInfo::next`.
4. `xr-sim` retains the supplied device/context for the session.
5. Swapchain calls dispatch to the selected backend and return native image
   objects with the standard API-specific image structure.

The headless extension skips steps 2 and 3 and creates no swapchains.

## Architecture-neutral handles

OpenXR handles are 64-bit integers in 32-bit builds and pointer-shaped values
in 64-bit builds. `make_typed_handle` and the `handle_bits` helpers encode the
same type/index/generation payload in either representation. Stale handles are
rejected without dereferencing application-controlled pointers.

## Concurrency and bounded waits

Control-file changes are staged on a polling thread and committed once inside
`xrWaitFrame`, producing a coherent pose/input snapshot for the entire frame.
No wait in the runtime is unbounded: frame pacing, control acknowledgement, and
shutdown paths all have a finite escape route. This prevents an abandoned step
test from hanging the application indefinitely.

OpenGL context access is restricted to the OpenXR functions that own graphics
work. Vulkan swapchain allocation uses device functions loaded from the
application's binding and does not link a particular Vulkan loader at build
time.
