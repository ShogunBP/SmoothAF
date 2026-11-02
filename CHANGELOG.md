# Changelog

## SmoothAF v0.8.0 — RAM telemetry priority

- Temporarily disabled filesystem export and FIO diagnostics in the normal menu and build path.
- Preserved fixed-size RAM history, AF telemetry, `Log Page`, `Log Entry`, `Previous`, `Next` and `Clear History`.
- Added a host RAM diagnostics test covering disabled/enabled logging, chronological order, wrap-around, dropped entries, timestamps and clear.
- No FIO operation is performed during initialization, autofocus, monitor or normal diagnostics.
- No firmware, stubs, loader, autofocus algorithm, SD installation, push, merge or tag was changed or performed.

## SmoothAF v0.8.0 — FIO context diagnostic

- Replaced the callback-context probe with `Diagnostic > Test Root Create` and `Diagnostic > Test Root Edit` for the Legacy `ML_CARD` root.
- Root Edit requires a preallocated 4096-byte file and uses only `FIO_OpenFile(..., O_RDWR | O_SYNC)`, `FIO_SeekSkipFile(..., SEEK_SET)`, `FIO_WriteFile` and `FIO_CloseFile`; it does not create, truncate, append, remove or enlarge the file.
- Withheld `Test FIO Task` from the physical menu because Legacy `deinit()` has no join/kill guarantee; a bounded shutdown wait cannot prove task exit before unload.
- Preserved autofocus, monitor, scheduler, movement and the existing AF log exporter.
- Added read-only `Diagnostic > Test File Paths` for simple, `A:/`, `B:/` and `ML/` forms of the prepared root file; it reports existence, raw size return, size, read-only open and close, plus `ML_CARD`/`SHOOTING_CARD` without changing selection or modifying the card.
- No physical test, SD installation, push, merge or tag was performed by this change.
- ARM staging validation passed with `DEP=23`, `Will load on: 600D`, ELF ARM/EABI5, size 43480 bytes and SHA-256 `2f6d1878ec7a7d890fa303da94d01b5fa03caae503f63ec47038ee1c6197c4e9`.


## SmoothAF v0.8.0 — Log export

- Added manual, post-`Stop` text export of the fixed RAM diagnostic snapshot through the Legacy `FIO_*` file API.
- Export requires `Task State EXITED`, writes a new `ML/LOGS/SMOOTHAF-###.LOG`, reports dropped events and propagates write failures.
- Export diagnostics now distinguish internal error `5` (`FILE_CREATE`) from raw directory/write returns and show the attempted path in the read-only menu value.
- No continuous card writes, focus algorithm changes, firmware/loader changes or automatic SD installation.


## SmoothAF v0.8.0 — Experimental Local Search

- Added a short, contrast-guided local focus search inside the existing shared monitor task.
- Limited the first experiment to four commands and one reversal per manually armed episode.
- Added post-move score, delta, best score and relative-position indicators.
- Preserved `smoothaf_move()` as the only physical lens boundary and kept DRY RUN physical-free.
- Host and ARM validation remain evidence of deterministic logic and compilation only; physical v0.8 validation is separate.


## SmoothAF v0.7.0 — Experimental POC

- Added the first Vlog AF proof of concept inside the existing shared monitor task.
- Added `OFF`, `DRY RUN` and explicitly armed `ACTIVE` modes; DRY RUN never calls `smoothaf_move()`.
- Fixed ACTIVE observability: separate move attempts, lens API calls, API result, block reason, per-episode command count and explicit episode result.
- Limited the first ACTIVE test path to one command per episode with no automatic reversal; COMPLETE never implies physical movement.
- Added explicit no-movement, blocked, failed, cancelled and API-success outcomes; physical displacement still requires camera observation.
- Preserved the existing movement boundary and block checks; no Movie Mode, recording, MF or competing focus-controller bypass is intended.
- This is contrast-based experimental correction, not Canon Native Continuous AF. Host and ARM validation do not establish physical focus quality.
- No physical v0.7 validation, SD-card installation, push, merge or tag was performed.

## SmoothAF v0.6.0 — Experimental

- Added an asynchronous, read-only Vlog Monitor using the shared SmoothAF task.
- Added configurable 1000 ms, 500 ms default, 250 ms and experimental 100 ms intervals.
- Added no-backlog scheduling, duration/overrun/blocked/invalid counters and cooperative Stop.
- Added the read-only `Vlog Monitor > Monitor Performance` submenu with individual task, interval, duration, counter and last-error indicators; unavailable values display `N/A` and metrics remain visible after Stop.
- Added conservative reference adaptation, hysteresis, persistent blur transitions and lighting/low-confidence uncertainty handling.
- Automatic samples do not create one log record each; only monitor session and important transitions use the fixed 32-entry history.
- Preserved manual Measure, Legacy HC, Adaptive HC, Normal AF, Movie/recording gates and all lens movement boundaries.
- The monitor never calls `lens_focus()`, Canon AF, half-shutter, exposure controls or VRAM writes.
- ARM staging build passed with `Will load on: 600D`, `DEP=23`, ELF ARM EABI5, 24352 bytes and SHA-256 `7d55bb2d98f7d73bf2c8620623519f84afe85a1eb3ec23467ddf30e98254ddce`.
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
