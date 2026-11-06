# Changelog

## SmoothAF v0.8.0-beta.1 — Experimental pre-release

- Prepared the public source tree for the v0.8.0-beta.1 pre-release without publishing private Legacy dependencies, camera symbols, local logs or historical build artifacts.
- Preserved the bounded AF, monitor, diagnostics, task-lifecycle and logger implementation from the physically tested v0.8 candidate.
- Added explicit documentation for manual installation, checksum verification, SD-card backup, recovery and cooperative STOP/PANIC behavior.
- The release artifact is `smoothaf.mo`, Build ID `31912f4d2e08-600D.102`, SHA-256 `494aa507f44dccf8e7403b0106cf7c6bfa8b91f5c0f36a13720c2d9708f28a4c`, size `131444` bytes.
- The artifact was built from source commit `31912f4d2e089b320b4664eac92ae665e4521f17`; repository hygiene was committed separately as `90a21851624a1474195607d49e44ad49aa8235a4`.
- `ACTIVE CONTINUOUS`, Movie Active and recovery remain experimental. They are not stable or hunting-free features and are not generalized compatibility claims.

## SmoothAF v0.8-pre-refactor — Experimental

- Removed Canon AF Assist UNAVAILABLE adapter and all trigger UI.
- Removed complete logging/history buffers, viewer, FIO export, and filesystem diagnostics.
- Retained only one fixed-RAM telemetry record for score, timing, direction, step, state, error, and reason.
- Preserved LiveView ROI/SAD/Laplacian measurement, bounded focus controllers, cancellation, timeout, task lifecycle, and passive monitor.

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

This release is experimental. Its continuous path is not stable or guaranteed to avoid hunting, and it does not claim compatibility with other cameras, firmware, lenses or Legacy revisions. Host benchmarks are simulations and do not prove physical focus performance.

## Historical baseline

FastAF 0.1 artifacts and validation records remain unchanged.
