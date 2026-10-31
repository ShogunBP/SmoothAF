# Changelog

## SmoothAF v0.6.0 — Experimental

- Added an asynchronous, read-only Vlog Monitor using the shared SmoothAF task.
- Added configurable 1000 ms, 500 ms default, 250 ms and experimental 100 ms intervals.
- Added no-backlog scheduling, duration/overrun/blocked/invalid counters and cooperative Stop.
- Added conservative reference adaptation, hysteresis, persistent blur transitions and lighting/low-confidence uncertainty handling.
- Automatic samples do not create one log record each; only monitor session and important transitions use the fixed 32-entry history.
- Preserved manual Measure, Legacy HC, Adaptive HC, Normal AF, Movie/recording gates and all lens movement boundaries.
- The monitor never calls `lens_focus()`, Canon AF, half-shutter, exposure controls or VRAM writes.
- ARM staging build passed with `Will load on: 600D`, `DEP=23`, ELF ARM EABI5, 24200 bytes and SHA-256 `9909e7c650530765bd94ac424c9d986d10f615ff3918eade70e32ee943a78e01`.
- Host core, monitor, benchmark and menu regression checks passed.
- No automatic autofocus, physical v0.6 validation, SD-card installation, push, merge or tag was performed.

### Known v0.6 limitations

- Thresholds remain heuristics and require physical validation on the target camera and lens.
- Legacy provides no general task join/kill guarantee; unloading while a task is active remains unsafe.
- The 100 ms interval is experimental and may be disabled if physical testing shows instability.

## SmoothAF v0.5.0 — Experimental

- Removed PANIC, Rearm and the separate Safe Mode concept from the current SmoothAF controls.
- Preserved `Enable=OFF` at load, cooperative `Stop`, single-task ownership, movement limits, timeout and conflict gates.
- Fixed `Diagnostic > Measure` so it is read-only, independent of Enable and computes SAD and Laplacian from the same LiveView sample.
- Centralized measurement publication across Measure, Focus Step and Focus Test, including distinct valid, low-confidence and error outcomes and independent SAD/Laplacian payloads.
- Fixed Debug Logging publication diagnostics and recent-entry navigation; semaphore publication failures are counted instead of being silent.
- Added passive `Vlog Monitor` status, scores, reference/smoothing, session sample count and `manual/N/A` sampling information; Start waits for a new sample, Stop freezes the session, and the monitor does not create a periodic task or move the lens.
- Preserved Canon AF Assist as an unavailable, blocked adapter with no lens movement.
- Docker host validation passed; benchmark output remains `(SIMULATION)`. ARM validation was performed in temporary staging and did not replace the consolidated artifact.
- No physical v0.5 validation, SD-card installation, push, merge or tag was performed for v0.5.
