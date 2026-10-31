# SmoothAF Architecture

## Scope

SmoothAF is an experimental single-shot contrast autofocus module integrated into a compatible Magic Lantern Legacy Lua Fix tree. The repository is not a complete Magic Lantern tree. The Legacy tree supplies module headers, build conventions, camera-specific symbols, menu APIs, LiveView VRAM access, task APIs, semaphores and `lens_focus()`.

See [Development](DEVELOPMENT.md) for integration and build inputs, [Research](RESEARCH.md) for unvalidated investigations, and [Testing](TESTING.md) for evidence and limits.

## Runtime safety model

- `Enable` starts at `0`; loading creates no task and emits no lens command.
- Activation is manual and each search is bounded.
- Read-only Measure and the asynchronous Vlog Monitor never require movement enablement.
- Stop requests cooperative cancellation; a synchronous lens command already in flight may finish.
- A task is reserved before `task_create()`; task-creation failure releases the reservation.
- `smoothaf_sem` protects control state and diagnostic snapshots. VRAM reads and sleeps occur outside the semaphore.
- Movie Mode, recording, MF, Continuous AF, Follow Focus, Rack Focus and Focus Stacking remain blocked.
- Timeout and cancellation are checked between synchronous lens commands. A `lens_focus()` command already in flight may finish.
- Do not unload, reload or replace the module while a task is active. The Legacy loader does not provide a general task join/kill guarantee.

There is no global arbitration across every Magic Lantern module. SmoothAF maintains one task at a time; manual operations take priority over the next monitor sample.

## Focus controllers

Legacy HC remains the default bounded controller and preserves `lens_focus(direction, smoothaf_step, 1, 0)`. Adaptive HC remains experimental. Normal AF remains bounded single-shot. None of these changes alter the existing movement limits or introduce continuous AF.

## Asynchronous passive Vlog Monitor

The v0.6 monitor uses the existing shared SmoothAF task with `SMOOTHAF_TASK_MONITOR`; it does not create a task per sample. Start is explicit and the monitor is disabled during module load. The first sample is immediate, then the next deadline is computed from the previous completion time. Slow samples increment overrun/skipped metrics and never create catch-up work.

The monitor reads the same central 64x64 UYVY/YUV422 ROI through `smoothaf_measure()`. SAD, Laplacian and mean luminance come from the same frame. The monitor never calls `lens_focus()`, half-shutter, Canon AF, exposure controls or VRAM writes. Menu callbacks only change flags/configuration or display snapshots.

States include `IDLE`, `MONITORING`, `STABLE`, `POSSIBLE_BLUR`, `UNCERTAIN`, `PAUSED` and `ERROR`. Blur requires persistent loss with separate entry/recovery thresholds. Low-confidence, invalid, low-texture and lighting-change samples do not become score zero and do not lower the sharpness reference. The reference adapts only conservatively while stable. `STABLE` is not a claim of perfect focus.

Sampling intervals are 1000 ms, 500 ms (default), 250 ms and 100 ms experimental. Movie and recording gates pause acquisition without backlog. Stop is cooperative and freezes the session; unload with a still-running task remains unsafe under Legacy.

## Diagnostics

Debug Logging remains off by default and uses the fixed 32-entry circular history. Manual completed measurements preserve the v0.5 logging contract. Automatic samples update in-memory metrics only; the monitor logs only start/stop and important state transitions, not every sample. Snapshots and formatting occur outside extended hardware work, and no continuous card writes are performed.

`Diagnostic > Measure` remains a single read-only LiveView sample and can run with `Enable=OFF`. The diagnostic viewer remains read-only with Previous/Next navigation and Clear History only while no SmoothAF task is active.

## Canon AF Assist boundary

Canon AF Assist remains unavailable and blocked. SmoothAF does not call `lens_autofocus()`, Lua internals, `PTP_AF_START`, `lens_setup_af()`, `module_send_keypress()` or an unverified Canon symbol. No v0.6 feature implements automatic autofocus.

## State ownership and unload limits

Only one SmoothAF task may own runtime work. Stop prevents future SmoothAF commands and monitor samples but cannot retroactively cancel a synchronous Legacy operation already sent. The Legacy loader may ignore `deinit()` return values and offers no general join/kill guarantee; unloading while a task is active remains unsafe.
