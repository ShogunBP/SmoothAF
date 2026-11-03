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
- Lens movement remains blocked in Movie Mode and recording. The sole recording exception is read-only Movie Dry Run for normal H.264; MF, Continuous AF, Follow Focus, Rack Focus and Focus Stacking remain blocked.
- Timeout and cancellation are checked between synchronous lens commands. A `lens_focus()` command already in flight may finish.
- Do not unload, reload or replace the module while a task is active. The Legacy loader does not provide a general task join/kill guarantee.

There is no global arbitration across every Magic Lantern module. SmoothAF maintains one task at a time; manual operations take priority over the next monitor sample.

## Focus controllers

The persisted Vlog selection is `Current Vlog`, `Legacy HC`, `Adaptive HC`, or `Normal AF`; invalid values normalize to `Current Vlog`. Focus Test remains isolated and advertises only the three reusable controllers. Vlog selection is copied into the episode at a safe boundary, and a menu edit during an active episode is shown as pending rather than replacing the live controller.

Current Vlog retains the monitor-native bounded policy. Legacy HC, Adaptive HC, and Normal AF use the reusable core state machines through the cooperative monitor task, with the same per-command safety gate, settle/result cycle, cancellation, cooldown and episode budgets. Confirmation measurements do not issue lens commands. No controller claims Canon-native AF or physical lock from a score alone.

Legacy HC remains the default bounded controller and preserves `lens_focus(direction, smoothaf_step, 1, 0)`. Adaptive HC remains experimental. Normal AF remains bounded single-shot. None of these changes alter the existing movement limits or introduce continuous AF.

## Experimental far-focus recovery

Far Focus Recovery is a separate, camera-independent bounded phase used only by the Current Vlog path when the score is below its experimental threshold. It records coarse acquisition, a two-sided bracket, fine refinement, best-position tracking and an explicit completion result. The adapter owns one lens command at a time and the recovery has a fixed command budget; cancellation and all existing movement gates remain authoritative. Recovery is disabled during Movie unless the existing experimental Movie authorization is explicitly enabled. The state machine and tests are not evidence of global recovery or real STM performance; Canon T3i validation is required.

## Asynchronous passive Vlog Monitor

The v0.6 monitor uses the existing shared SmoothAF task with `SMOOTHAF_TASK_MONITOR`; it does not create a task per sample. Start is explicit and the monitor is disabled during module load. The first sample is immediate, then the next deadline is computed from the previous completion time. Slow samples increment overrun/skipped metrics and never create catch-up work. In H.264 Movie Dry Run, the same task records recording edges and sample validity/timing in RAM; it remains read-only.

The monitor reads the same central 64x64 UYVY/YUV422 ROI through `smoothaf_measure()`. SAD, Laplacian and mean luminance come from the same frame. The monitor exposes a lightweight performance snapshot with task state, configured and actual start-to-start interval, attempt/result counters, duration and overrun data. Menu callbacks only format this snapshot. The monitor never calls `lens_focus()`, half-shutter, Canon AF, exposure controls or VRAM writes. Menu callbacks only change flags/configuration or display snapshots.

States include `IDLE`, `MONITORING`, `STABLE`, `POSSIBLE_BLUR`, `UNCERTAIN`, `PAUSED` and `ERROR`. Blur requires persistent loss with separate entry/recovery thresholds. Low-confidence, invalid, low-texture and lighting-change samples do not become score zero and do not lower the sharpness reference. The first confident sample creates a `PROVISIONAL` reference; it never authorizes blur by itself. Only a genuinely confirmed bounded AF result (`CONVERGED` or photometrically confirmed `BEST_RECOVERED`) promotes it to `VALID`; `NO_IMPROVEMENT`, including a completed AUTO probe, is never focus confirmation. `BEST_RECOVERED` requires both commanded return to the estimated best position and a final score within the best-score deadband. A confirmed lighting change moves the reference to `OBSOLETE`/`RECALIBRATE`; the next scene starts `PROVISIONAL` again. AUTO tracks luminance over all probe samples and rejects direction evidence when the range is environmentally inconsistent. Blur runs require genuinely adjacent confident samples, so `low/middle/low` and low-texture samples break the run. `ACTIVE CONTINUOUS` may run a bounded acquisition series per provisional epoch, with at most three attempts per epoch and an absolute session cap, gated by `Enable`, arming, cooldown/backoff, movie gates and STOP/PANIC. `ACQUIRE_EXHAUSTED` is distinct from `SAFETY_PAUSED`. A post-move lighting change aborts the episode without counting an AF failure. The reference adapts only conservatively while stable. `STABLE` is not a claim of perfect focus.

