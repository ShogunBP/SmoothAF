# SmoothAF v0.8.0-beta.1 Installation

SmoothAF v0.8.0-beta.1 is an **experimental pre-release** for a narrowly validated setup. It is not a replacement for Magic Lantern, its loader, firmware, or camera backups.

## Supported validation target

The release is intended only for the following target:

- Canon EOS 600D / Rebel T3i;
- Canon firmware 1.0.2;
- Magic Lantern Legacy Lua Fix;
- the supplied release module `smoothaf.mo`.

No compatibility claim is made for another camera, firmware version, Magic Lantern branch, lens, or loader revision.

## Before installation

1. Make and verify a complete SD-card backup.
2. Confirm that the camera, firmware and Legacy Lua Fix revision match the target above.
3. Download `smoothaf.mo` from the GitHub Release attachment, not from a repository build directory.
4. Verify the downloaded file before copying it to the card:

```powershell
Get-FileHash .\smoothaf.mo -Algorithm SHA256
```

The v0.8.0-beta.1 candidate must report:

```text
SHA256  494aa507f44dccf8e7403b0106cf7c6bfa8b91f5c0f36a13720c2d9708f28a4c
SIZE    131444 bytes
BUILD   31912f4d2e08-600D.102
```

Do not install a file with a different hash or Build ID.

## Manual installation

With the camera powered off, copy the verified file to:

```text
ML/MODULES/smoothaf.mo
```

The repository and build scripts never write to an SD card automatically. Do not rename the file to another module name, replace `AUTOEXEC.BIN`, replace firmware, or modify the loader.

Boot the camera and confirm that the module appears in the Magic Lantern module list. Confirm the displayed Build ID when the runtime diagnostics page is available. Loading must not move the lens. Keep `Enable=OFF` until the initial checks are complete.

## First checks and safe operation

1. Start in photographic LiveView with recording stopped.
2. Confirm that no lens movement occurs during module loading.
3. Confirm `Enable=OFF`.
4. Use `Diagnostic > Measure` for a read-only measurement.
5. Review `Status`, including the block reason and task state.
6. Only then perform one controlled `Diagnostic > Focus Step`, if authorized for the test setup.
7. Use `Focus Test` only after the single-step path is understood.
8. Do not unload, reload or replace the module while a task is active.
9. Treat `lens_focus()` success as API acceptance only; it does not prove physical displacement.

`ACTIVE` and `ACTIVE CONTINUOUS` are experimental paths. They are not stable autofocus modes, are not guaranteed to avoid hunting, and must not be treated as validated normal-use features. Movie movement additionally requires the explicit experimental authorization and is restricted by the source gates to normal H.264 conditions; RAW/custom recording and competing focus controllers remain blocked.

## STOP/PANIC and recovery

Use the module's `Stop` control when the task is responsive. STOP is cooperative; a synchronous Legacy lens call already in flight may finish. PANIC/safety gates must remain authoritative and no further episode should be armed after a safety stop.

Immediately stop the test and power the camera off if there is unexpected lens activity, a task stall, recording corruption, abnormal heat, a battery/card warning, or any state mismatch. After powering off, remove `smoothaf.mo` from the card with a computer or card reader and boot without it. Restore the SD-card backup if the card state is uncertain. Do not remove or reload the module while its task is active.

## Reproducing the build

Host tests and the ARM build require Docker Desktop, a compatible Legacy Lua Fix checkout and the private camera-specific `600D_102.SYM` input. These inputs are not distributed in the repository or release. See [Development](docs/DEVELOPMENT.md) for acquisition boundaries, path configuration and staging behavior.

The build uses the source commit and target configuration to derive the Build ID. A reproducible build must be independently verified against the release manifest before any camera installation.
