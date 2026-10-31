# Changelog

## SmoothAF v0.5.0 — Experimental

- Removed PANIC, Rearm and the separate Safe Mode concept from the current SmoothAF controls.
- Preserved `Enable=OFF` at load, cooperative `Stop`, single-task ownership, movement limits, timeout and conflict gates.
- Fixed `Diagnostic > Measure` so it is read-only, independent of Enable and computes SAD and Laplacian from the same LiveView sample.
- Centralized measurement publication across Measure, Focus Step and Focus Test, including distinct valid, low-confidence and error outcomes and independent SAD/Laplacian payloads.
- Fixed Debug Logging publication diagnostics and recent-entry navigation; semaphore publication failures are counted instead of being silent.
- Added passive `Vlog Monitor` status, scores, reference/smoothing, session sample count and `manual/N/A` sampling information; Start waits for a new sample, Stop freezes the session, and the monitor does not create a periodic task or move the lens.
- Preserved Canon AF Assist as an unavailable, blocked adapter with no lens movement.
- Docker host validation passed; benchmark output remains `(SIMULATION)`. ARM validation was performed in temporary staging and did not replace the consolidated artifact.
- No physical v0.5 validation, SD-card installation, push, merge or tag was performed.

### Known v0.5 limitations

- The Vlog Monitor observes manual samples and is not continuous or asynchronous.
- SAD/Laplacian scores cannot distinguish blur from scene changes, lighting changes or insufficient texture by themselves.
- `Stop` cannot interrupt a synchronous `lens_focus(wait=1)` call already in flight; unloading during an active task remains unsafe.

## SmoothAF v0.4.0 — Canon AF Assist PoC (blocked, experimental)

- Added a host-testable unavailable adapter and explicit state boundary for future Canon Native AF Assist.
- Added a read-only `Canon AF Assist` menu with `Trigger Canon AF` visibly blocked by `No safe Canon C API`.
- No Canon symbol, Lua callback, `PTP_AF_START`, half-shutter event or lens movement is invoked.
- Host validation covers deterministic rejection; this branch does not claim Canon AF support or physical validation.
- ARM build validation produced `9a600fc9f3acd92c027095819e8bfdeea8a440c2cf16fd8f28989006d94a566c` with 23 dependencies and `Will load on: 600D`; this proves only the blocked adapter build.

## SmoothAF v0.3.0 — Experimental

- Added bounded single-shot `Normal AF` with separate Acquisition and Refinement phases.
- Preserved Legacy HC as the default/fallback and Adaptive HC as an experimental mode.
- Added a fixed 32-record read-only Debug Logging viewer with Previous/Next navigation and task-safe Clear History.
- Extended controller diagnostics with explicit movement, measurement, position and task-result payloads.
- Preserved the physical call `lens_focus(direction, smoothaf_step, 1, 0)` and existing command/reversal limits.
- Extended the deterministic Legacy/Adaptive/Normal benchmark; all benchmark output remains `(SIMULATION)`.
- Host tests, menu regression and ARM build validation remain local checks only. Normal AF has not been physically validated.
- No push, merge, new tag, firmware update or SD-card installation was performed for v0.3.

### Known v0.3 limitations

- Large focus-distance changes may still exceed the bounded search reach.
- Adaptive HC simulated travel depends on Step Size; this does not establish physical superiority.
- Continuous AF and recording-time movement remain disabled.

## SmoothAF v0.2.0 — Experimental

- Preserved Legacy HC as the default bounded focus controller.
- Added experimental Adaptive HC as an explicit selectable mode.
- Added configurable fixed-size debug logging with a 32-entry circular buffer.
- Added deterministic host tests and a Legacy HC versus Adaptive HC benchmark.
- Added automatic local detection of the ignored Legacy Lua Fix tree and private `600D_102.SYM`, while retaining external path overrides.
- Validated the ARM build with 23 dependencies for `600D`.

### Physical test record

The reported physical observations used an EOS 600D / Rebel T3i with firmware 1.0.2, Magic Lantern Legacy Lua Fix, EF 50mm f/1.8 STM, Step Size 3 and Laplacian. They were not a controlled same-scene comparison:

- Legacy HC: `Search Limit`, score 2677, best 2877, mean 74, 1381 ms, 3 commands, 1 reversal.
- Adaptive HC: `Search Limit`, score 3059, best 3087, mean 55, 2374 ms, 4 commands, 3 reversals.

The module loaded, menus worked and micro-adjustments were observed. Large focus-distance changes were not recovered reliably. Debug Logging had no complete on-camera history viewer. Step Sizes 1 and 2 did not work in the recorded EF 50mm f/1.8 STM tests.

This release is experimental. It does not implement continuous AF and does not claim compatibility with other cameras, firmware, lenses or Legacy revisions. Host benchmarks are simulations and do not prove physical focus performance.

## Historical baseline

FastAF 0.1 artifacts and validation records remain unchanged.