Sampling intervals are 1000 ms, 500 ms (default), 250 ms and 100 ms experimental. Movie and recording gates pause acquisition without backlog. Stop is cooperative and freezes the session; unload with a still-running task remains unsafe under Legacy.

## Experimental v0.8 Vlog AF local-search POC

The v0.8 proof of concept extends the existing monitor task; it does not create a second task. `Vlog AF Mode` starts `OFF`. A manually armed episode can run in `DRY RUN`, one-shot `ACTIVE`, or persistent `ACTIVE CONTINUOUS`; DRY RUN records a command intent without calling the movement adapter. Physical Movie movement additionally requires `Movie Active Experimental=ON` and is limited to LiveView normal H.264 Movie, while RAW/custom recording remains blocked. All physical modes require `Enable` and recheck `smoothaf_block_reason()` immediately before `smoothaf_move()`.

Each first-test local-search episode is limited to four commands and one reversal, with `SETTLING` after a command and cooldown after improvement. A result is eligible only after the settle deadline; the current implementation uses the same periodic sample for monitor metrics and episode scoring, so physical validation must treat this as experimental. Invalid or low-confidence samples terminate the episode. Stop disarms the episode, but cannot interrupt a synchronous `lens_focus(wait=1)` already in flight. The POC uses `POSSIBLE_BLUR` only as its trigger and does not claim Canon Native Continuous AF or `LOCKED` from a score alone. `AF Lens Calls` proves API invocation, not physical displacement; `AF Episode Result` distinguishes measured improvement, block, failure and cancellation. Relative positions are command-count estimates, not encoder readings. Best-position recovery result codes are reserved until a return path is fully implemented.

## Diagnostics

Debug Logging remains off by default and uses the fixed 32-entry circular history. Manual completed measurements preserve the v0.5 logging contract. Automatic samples update in-memory metrics only; the monitor logs only start/stop and important state transitions, not every sample. Snapshots and formatting occur outside extended hardware work. AF telemetry and history are fixed-size, volatile and RAM-only; episode timelines use bounded event and summary rings, with one terminal `COMPLETE` event per episode. No dynamic allocation or card I/O occurs during initialization, autofocus, monitoring, normal diagnostics or logging callbacks. `Log Page`, `Log Entry`, `Previous`, `Next` and `Clear History` expose the bounded RAM snapshot, with clearing allowed only when no task is active. Filesystem export and FIO probes are temporarily excluded from the normal menu and build; their source remains isolated behind `SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS=1` for future explicit investigation.

`Diagnostic > Measure` remains a single read-only LiveView sample and can run with `Enable=OFF`. The diagnostic viewer remains read-only with Previous/Next navigation and task-safe Clear History. Filesystem actions are intentionally absent from the normal menu; no FIO operation is reachable through initialization, autofocus, monitor, normal diagnostics or logging callbacks. Future FIO investigation is opt-in only through the explicit `SMOOTHAF_ENABLE_FILESYSTEM_DIAGNOSTICS` build flag and is outside the production path.

## Canon AF Assist boundary

Canon AF Assist remains unavailable and blocked. SmoothAF does not call `lens_autofocus()`, Lua internals, `PTP_AF_START`, `lens_setup_af()`, `module_send_keypress()` or an unverified Canon symbol. No v0.6 feature implements automatic autofocus.

## State ownership and unload limits

Only one SmoothAF task may own runtime work. Stop prevents future SmoothAF commands and monitor samples but cannot retroactively cancel a synchronous Legacy operation already sent. The Legacy loader may ignore `deinit()` return values and offers no general join/kill guarantee; unloading while a task is active remains unsafe.
