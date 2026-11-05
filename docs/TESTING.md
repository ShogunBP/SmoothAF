# SmoothAF Testing and Validation

## Evidence classes

- **Host test:** deterministic logic and safety checks, not camera evidence.
- **ARM build validation:** compilation, dependency resolution and target selection, not runtime ABI or physical movement evidence.
- **Physical test:** camera, firmware, Legacy revision, lens and controlled scene are recorded; results do not generalize beyond those conditions.
- **Simulation:** deterministic host episode/controller simulator and comparative benchmark; every reported simulator or benchmark row must remain labeled `(SIMULATION)`.

See [Development](DEVELOPMENT.md) for commands and [Architecture](ARCHITECTURE.md) for runtime safety rules.

## Local validation procedure

Run the repository checks from PowerShell:

```powershell
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -Release
```

The host test set covers core metrics, simultaneous SAD/Laplacian ROI calculation, bounds, cancellation, fixed-deadline timeout, task reservation, simulated lens/measurement failures, asynchronous monitor scheduling, intervals, wrap-around, hysteresis, lighting uncertainty, provisional-to-valid reference qualification, acquisition limits, 4/6/8/12 budgets, AUTO/recovery reservation, STOP/PANIC gates, continuous lifecycle and failure policy, Focus Test regressions, performance snapshot semantics, individual performance submenu regression, the strict shared-seam episode/controller simulator, and deterministic benchmark invariants. Monitor and simulator tests use fake logical clocks and no real camera task or VRAM. Automatic samples update fixed in-memory metrics and do not create one diagnostic record per sample. The simulator drives the same production episode/controller seam with fake lens acceptance, settle time, post-move scoring, cancellation, and recovery callbacks; its trace and assertions are host evidence only. The benchmark remains separate: it runs Legacy HC, Adaptive HC and Normal AF on the same deterministic scenarios at simulated Step Sizes 1, 2 and 3. Synthetic ticks are workload units, not wall-clock or lens time. Neither host test establishes physical camera safety.

The algorithm-selection tests additionally cover enum normalization, Focus Test isolation, Movie recovery opt-in, bounded coarse/bracket/fine recovery transitions and explicit cancellation. These are host state-machine tests only; they do not validate camera movement or optical recovery.

The ARM build uses the compatible Legacy Lua Fix tree and private `600D_102.SYM` input supplied outside the repository. Dependency output is counted from the generated `.dep` file and `Will load on: 600D` confirms build-time resolution only.

## SmoothAF v0.8 validation procedure

The v0.8 POC must be validated in this order. The ARM script reports the dependency count from the generated `.dep` file; neither that count nor `Will load on: 600D` proves runtime ABI or camera behavior.

The normal module path contains no direct FIO calls in the AF task or menu callbacks. `-TestOnly` compiles the logger and host-simulated FIO backend in a disposable `/tmp` workdir; this validates ownership, retries and rotation logic only. Production FIO remains consumer-task-owned and is not camera evidence.

