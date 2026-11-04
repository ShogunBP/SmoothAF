# SmoothAF

**SmoothAF v0.2.0 - Experimental**

**Fast focus. Minimal movement.**

SmoothAF is an experimental autofocus module for Magic Lantern, designed to achieve fast, accurate and smooth focusing with minimal lens movement. Built for Canon EOS cameras, it aims to reduce focus hunting through efficient contrast detection, optimized focus control and lightweight algorithms.

## Overview

SmoothAF performs single-shot contrast-based focus experiments in LiveView. It reads a bounded YUV422 region, scores image contrast, and uses a bounded hill-climbing search to probe focus direction. The module is conservative: no focus movement occurs while loading, activation is manual, and every search is limited.

This repository is an independently organized source distribution designed to integrate with a Magic Lantern Legacy Lua Fix tree. It does not replace Magic Lantern or its loader.

## Features

- Contrast autofocus using SAD and Laplacian metrics.
- Bounded hill-climbing search.
- Configurable Step Size.
- `Enable` remains off at module load; lens movement is always manual and bounded.
- Read-only periodic Vlog Monitor with SAD/Laplacian, ROI, blur, and performance counters.
- Movie Dry Run probe for normal H.264 recording; it never calls the lens adapter.
- Diagnostic measurement and one-step focus command.
- Runtime status plus one fixed-RAM telemetry record for score, timing, direction, step, state, error, and reason.
- Optional persistent structured logging with a fixed RAM ring and disabled-by-default FIO consumer; no physical SD validation is claimed.
- Reproducible host and ARM builds with Docker.

## Compatibility

Validated target:

- Canon EOS 600D / Rebel T3i.
- Canon firmware 1.0.2.
- Magic Lantern Legacy Lua Fix.
- Canon EF 50mm f/1.8 STM used in physical testing.

No compatibility claim is made for other cameras, firmware versions, Magic Lantern branches, or lenses.

## Installation

Make an SD-card backup first. Copy the built module manually to:

```text
ML/MODULES/smoothaf.mo
```

The Legacy loader accepts `.mo` files from `ML/MODULES` and derives the module name from the filename, so the renamed artifact must be named `smoothaf.mo`. Confirm that the module appears in the Magic Lantern module list before enabling it. This repository never copies files to the card automatically.

On failure, power the camera off, remove the module using a computer or card reader, and boot without it. Do not replace `AUTOEXEC.BIN`, firmware, or the loader.

## Building

Requirements:

- Windows PowerShell 5.1 or PowerShell 7.
- Docker Desktop with a running daemon.
- Docker Desktop with permission to build the `smoothaf-legacy-build:bookworm` image from `docker/Dockerfile`.
- A compatible Magic Lantern Legacy Lua Fix source tree for integration.
- The private `600D_102.SYM` file supplied separately by the camera owner. It is intentionally not tracked here.

From the repository root:

```powershell
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1 -Clean
.\build-smoothaf.ps1 -Release
```

The build creates the Docker image, runs the host tests with `-TestOnly`, copies the project into a temporary Legacy tree, generates module strings, builds the ARM module with `make clean all` (never `nodep`), checks dependencies, and computes SHA-256. The build never accesses an SD card or modifies a Magic Lantern tree permanently. `-Release` additionally writes a hash-named copy under `artifacts/releases/`.

## Usage

1. Load the module and confirm that no lens movement occurs during loading.
2. Confirm `Enable` is off.
3. Use `Diagnostic > Measure` for a read-only measurement.
4. Enable the module manually.
5. Use `Diagnostic > Focus Step` for one controlled step.
6. Set `Step Size` only after considering the tested-lens limitations.
7. Use `Focus Test` only after the single-step path is stable.
8. Read `Status` for score, timing, blocking reason, task result, and `lens_focus()` diagnostics.
9. Use `Stop` when needed. A synchronous lens command already in flight may finish.

Do not unload or reload the module while a task is active.

## Known Limitations

- Physical tests with the Canon EF 50mm f/1.8 STM found Step Size 3 working and stopping automatically.
- Step Sizes 1 and 2 did not work in the recorded tests with that lens.
- The search can fail when the lens starts very far out of focus.
- Continuous AF is not implemented.
- Other cameras, firmware versions, and lenses are not validated.
- The module is experimental; static build validation does not prove runtime ABI compatibility or physical lens movement.
- `lens_focus()` returning success does not by itself prove physical movement.
- SmoothAF v0.2.0 is experimental and has no continuous AF.
- The recorded physical tests reported successful module loading, working menus and micro-adjustments, but did not recover large focus-distance changes reliably.
- Persistent logging and FIO export are optional, disabled by default, and not physically validated; runtime telemetry remains fixed-size RAM state.
- The recorded EF 50mm f/1.8 STM tests used Step Size 3; Step Sizes 1 and 2 did not work in those tests.
- Legacy HC and Adaptive HC results below are not a controlled same-scene comparison and do not establish that Adaptive HC is superior.

### Recorded physical observations

These observations were supplied for an EOS 600D / Rebel T3i with firmware 1.0.2, Magic Lantern Legacy Lua Fix, EF 50mm f/1.8 STM, Step Size 3 and the Laplacian metric. They are not a controlled comparison:

| Mode | Result | Score | Best | Mean | Time | Commands | Reversals |
|---|---|---:|---:|---:|---:|---:|---:|
| Legacy HC | Search Limit | 2677 | 2877 | 74 | 1381 ms | 3 | 1 |
| Adaptive HC | Search Limit | 3059 | 3087 | 55 | 2374 ms | 4 | 3 |

The module loaded on the camera, the menus worked and micro-adjustments were observed. Large changes in focus distance were not recovered reliably. These observations do not validate compatibility with other cameras, firmware, lenses or Legacy revisions.

## Roadmap

Possible future work, not implemented in this release:

- More systematic calibration across supported lenses.
- Better recovery and diagnostics for out-of-focus starting positions.
- Additional hardware validation across supported Legacy ports.
- Improved packaging and release automation.

## Contributing

Keep changes narrow and preserve the manual-activation and bounded-movement safety model. Run host tests and the full Docker build before submitting a change. Do not include SD-card backups, firmware dumps, private symbol tables, credentials, or personal build logs. Report the camera model, firmware, Magic Lantern revision, lens, Step Size, starting-focus condition, and exact diagnostic status when reporting a problem.

## License

SmoothAF is derived from code intended to integrate with Magic Lantern Legacy. License and attribution obligations for surrounding Magic Lantern APIs and copied or adapted components must be reviewed before redistribution. This repository does not add a new project license until those obligations are confirmed. See `docs/LICENSE-NOTES.md`.

## Acknowledgments

SmoothAF acknowledges the Magic Lantern project and its Legacy Lua Fix ecosystem. The module relies on Magic Lantern interfaces including menu registration, LiveView VRAM access, task creation, semaphores, and `lens_focus()`.

Useful references:

- [Magic Lantern](https://www.magiclantern.fm/)
- [Magic Lantern source repository](https://github.com/reticulatedpines/magiclantern_simplified)
- [SmoothAF issue tracker](https://github.com/ShogunBP/SmoothAF/issues)
