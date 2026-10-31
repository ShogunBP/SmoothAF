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

The v0.5 test set covers core metrics, simultaneous SAD/Laplacian ROI calculation, bounds, cancellation, timeout, Enable admission, task reservation, simulated lens/measurement failures, passive monitor/menu regression and deterministic benchmark invariants. The runtime publication path now carries valid, low-confidence and invalid measurement outcomes with independent SAD/Laplacian payloads, while the fixed viewer follows newly appended records. The benchmark runs Legacy HC, Adaptive HC and Normal AF on the same deterministic scenarios at simulated Step Sizes 1, 2 and 3. Synthetic ticks are workload units, not wall-clock or lens time.

The ARM build uses the compatible Legacy Lua Fix tree and private `600D_102.SYM` input supplied outside the repository. Dependency output `DEP=23` and `Will load on: 600D` confirm build-time resolution only.

## SmoothAF v0.5 validation record

- Docker host tests: passed (`All SmoothAF core tests passed`).
- Comparative benchmark: passed; every scenario reported `invariants=PASS` and every benchmark row remained marked `(SIMULATION)`.
- Menu regression: passed (`SmoothAF menu regression tests passed`).
- ARM build: passed in a temporary staging copy of the repository and Legacy tree; `Will load on: 600D`, `DEP=23`.
- Experimental staging module: 21736 bytes, SHA-256 `42d2c12e3c9ed462c68b45d8382da542273d4861090d3053bc0edc692d19448b`.
- Format: `ELF 32-bit LSB relocatable, ARM, EABI5`.
- The pre-existing `artifacts/consolidated/smoothaf.mo` was preserved at 17864 bytes with SHA-256 `9a600fc9f3acd92c027095819e8bfdeea8a440c2cf16fd8f28989006d94a566c`.
- No physical v0.5 validation or SD-card installation was performed.

The ARM result proves compilation and dependency resolution for the supplied Legacy/symbol inputs only. It does not prove runtime ABI compatibility, physical movement, focus acquisition or Vlog Monitor behavior on camera.

## SmoothAF v0.4 blocked-adapter build record

The v0.4 build includes only the unavailable Canon AF Assist state boundary. It does not import Canon AF symbols, call Lua internals, send half-shutter events or move the lens.

- Size: 17864 bytes
- SHA-256: `9a600fc9f3acd92c027095819e8bfdeea8a440c2cf16fd8f28989006d94a566c`
- Dependency check: 23 dependencies; `Will load on: 600D`

This ARM result proves compilation and dependency resolution for the blocked adapter only. It does not prove Canon AF Assist support or physical autofocus.

## SmoothAF v0.3 build record

- Artifact: `artifacts/consolidated/smoothaf.mo`
- Size: 16460 bytes
- SHA-256: `b7639ab3afca22ddc55b39e6b0f949363c16fd29aa62f2c1bd4d41a89ba1f8cb`
- Dependency check: 23 dependencies; `Will load on: 600D`
- Release: `artifacts/releases/smoothaf-b7639ab3afca22ddc55b39e6b0f949363c16fd29aa62f2c1bd4d41a89ba1f8cb`

The v0.3 artifact includes the fixed 32-record viewer and diagnostic payloads. Normal AF has not been physically validated. No continuous AF or recording-time movement is enabled.

The v0.4 Canon AF Assist adapter is intentionally unavailable. Host tests can prove deterministic rejection and menu visibility only; they do not prove a Canon AF API, lens movement or physical autofocus.

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
5. Test one controlled `Focus Step`, then use cooperative `Stop` while the interface is responsive.
6. Record camera, firmware, Legacy revision, lens, Step Size, initial focus condition and diagnostics.
7. Do not test Continuous AF, Movie AF or recording-time movement in the current release.
8. Never unload or reload during an active task.

No SD-card installation or firmware/loader modification is part of repository validation.
