# FastAF 0.1 Baseline

## Purpose

This document records the FastAF 0.1 baseline preserved before the SmoothAF organization work. The focus algorithm was not changed as part of the migration.

## Validated binary

- Historical module: `fastaf.mo`
- Baseline SHA-256: `123eec5c255d47a01927a719a3905135589a0916c199056860c032d111e2a113`
- Size: 14468 bytes
- Format: ARM EABI5 ELF32 relocatable module
- Target: Canon EOS 600D / Rebel T3i, firmware 1.0.2
- Magic Lantern: Legacy Lua Fix
- Lens: Canon EF 50mm f/1.8 STM

The preserved binary is stored under `artifacts/baseline/`. It is retained as historical evidence and is not a substitute for testing the renamed `smoothaf.mo` artifact.

## Physical observations supplied for this baseline

- Step Size 3 found focus and stopped automatically.
- Step Sizes 1 and 2 did not work in the recorded tests with this lens.
- Search may fail when the lens starts very far out of focus.
- The `MENU_EOL` registration ASSERT was corrected.

These observations are specific to the stated camera, firmware, Magic Lantern build, lens, and test conditions. They are not general compatibility claims.

## Safety properties

- `Enable` starts off.
- Loading the module does not move the lens.
- `PANIC / Emergency Stop` disables future operations.
- Rearm is explicit and leaves `Enable` off.
- Movement is bounded; continuous AF is not implemented.

## Migration boundary

The SmoothAF repository reorganizes and renames the module. It does not claim that a renamed module is physically validated merely because the preserved FastAF binary was tested. A new `smoothaf.mo` requires an independent controlled camera test.
