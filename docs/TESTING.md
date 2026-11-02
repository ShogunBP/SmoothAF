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

The v0.6 test set covers core metrics, simultaneous SAD/Laplacian ROI calculation, bounds, cancellation, timeout, task reservation, simulated lens/measurement failures, asynchronous monitor scheduling, intervals, wrap-around, hysteresis, lighting uncertainty, performance snapshot semantics (attempts, valid/invalid, low confidence, blocked/skipped, overruns, start-to-start interval, duration, N/A and post-Stop persistence), individual performance submenu regression and deterministic benchmark invariants. Monitor tests use a fake logical clock and no real camera task or VRAM. Automatic samples update fixed in-memory metrics and do not create one diagnostic record per sample. The benchmark runs Legacy HC, Adaptive HC and Normal AF on the same deterministic scenarios at simulated Step Sizes 1, 2 and 3. Synthetic ticks are workload units, not wall-clock or lens time.

The ARM build uses the compatible Legacy Lua Fix tree and private `600D_102.SYM` input supplied outside the repository. Dependency output `DEP=23` and `Will load on: 600D` confirm build-time resolution only.

## SmoothAF v0.8 validation procedure

The v0.8 POC must be validated in this order:

The normal build does not include filesystem export or FIO probes. `Export AF Log`, `Test SD Write`, Root FIO tests and path probes are absent from the normal menu. Logging and AF telemetry are fixed-size, volatile and RAM-only; no FIO operation is performed during initialization, autofocus, monitoring or normal diagnostics. The experimental FIO sources remain isolated and are not compiled by the default `-TestOnly` path.

1. Run `.\build-smoothaf.ps1 -TestOnly` and preserve the `(SIMULATION)` label on benchmark output.
2. Use host tests to validate mode, arm, episode, four-command limit, settle, cooldown, no-reversal behavior and Stop. Host tests do not prove lens movement safety on camera.
3. Build ARM only in an isolated staging copy. Validate `Will load on: 600D`, `DEP=23` and ELF ARM/EABI5. Save the experimental module only as `artifacts/v0.8/smoothaf.mo`.
4. Test DRY RUN first in photographic LiveView with recording stopped. Confirm state/request changes and zero lens movement.
5. Test Movie Dry Run separately in normal H.264 recording. Start the monitor before recording, select `Vlog AF Mode=DRY RUN`, manually arm one episode, and use a second phone/camera to record the ML display. Confirm recording start/end events, sample count, validity/confidence, score, duration, blur transitions and zero `AF Lens Calls`; custom and RAW recording must remain blocked. The existing menu is the least-invasive visibility path, but it may not remain open or refresh over the recording UI; treat its values as opportunistic and use the fixed RAM Log Page/Log Entry after the clip as the authoritative fallback.
6. Exercise four short scenes: initially focused static target, nearly focused target, heavily defocused target and a subject approaching/receding. Use short clips first; stop immediately on recording corruption, task stall, abnormal heat, battery/card warning or any unexpected lens command.
7. Movie ACTIVE is now available only as an explicitly authorized experiment: set `Movie Active Experimental=ON`, `Enable=ON`, select one-shot `ACTIVE` or persistent `ACTIVE CONTINUOUS`, start monitoring, and manually arm. Use normal H.264 Movie only; RAW/custom, MF and competing focus controllers remain blocked. Begin with Test A (one command), then bounded Test B, and only then Test C continuous episodes. Do not enable B or C automatically. Do not unload/reload while the shared task is active.

Test A: H.264 recording, experimental ON, one-shot ACTIVE, episode limit 1, verify at most one STM command and intact clip. Test B: H.264 recording, one-shot ACTIVE, bounded episode search, verify controlled completion. Test C: H.264 recording, ACTIVE CONTINUOUS, one manual arm, multiple episodes, verify cooldown/blur-clear latch and STOP/PANIC interruption.
8. For Movie Active tests record `Movie Recording State`, mode, `AF Move Attempts`, `AF Lens Calls`, `AF Last Block`, `AF Last Move Result`, `AF Commands This Episode`, `AF Episode Result`, scores, delta, direction, cooldown and episode count. `AF Lens Calls=1` proves only API invocation, not physical displacement. Stop immediately on recording corruption, unexpected lens activity, task stall, abnormal heat or battery/card warning.
9. For any existing non-Movie ACTIVE test, record the same AF fields.
9. `AF Lens Calls=1` proves only that `lens_focus()` was invoked. It does not prove physical displacement. `COMPLETE` alone is not success; a zero-call episode must show a no-movement, blocked or cancelled result.