1. Run `.\build-smoothaf.ps1 -TestOnly` and preserve the `(SIMULATION)` label on benchmark output.
2. Use host tests to validate mode, arm, episode, 4/6/8/12 command limits, AUTO and recovery reservations, fixed deadline, settle, cooldown, cancellation, failure policy and STOP/PANIC. Host tests do not prove lens movement safety on camera.
3. Build ARM only in an isolated staging copy. Validate `Will load on: 600D`, the generated `DEP` count and ELF ARM/EABI5. For the semaphore diagnosis, preserve only the identified candidate at `artifacts/v0.8/smoothaf.mo`; do not install it automatically. Record the final clean-commit size, hash and Build ID with the build output.
4. Test DRY RUN first in photographic LiveView with recording stopped. Confirm state/request changes and zero lens movement.
5. Test Movie Dry Run separately in normal H.264 recording. Start the monitor before recording, select `Vlog AF Mode=DRY RUN`, manually arm one episode, and use a second phone/camera to record the ML display. Confirm recording start/end events, sample count, validity/confidence, score, duration, blur transitions and zero `AF Lens Calls`; custom and RAW recording must remain blocked. The existing menu is the least-invasive visibility path, but it may not remain open or refresh over the recording UI; treat its values as opportunistic and use the fixed RAM Log Page/Log Entry after the clip as the authoritative fallback.
6. Exercise four short scenes: initially focused static target, nearly focused target, heavily defocused target and a subject approaching/receding. Use short clips first; stop immediately on recording corruption, task stall, abnormal heat, battery/card warning or any unexpected lens command.
7. Movie ACTIVE is now available only as an explicitly authorized experiment: set `Movie Active Experimental=ON`, `Enable=ON`, select one-shot `ACTIVE` or persistent `ACTIVE CONTINUOUS`, start monitoring, and manually arm. Use normal H.264 Movie only; RAW/custom, MF and competing focus controllers remain blocked. Begin with Test A (one command), then bounded Test B, and only then Test C continuous episodes. Do not enable B or C automatically. Do not unload/reload while the shared task is active. Far Focus Recovery remains Movie-disabled unless this same explicit authorization is enabled; any recovery result is experimental and must be recorded separately from controller results.

Test A: H.264 recording, experimental ON, one-shot ACTIVE, episode limit 1, verify at most one STM command and intact clip. Test B: H.264 recording, one-shot ACTIVE, bounded episode search, verify controlled completion. Test C: H.264 recording, ACTIVE CONTINUOUS, one manual arm, multiple episodes, verify cooldown/blur-clear latch and STOP/PANIC interruption.
8. For Movie Active tests record `Movie Recording State`, mode, `AF Move Attempts`, `AF Lens Calls`, `AF Last Block`, `AF Last Move Result`, `AF Commands This Episode`, `AF Episode Result`, scores, delta, direction, cooldown and episode count. `AF Lens Calls=1` proves only API invocation, not physical displacement. Stop immediately on recording corruption, unexpected lens activity, task stall, abnormal heat or battery/card warning.
9. For any existing non-Movie ACTIVE test, record the same AF fields.
10. `AF Lens Calls=1` proves only that `lens_focus()` was invoked. It does not prove physical displacement. `COMPLETE` alone is not success; a zero-call episode must show a no-movement, blocked or cancelled result.

A synchronous `lens_focus(wait=1)` may finish after Stop. The Legacy loader has no general task join/kill guarantee; a task still active after the bounded deinit wait remains an unload hazard.

## SmoothAF v0.8 validation record

