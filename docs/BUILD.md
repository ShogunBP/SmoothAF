# SmoothAF Build

The repository keeps the source and tests independent from the full Magic Lantern tree. A compatible Legacy Lua Fix checkout is required to compile the module because the module headers, Makefile integration, and camera-specific symbol resolution belong to that tree.

Set the private inputs in PowerShell:

```powershell
$env:SMOOTHAF_LEGACY_ROOT = 'D:\path\to\magic-lantern-legacy-lua-fix'
$env:SMOOTHAF_SYMBOL_SOURCE = 'D:\private\600D_102.SYM'
```

Run:

```powershell
.\build-smoothaf.ps1
.\build-smoothaf.ps1 -TestOnly
.\build-smoothaf.ps1 -Clean
.\build-smoothaf.ps1 -Release
```

The build refuses to run without an explicit Legacy tree and symbol-table path. It does not store the private symbol table in the repository and does not modify the source tree permanently.

The current migration preserves the FastAF binary baseline under `artifacts/baseline/`. A renamed `smoothaf.mo` must be rebuilt and physically tested independently.
