# SmoothAF Testing and Validation

## Evidence classes

- **Host test:** deterministic logic and safety checks, not camera evidence.
- **ARM build validation:** compilation, dependency resolution and target selection, not runtime ABI or physical movement evidence.
- **Physical test:** camera, firmware, Legacy revision, lens and controlled scene are recorded; results do not generalize beyond those conditions.
- **Simulation:** benchmark workload only; every reported benchmark row must remain labeled `(SIMULATION)`.

See [Development](DEVELOPMENT.md) for commands and [Architecture](ARCHITECTURE.md) for runtime safety rules.

## Local validation procedure

Run the repository checks from PowerShell:

```powershell
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -Release
```

The test set covers core metrics, UYVY ROI indexing, bounds, cancellation, timeout, Safe Mode, PANIC, Rearm, task reservation, simulated lens/measurement failures, menu regression and deterministic benchmark invariants. The benchmark runs Legacy HC, Adaptive HC and Normal AF on the same deterministic scenarios at simulated Step Sizes 1, 2 and 3. Synthetic ticks are workload units, not wall-clock or lens time.

The ARM build uses the compatible Legacy Lua Fix tree and private `600D_102.SYM` input supplied outside the repository. Dependency output `DEP=23` and `Will load on: 600D` confirm build-time resolution only.

## SmoothAF v0.3 build record

- Artifact: `artifacts/consolidated/smoothaf.mo`
- Size: 16460 bytes
- SHA-256: `b7639ab3afca22ddc55b39e6b0f949363c16fd29aa62f2c1bd4d41a89ba1f8cb`
- Dependency check: 23 dependencies; `Will load on: 600D`
- Release: `artifacts/releases/smoothaf-b7639ab3afca22ddc55b39e6b0f949363c16fd29aa62f2c1bd4d41a89ba1f8cb`

The v0.3 artifact includes the fixed 32-record viewer and diagnostic payloads. Normal AF has not been physically validated. No continuous AF or recording-time movement is enabled.

## SmoothAF v0.2 physical record

Reported observations used an EOS 600D / Rebel T3i, firmware 1.0.2, Magic Lantern Legacy Lua Fix, Canon EF 50mm f/1.8 STM, Step Size 3 and Laplacian. They were not a controlled same-scene comparison:

| Mode | Result | Score | Best | Mean | Time | Commands | Reversals |
|---|---|---:|---:|---:|---:|---:|---:|
| Legacy HC | `Search Limit` | 2677 | 2877 | 74 | 1381 ms | 3 | 1 |
| Adaptive HC | `Search Limit` | 3059 | 3087 | 55 | 2374 ms | 4 | 3 |

The module loaded, menus worked and micro-adjustments were observed. Large focus-distance changes were not recovered reliably. Debug Logging did not yet provide a complete on-camera history viewer in that record. Step Sizes 1 and 2 did not work in the recorded tests. These observations do not establish Adaptive HC superiority or compatibility beyond the stated setup.

## FastAF 0.1 historical records

The preserved baseline is historical and is not a substitute for testing `smoothaf.mo`.

### Baseline artifact

- Historical module: `fastaf.mo`
- Size: 14468 bytes
- Format: ARM EABI5 ELF32 relocatable module
- Target: Canon EOS 600D / Rebel T3i, firmware 1.0.2
- Magic Lantern: Legacy Lua Fix
- Lens: Canon EF 50mm f/1.8 STM
- Baseline SHA-256: `123eec5c255d47a01927a719a3905135589a0916c199056860c032d111e2a113`

Recorded baseline observations: Step Size 3 found focus and stopped automatically; Step Sizes 1 and 2 failed in the recorded tests; search could fail from severe defocus; and the `MENU_EOL` registration ASSERT was corrected. Safe Mode, PANIC, explicit Rearm and bounded movement were part of the documented safety model.

### Separate validation record

A second historical validation document identified a different artifact:

- Size: 14308 bytes
- SHA-256: `BC2AC402D9F34194424DA39DB78345E981BF9EA6FC289C169BFBF1706434D37D`
- Format: ELF32 LSB relocatable, ARM EABI5, little-endian
- Reported dependency count: 23
- Runtime resolution of `__aeabi_idiv`, `lens_focus`, task, VRAM, focus, semaphore and menu symbols was not proven against the installed camera.

The two hash/size pairs are preserved as distinct historical provenance records. This documentation does not claim they describe the same file.

## Historical validation and unload limits

The older FastAF validation covered host tests, generated strings, ARM output and dependency checks in an isolated Legacy tree. It also recorded that the Legacy loader calls `deinit()`, ignores its return value and provides no general task join/kill. A synchronous `lens_focus(wait=1)` may remain blocked during deinitialization. Do not unload, reload or replace a module while a task is active.

The physical test protocol remains conservative:

1. Confirm the final artifact hash and back up the card.
2. Copy only the module manually to `ML/MODULES/`.
3. Confirm module load, menu visibility, `Enable=0` and no movement during loading.
4. Start with read-only `Diagnostic > Measure`.
5. Test one controlled `Focus Step`, then PANIC while the interface is responsive.
6. Record camera, firmware, Legacy revision, lens, Step Size, initial focus condition and diagnostics.
7. Do not test Continuous AF, Movie AF or recording-time movement in the current release.
8. Never unload or reload during an active task.

No SD-card installation or firmware/loader modification is part of repository validation.