- Host tests: passed (`All SmoothAF core tests passed`; `SmoothAF monitor tests passed`; `SmoothAF diagnostics RAM tests passed`).
- Comparative benchmark: passed; scenarios reported `invariants=PASS` and output remained `(SIMULATION)`.
- Menu regression: passed (`SmoothAF menu regression tests passed`).
- Episode telemetry host coverage includes bounded RAM event/summary records, timestamp wraparound, overflow accounting and retained terminal summaries. Physical lifecycle behavior remains pending.
- `git diff --check`: passed; line-ending conversion warnings are non-fatal.
- ARM staging build: passed in a temporary copy of the Legacy Lua Fix tree with `600D_102.SYM`; the source Legacy tree was not modified.
- Historical pre-commit ARM evidence is preserved separately: `Will load on: 600D`, `DEP=36`, `SIZE=98324`, SHA-256 `4940383799aafb6c6bfb8fb9c5b3ea44e90ac61ffe57a6ee52c76b2cd3af5b51`, Build ID `01020cee8635-dirty-600D.102`. It is not a release artifact.
- Historical immutable-commit ARM output: `Will load on: 600D`, `DEP=36`, size `99052`, SHA-256 `2f679ed6f8fa2ca83b9b62c57a5088858f065cd4fb52f919e0bc3ee74bd4537c`, embedded Build ID `5de054e07d96-600D.102`; format is ARM/EABI5 relocatable.
- Previous dirty diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `106072`, SHA-256 `71e9975a01377bd56387ad41775e18ad423bdbb0b89b82c92204a8bdec0a37c1`, embedded Build ID `5d5ef1406810-dirty-600D.102`; it remains historical evidence only and has not been installed.
- Intermediate clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `bf9c11c208db0c17b4026d3344b801a453fa63f2d80fe1431ceec064a524a2c4`, embedded Build ID `8a97f9e36340-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `a1e5d1ab9f3064e491412bdec0f0b4621a1bd4b2bbf7bed0c6607052cc3cc376`, embedded Build ID `49236401addc-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `2eb5c94e1899bf99ec643eabe5190b57b40c3aae306d120d93dbaa3b0d6b2b17`, embedded Build ID `3cb51e83b3a7-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `ddff7d848b4f76a9be710260622546e54fb14c2f6fc5b9294b0e8b95bba1279f`, embedded Build ID `65f9d3d48ec4-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `7c9fb5d5eb4e524d0248719d316d8c7f9f1a63c659489e29f28249c4f0cc9dd0`, embedded Build ID `d69daedd1511-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `fbf0ddd11a90d07f0ceac234d583d9357f2d9ad2687ec52cb9568506243db9b4`, embedded Build ID `dfb1d9957935-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `c886514a601cd813adc7287eb5b6eeb8bff55425cb613a2b12c0ce1edec3f1db`, embedded Build ID `a0c7583df341-600D.102`; it remains historical evidence only.
- Previous clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `7995f2ca6edc286b220494b21a57e81b508cf1c0feb6a49e5bbb1f343228a906`, embedded Build ID `99363514653f-600D.102`; it remains historical evidence only.
- Final clean diagnostic candidate ARM output: `Will load on: 600D`, `DEP=37`, size `107536`, SHA-256 `9271ec593bd725dd8f3265143e6995c13b13864960e5f5b92f438802199fb0dc`, embedded Build ID `90a4edc5bf63-600D.102`; format is ARM/EABI5 relocatable. It has not been installed automatically.

## Phase 3A audit matrix

| ID | Problema | Correção implementada | Teste | Resultado | Risco restante |
|---|---|---|---|---|---|
| P3A-01 | FIO/card I/O no caminho AF | Ring RAM MPSC e consumidor FIO independente com ownership exclusivo de `FILE *` | Logger host, `-Wall -Wextra -Werror`, ARM build | PASS | FIO/cache/DMA real não validado |
| P3A-02 | STOP/PANIC poderia aguardar escrita | STOP/PANIC apenas publicam estado/request; deinit sinaliza o consumidor sem drenar o cartão síncronamente | `stop_panic_non_blocking`, revisão de lifecycle | PASS no host | `lens_focus(wait=1)` e loader Legacy podem permanecer bloqueantes |
| P3A-03 | Corrupção após short write/erro | Batch permanece reservado; retry usa assinatura/offset e não rotaciona linha em andamento | `consumer_failure`, short-write, rotation-boundary retry | PASS | Falhas FIO e remoção física dependem do runtime |
| P3A-04 | Configuração concorrente de FIO | Requests são aplicados pelo consumidor; mudanças durante aplicação deixam `config_pending` para o próximo ciclo | logger FIO host e revisão de ownership | PASS | Não há teste concorrente com preempção DryOS real |
| P3A-05 | Contenção/overflow do ring | Gate bounded try-lock, barreiras ARM9 e contadores de drop/contensão | pthread producer/consumer, overflow, UBSan | PASS | TSan indisponível neste ambiente |
| P3A-06 | Eventos persistentes incompletos/duplicados | Hooks centralizados para sessão, episódio terminal, COMMAND, MEASUREMENT, TIMEOUT, LIMIT, CANCELLED e WRITE_ERROR | eventos específicos, integridade e ordenação host | PASS | Ordenação física só será observável no cartão |
| P3A-07 | Proveniência ARM/DEP inconsistente | Build ID derivado de commit/configuração, `.dep` contado do arquivo gerado e staging descartável | `build-3a.sh`, ARM/EABI5 | PASS pré-commit; repetir pós-commit | Avisos HGVERSION/HGDIFF por ausência de `.hg` no staging |
| P3A-08 | Unload seguro não demonstrável | Deinit observa quiescência e não remove menu se tarefa continuar; documentação explicita NO-GO | revisão Legacy + testes host | PASS como contenção | Loader sem join/kill e tarefa ativa continuam bloqueador físico |

