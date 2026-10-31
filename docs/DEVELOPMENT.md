# SmoothAF Development

## Repository boundary

SmoothAF is an independently organized module source distribution, not a complete Magic Lantern tree. A compatible Legacy Lua Fix checkout is required for the ARM build. The private camera symbol table is supplied separately and must remain outside Git, releases and SD-card packages.

Do not modify `AUTOEXEC.BIN`, firmware, the loader or an SD card as part of development. Do not unload or reload the module while a task is active.

## Private build inputs

PowerShell variables may point to local copies of the required inputs:

```powershell
$env:SMOOTHAF_LEGACY_ROOT = 'D:\path\to\magic-lantern-legacy-lua-fix'
$env:SMOOTHAF_SYMBOL_SOURCE = 'D:\private\600D_102.SYM'
```

The repository build script also detects the ignored local `docs/references/` tree when available. The symbol table is transformed only into a temporary derived build input. `docs/references/` and `600D_102.SYM` must not be committed or copied into releases.

Required integration inputs are:

- compatible Legacy Lua Fix source tree;
- camera-specific symbol source for the target firmware;
- `src/` module source and core headers;
- generated `module_strings.h`.

Never substitute a symbol table from another camera or firmware.

## Build and test commands

From the repository root, with Docker Desktop running:

```powershell
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1 -Clean
.\build-smoothaf.ps1 -Release
```

The full build creates or uses the `smoothaf-legacy-build:bookworm` image, runs host tests, copies the project into a temporary Legacy tree, generates module strings, builds the ARM module with `make clean all` (never `nodep`), checks dependencies and computes SHA-256. `-TestOnly` runs local checks without ARM integration; `-Release` writes a hash-named local artifact.

Host benchmark output is deterministic controller simulation. It must remain labeled `(SIMULATION)` and must not be presented as physical focus evidence.

## Legacy integration boundary

The module depends on Legacy interfaces for menu registration, LiveView VRAM, task creation, semaphores, timing, focus state and `lens_focus()`. The generated symbol file must match the target camera and firmware. Build success and `Will load on: 600D` do not prove runtime ABI compatibility or physical lens behavior.

`integration/README.rst` remains the short integration document used by the Legacy build. The former Markdown integration guide is consolidated here.

## Development safety rules

- Keep `Enable=0` at initialization.
- Preserve PANIC, explicit Rearm and bounded command/reversal budgets.
- Keep Movie Mode, recording, Continuous AF, MF, Follow Focus, Rack Focus and Focus Stacking blocked.
- Do not add continuous card logging; use the fixed in-memory diagnostic history.
- Do not treat `lens_focus()` success as proof of physical movement.
- Preserve the separation between host simulation, ARM build validation and controlled camera testing.
- Keep private symbols, firmware, loader files, SD-card backups and personal logs out of commits.

## Migration note

The FastAF-to-SmoothAF migration renamed the module and internal symbols while preserving the focus algorithm, Safe Mode, PANIC behavior, task limits and the `MENU_EOL` fix. The historical `fastaf.mo` is not interchangeable evidence for a newly built `smoothaf.mo`; each renamed artifact requires independent validation.
