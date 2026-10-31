# SmoothAF Architecture

## Scope

SmoothAF is an experimental single-shot contrast autofocus module integrated into a compatible Magic Lantern Legacy Lua Fix tree. The repository is not a complete Magic Lantern tree. The Legacy tree supplies module headers, build conventions, camera-specific symbols, menu APIs, LiveView VRAM access, task APIs, semaphores and `lens_focus()`.

See [Development](DEVELOPMENT.md) for integration and build inputs, [Research](RESEARCH.md) for unvalidated investigations, and [Testing](TESTING.md) for evidence and limits.

## Runtime safety model

- `Enable` starts at `0`; loading creates no task and emits no lens command.
- Activation is manual and each search is bounded.
- `Enable` starts at `0` and is explicit movement armament; read-only Measure does not require it.
- `Stop` requests cooperative cancellation; a synchronous lens command already in flight may finish.
- A task is reserved before `task_create()`; task-creation failure releases the reservation.
- `smoothaf_sem` protects state transitions and diagnostic snapshots. The viewer does not create tasks or move the lens.
- Movie Mode, recording, MF, Continuous AF, Follow Focus, Rack Focus and Focus Stacking remain blocked.
- Timeout and cancellation are checked between synchronous lens commands. A `lens_focus()` command already in flight may finish.
- Do not unload, reload or replace the module while a task is active. The Legacy loader does not provide a general task join/kill guarantee.

There is no global arbitration across every Magic Lantern module. SmoothAF applies the public conflict gates available to it and maintains one SmoothAF task at a time.

## Focus controllers

### Legacy HC

Legacy HC is the default and fallback controller. It uses bounded hill-climbing with the existing physical call:

```c
lens_focus(direction, smoothaf_step, 1, 0)
```

The controller uses a central 64×64 UYVY/YUV422 region and integer-only SAD or Laplacian scores. UYVY luminance is read at `row[x * 2 + 1]`; the runtime VRAM pitch is respected.

### Adaptive HC

Adaptive HC is selectable experimental behavior. It retains explicit command and reversal limits while adapting its policy to the configured step size. Simulated travel is not a physical performance claim.

### Normal AF

Normal AF is a bounded, single-shot controller with two phases:

- Acquisition: SAD-oriented coarse search, at most 8 commands.
- Refinement: Laplacian-oriented refinement, at most 8 commands.
- Total: at most 16 commands and 4 reversals.

The controller is not continuous AF and does not extend the `lens_focus()` API or physical command limits. Large initial defocus can exceed its bounded reach.

## Lens API boundary

The Legacy declaration is:

```c
int lens_focus(int num_steps, int stepsize, int wait, int extra_delay);
```

With `wait=1`, SmoothAF sends one synchronous command, waits for the Legacy completion path and records the return value. A return of success means the Legacy error flag was not set; it does not independently prove physical movement or acquired focus. See [Research](RESEARCH.md) for the separate Canon AF investigation.

## Canon AF Assist boundary

The v0.4 PoC reserves a read-only `Canon AF Assist` menu but uses an unavailable adapter. It does not call `lens_autofocus()`, Lua internals, `PTP_AF_START`, `lens_setup_af()`, `module_send_keypress()` or any unverified Canon symbol. The adapter starts `UNAVAILABLE`, returns `No safe Canon C API`, creates no task and cannot move the lens. A future native implementation requires a verified C declaration, implementation/export, ABI, `600D_102.SYM` resolution, request/result correlation, cleanup, cancellation semantics and controlled hardware validation.

## Diagnostics and passive Vlog Monitor

Debug Logging is off by default and uses a fixed 32-entry in-memory circular history. Records include mode, phase, score/best score, direction, movement, position, command/reversal counts, elapsed time, `lens_focus()` return value and task-end reason. The on-camera viewer supports read-only Previous/Next navigation and Clear History only when no task is active. It performs no continuous card writes and no lens movement.

`Diagnostic > Measure` is a single read-only LiveView sample. It does not call `lens_focus()` and can run with `Enable=OFF`; it publishes SAD and Laplacian scores from the same ROI sample, plus mean luminance, confidence and error status.

`Vlog Monitor` is passive in v0.5. Start/Stop changes only viewer state and consumes new manual samples. It has no periodic task, no automatic VRAM sampling and no lens movement. `Sampling Interval` is `manual/N/A`. Its heuristic states do not establish perfect focus, and invalid or missing samples pause or remain uncertain rather than proving blur.

## Legacy FastAF baseline and migration

FastAF 0.1 is historical evidence, not evidence that the renamed SmoothAF module is physically validated. The migration renamed the module and symbols while preserving the bounded focus logic, task limits and the `MENU_EOL` registration correction. PANIC, Rearm and the separate Safe Mode concept are not part of the current v0.5 controls.

The historical review identified and addressed, in that baseline work:

- alignment with the Legacy `Makefile.modules` convention;
- reservation before task creation and task-create failure handling;
- cancellation checks between commands and after measurement;
- explicit blocks for focus conflicts;
- UYVY indexing as `row[x * 2 + 1]`;
- bounded commands, synchronous `lens_focus(wait=1)` and explicit unload restrictions.

Historical review findings are not claims that every old artifact has identical provenance. The preserved FastAF records and their differing hashes are listed in [Testing](TESTING.md).

## State ownership rules

Only one SmoothAF task may own movement. Measurement and controller state are bounded and deterministic in host tests. Stop prevents future SmoothAF commands but cannot retroactively cancel a synchronous Legacy operation already sent. Any future Canon AF integration must add a separate serialized state machine and mutual exclusion with `lens_focus()`; it must not bypass these rules.