**Recomendação física:** **NO-GO**. Não instalar no SD nem executar validação física até o build pós-commit ser conferido e a quiescência do loader/tarefas ser observada em câmera controlada.
- The staged module is an `ELF 32-bit LSB relocatable, ARM, EABI5 version 1 (SYSV)` `.mo`; undefined symbols are Legacy-resolved imports, including `lens_focus`, `get_ms_clock`, VRAM, task, menu and semaphore APIs. The Legacy build does not emit a separate `.elf`; the `.mo` is the inspected relocatable ELF.
- The embedded Build ID is checked against the final commit. The pre-build v0.8 artifacts remain preserved in the existing untracked `artifacts/v0.8/smoothaf.mo.pre-*` backups; no backup was deleted.
- No physical v0.8 or Movie validation or SD-card installation has been performed. All camera results, including zero lens movement in Dry Run and optical focus/displacement, remain pending. Movie Dry Run must have zero physical lens commands; Movie ACTIVE is not enabled for recording.
- FIO export is disabled by default and host validation covers only the simulated consumer, rotation, short-write retry, card removal/recovery and configuration ownership. No physical SD, cache/DMA or camera-runtime evidence is claimed.

## Semaphore startup diagnosis (2026-11-04)

The Legacy Lua Fix source used by the ARM staging build defines the semaphore ABI in `docs/references/magic-lantern-branch-lua_fix/src/dryos.h`: `create_named_semaphore(const char *, int)` returns `struct semaphore *`; `take_semaphore(struct semaphore *, int)` and `give_semaphore(struct semaphore *)` return `int`. The Legacy self-test at `modules/selftest/selftest.c:1081-1086` verifies a non-null handle and `0` as success for take/give. The 600D.102 stubs and symbol table resolve these functions to `0xFF070E90`, `0xFF070FF0` and `0xFF0710D8`.

The diagnostic candidate instruments the real `smoothaf_init()` -> `smoothaf_lock()` -> `smoothaf_start_task()` path. It records first/last semaphore take and give results and durations, saturating call counters, acquisition latency, current/last hold duration, task-pointer provenance and first/last diagnostic error categories with raw API results, without dereferencing returned objects. The read-only `Diagnostic > Runtime Diagnostics` page is registered before the mandatory semaphore attempt, so semaphore-create failure remains visible in RAM-backed fields; task-creation and runtime semaphore failures are also visible. The production take timeout remains 50 ms; there are no retries, extra diagnostic tasks, FIO calls for telemetry, movement changes or AF algorithm/limit changes.

Host coverage in `tests/test_smoothaf_runtime.c` covers create NULL/non-NULL, invalid handles, take/give success/failure, optional FIO OFF and FIO-task failure, cleanup, STOP/PANIC and no-movement gating. `src/smoothaf_runtime.h` is a host-only lifecycle seam and is not used as a substitute for the ARM DryOS path; host tests cannot prove camera object validity or runtime ABI. The seam is therefore evidence for lifecycle expectations only, not a reproduction of `src/smoothaf.c` or DryOS.

**Current conclusion: root cause remains unconfirmed; physical status remains NO-GO.** The single diagnostic ARM candidate was built from the dirty working tree and is not a replacement for the historical `eb32bdb36824-600D.102` artifact. No SD-card installation, release, push, merge, rebase, reset or tag operation was performed.

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
