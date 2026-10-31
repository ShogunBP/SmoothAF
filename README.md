# SmoothAF

**SmoothAF v0.6.0 — Experimental**

**Fast focus. Minimal movement.**

SmoothAF is an experimental autofocus module for Magic Lantern, designed for bounded single-shot focus experiments and read-only sharpness monitoring. It is built for Canon EOS cameras and does not implement automatic autofocus in v0.6.

## Overview

SmoothAF performs single-shot contrast-based focus experiments in LiveView. It reads a bounded YUV422 region, scores image contrast, and uses a bounded hill-climbing search only when explicitly requested. The asynchronous Vlog Monitor is read-only: it samples the central ROI periodically, never moves the lens, and never changes exposure.

This repository is an independently organized source distribution designed to integrate with a Magic Lantern Legacy Lua Fix tree. It does not replace Magic Lantern or its loader.

## Features

- Contrast metrics using SAD and Laplacian from the same frame.
- Bounded `Legacy HC`, experimental `Adaptive HC` and bounded single-shot `Normal AF` modes.
- Reserved `Canon AF Assist` menu with an unavailable adapter; it never moves the lens.
- `Enable` off at module load as explicit movement armament.
- Cooperative `Stop`; an in-flight synchronous lens command may finish.
- Read-only `Diagnostic > Measure`, including with `Enable` off.
- Asynchronous passive `Vlog Monitor` using one shared SmoothAF task.
- Sampling intervals of 1000 ms, 500 ms (default), 250 ms and 100 ms experimental.
- No backlog: each sample schedules the next one from completion time.
- `Vlog Monitor > Monitor Performance` is a read-only submenu exposing task state, configured/actual start-to-start interval, duration last/average/max, attempts, valid/invalid, low confidence, blocked/skipped, overruns and last error. Unavailable intervals/durations display `N/A`; values remain visible after cooperative Stop.
- Actual interval is start-to-start; the first sample has no interval. Duration covers an executed measurement, including invalid results. Blocked and skipped cycles are not counted once per polling sleep.
- Hysteresis, persistent blur detection, low-confidence handling and lighting-change uncertainty.
- Fixed 32-record Debug Logging viewer with read-only navigation and task-safe Clear History.
- Docker host and ARM build validation.

## Monitor safety boundary

The monitor never calls `lens_focus()`, half-shutter, Canon AF, exposure controls or VRAM writes. It does not create a task per sample. Manual Measure, Focus Step and Focus Test are not run concurrently with the monitor task. Start/Stop are explicit and Stop is cooperative.

Legacy has no general task join/kill guarantee. Do not unload, reload or replace the module while any SmoothAF task is active, even after a timeout. The monitor reduces this risk because it does not call the lens, but it does not eliminate the loader limitation.

## Compatibility

Build target:

- Canon EOS 600D / Rebel T3i.
- Canon firmware 1.0.2.
- Magic Lantern Legacy Lua Fix.
- Canon EF 50mm f/1.8 STM used in earlier physical testing.

The v0.6 ARM build is compilation/dependency evidence only. No physical v0.6 validation has been performed. No compatibility claim is made for other cameras, firmware versions, Legacy branches or lenses.

## Installation

Make an SD-card backup first. Copy the built module manually to:

```text
ML/MODULES/smoothaf.mo
```

The repository never copies files to the card automatically. Do not replace `AUTOEXEC.BIN`, firmware or the loader.

## Building

Requirements and private Legacy integration inputs are documented in [docs/DEVELOPMENT.md](docs/DEVELOPMENT.md). The short command set is:

```powershell
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1 -Clean
.\build-smoothaf.ps1 -Release
```

ARM validation uses a temporary staging copy of the Legacy tree and the supplied `600D_102.SYM`; the original `docs/references` tree is not modified. The v0.6 experimental artifact is `artifacts/v0.6/smoothaf.mo`.

## First physical monitor test

1. Back up the card and copy only `artifacts/v0.6/smoothaf.mo` manually to `ML/MODULES/smoothaf.mo`.
2. Use photographic LiveView, not Movie Mode, with recording stopped.
3. Load the module and confirm `Enable=OFF` and no lens movement during loading.
4. Set `Vlog Monitor > Sampling Interval` to `1000 ms`.
5. Start `Vlog Monitor`; confirm that no lens movement, half-shutter action or Canon AF occurs.
6. Observe `Monitor Status`, SAD, Laplacian, reference, smoothed score and sample count for a static scene.
7. Change the scene or deliberately defocus manually; confirm uncertainty/possible-blur behavior without automatic correction.
8. Press Stop and confirm that values freeze and no new samples appear.
9. Never unload or reload while `Status` or monitor task activity indicates a task is active.
10. Record camera, firmware, Legacy revision, lens, interval, metric, scene and observed counters.

## Known Limitations

- Thresholds are heuristics and require physical validation.
- The 100 ms interval is experimental and may be disabled if unstable.
- Stop cannot interrupt a synchronous `lens_focus(wait=1)` command already in flight.
- Deinit cannot prove safe unload if a task remains active after its bounded wait.
- Static ARM validation does not prove runtime ABI compatibility or physical movement.
- The benchmark is simulation only.
- Canon AF Assist remains blocked because no safe Canon C API is proven.

## Documentation

- [Architecture](docs/ARCHITECTURE.md)
- [Development](docs/DEVELOPMENT.md)
- [Research](docs/RESEARCH.md)
- [Testing](docs/TESTING.md)
- [Roadmap](docs/ROADMAP.md)
- [Changelog](CHANGELOG.md)
