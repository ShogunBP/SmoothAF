# SmoothAF

**Fast focus. Minimal movement.**

SmoothAF is an experimental autofocus module for Magic Lantern, designed to achieve fast, accurate and smooth focusing with minimal lens movement. Built for Canon EOS cameras, it aims to reduce focus hunting through efficient contrast detection, optimized focus control and lightweight algorithms.

## Overview

SmoothAF performs single-shot contrast-based focus experiments in LiveView. It reads a bounded YUV422 region, scores image contrast, and uses a bounded hill-climbing search to probe focus direction. The module is conservative: no focus movement occurs while loading, activation is manual, and every search is limited.

This repository is an independently organized source distribution designed to integrate with a Magic Lantern Legacy Lua Fix tree. It does not replace Magic Lantern or its loader.

## Features

- Contrast autofocus using SAD and Laplacian metrics.
- Bounded hill-climbing search.
- Configurable Step Size.
- Safe Mode with `Enable` off at module load.
- `PANIC / Emergency Stop` and explicit rearm.
- Diagnostic measurement and one-step focus command.
- Runtime status, blocking reason, `lens_focus()` call marker and return value.
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
9. Use `PANIC / Emergency Stop` when needed. A synchronous lens command already in flight may finish.

Do not unload or reload the module while a task is active.

## Known Limitations

- Physical tests with the Canon EF 50mm f/1.8 STM found Step Size 3 working and stopping automatically.
- Step Sizes 1 and 2 did not work in the recorded tests with that lens.
- The search can fail when the lens starts very far out of focus.
- Continuous AF is not implemented.
- Other cameras, firmware versions, and lenses are not validated.
- The module is experimental; static build validation does not prove runtime ABI compatibility or physical lens movement.
- `lens_focus()` returning success does not by itself prove physical movement.

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