A synchronous `lens_focus(wait=1)` may finish after Stop. The Legacy loader has no general task join/kill guarantee; a task still active after the bounded deinit wait remains an unload hazard.

## SmoothAF v0.8 validation record

- Host tests: passed (`All SmoothAF core tests passed`; `SmoothAF monitor tests passed`; `SmoothAF diagnostics RAM tests passed`).
- Comparative benchmark: passed; scenarios reported `invariants=PASS` and output remained `(SIMULATION)`.
- Menu regression: passed (`SmoothAF menu regression tests passed`).
- Episode telemetry host coverage includes bounded RAM event/summary records, timestamp wraparound, overflow accounting and retained terminal summaries. Physical lifecycle behavior remains pending.
- `git diff --check`: passed; line-ending conversion warnings are non-fatal.
- ARM staging build: passed in a temporary copy of the Legacy Lua Fix tree with `600D_102.SYM`; the source Legacy tree was not modified.
- The final build output includes the commit-derived Build ID, `Will load on: 600D` and `DEP=23`; exact final provenance is recorded from the final build command output rather than copied from a historical artifact.
- The staged module is an `ELF 32-bit LSB relocatable, ARM, EABI5 version 1 (SYSV)` `.mo`; undefined symbols are Legacy-resolved imports, including `lens_focus`, `get_ms_clock`, VRAM, task, menu and semaphore APIs. The Legacy build does not emit a separate `.elf`; the `.mo` is the inspected relocatable ELF.
- The embedded Build ID is checked against the final commit. The pre-build v0.8 artifacts remain preserved in the existing untracked `artifacts/v0.8/smoothaf.mo.pre-*` backups; no backup was deleted.
- No physical v0.8 or Movie validation or SD-card installation has been performed. All camera results, including zero lens movement in Dry Run and optical focus/displacement, remain pending. Movie Dry Run must have zero physical lens commands; Movie ACTIVE is not enabled for recording.
- FIO export and filesystem probes are intentionally not part of the normal v0.8 validation path. The normal menu performs no card writes; host validation covers only volatile RAM telemetry and history. Experimental FIO tests remain isolated for a future explicitly enabled build and must not be treated as production validation.

## SmoothAF v0.6 validation record

- Branch: `feature/vlog-monitor-async-v0.6`.
- Docker host tests: passed (`All SmoothAF core tests passed`).
- Monitor tests: passed (`SmoothAF monitor tests passed`).
- Comparative benchmark: passed; scenarios reported `invariants=PASS` and output remained `(SIMULATION)`.
- Menu regression: passed (`SmoothAF menu regression tests passed`).
- ARM build: passed in a temporary staging copy of the Legacy tree using `600D_102.SYM`; the original Legacy tree was not modified.
- Build output: `Will load on: 600D`, `DEP=23`.
- Experimental module: `artifacts/v0.6/smoothaf.mo`, 24352 bytes, SHA-256 `7d55bb2d98f7d73bf2c8620623519f84afe85a1eb3ec23467ddf30e98254ddce`.
- Format: `ELF 32-bit LSB relocatable, ARM, EABI5 version 1 (SYSV)`.
- No physical v0.6 validation or SD-card installation was performed.
- The Legacy loader has no general task join/kill guarantee. A task that remains active after deinit's bounded wait is an unload hazard; do not unload, reload or replace the module while a task is active.

The v0.6 result proves compilation and dependency resolution for the supplied Legacy/symbol inputs only. It does not prove runtime ABI compatibility, physical movement, monitor timing on camera, or focus-quality classification.

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
