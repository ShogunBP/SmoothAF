# FastAF to SmoothAF Migration

The migration renames the module source and internal symbols while preserving the focus algorithm, Safe Mode, PANIC behavior, task limits, and menu regression fix.

The historical FastAF binary remains under `artifacts/baseline/` with SHA-256:

```text
123eec5c255d47a01927a719a3905135589a0916c199056860c032d111e2a113
```

A renamed module must be built against a compatible Legacy Lua Fix tree and tested independently. The historical binary and the new module are not interchangeable evidence of one another.
