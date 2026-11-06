# SmoothAF v0.8.0-beta.1 — Experimental pre-release

SmoothAF v0.8.0-beta.1 is an experimental pre-release. It is not a stable autofocus release and is not a replacement for Magic Lantern, its loader, camera firmware, or an SD-card backup.

## Validated target

This release is intended only for the following target:

- Canon EOS 600D / Rebel T3i;
- Canon firmware 1.0.2;
- Magic Lantern Legacy Lua Fix;
- a compatible lens, with physical observations recorded using an EF 50mm f/1.8 STM.

No compatibility claim is made for another camera, firmware version, Magic Lantern branch, loader revision or lens.

## Release artifact

Download the attached `smoothaf.mo` and verify it before installation:

```text
Build ID: 31912f4d2e08-600D.102
SHA-256:  494aa507f44dccf8e7403b0106cf7c6bfa8b91f5c0f36a13720c2d9708f28a4c
Size:     131444 bytes
```

This is the only module authorized for this release. The binary was built from source commit `31912f4d2e089b320b4664eac92ae665e4521f17`. The public repository hygiene commit is `90a21851624a1474195607d49e44ad49aa8235a4`; the binary Build ID and the future release-tag commit identify different snapshots and must not be conflated.

## Installation

1. Make and verify a complete SD-card backup.
2. Confirm the camera, firmware and Legacy Lua Fix target match the compatibility section.
3. Verify the downloaded file's SHA-256 and size.
4. With the camera powered off, copy it to `ML/MODULES/smoothaf.mo`.
5. Boot and confirm that the module appears in the module list without lens movement during loading.
6. Leave `Enable=OFF` for the initial checks. Use `Diagnostic > Measure` before any controlled focus command.

See [`INSTALL.md`](../INSTALL.md) for the complete manual installation, backup, checksum, recovery and STOP/PANIC procedure.

## Experimental behavior and limitations

- `ACTIVE` and `ACTIVE CONTINUOUS` are experimental bounded paths. They are not stable autofocus modes and are not guaranteed to avoid hunting.
- Movie Active requires explicit experimental authorization and is restricted by the source gates to normal H.264 recording. RAW/custom recording and competing focus controllers remain blocked.
- Movie Dry Run is read-only and does not call the lens adapter.
- Searches may fail with large initial defocus, low texture, blur, lighting changes, invalid measurements or scene changes.
- `lens_focus()` success proves API acceptance only; it does not prove physical lens displacement.
- STOP is cooperative; a synchronous Legacy lens call already in flight may finish. Do not unload or reload while a task is active.
- Host tests and ARM dependency resolution are not physical camera evidence. Recorded physical observations do not generalize to other cameras, firmware, Legacy revisions or lenses.

If there is unexpected lens activity, a task stall, recording corruption, abnormal heat, a battery/card warning or any state mismatch, stop immediately and power off the camera. Remove the module with a computer or card reader and restore the SD-card backup if necessary.

## Verification performed

The host suite passed, including core, algorithm, monitor, diagnostics, logger, lifecycle, production-seam, menu-regression and simulation checks. The preserved artifact matches the identity above. The ARM build evidence reports `DEP=39` and `Will load on: 600D`; these are build-time checks, not a claim of unrestricted runtime compatibility.

This release does not include logs, historical builds, private symbols, the Legacy Lua Fix dependency tree, SD-card backups, credentials or the unreviewed B2-B development files.
