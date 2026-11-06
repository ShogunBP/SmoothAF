# SmoothAF v0.8.0-beta.1 Known Issues

This release is **experimental** and is limited to the validated target: Canon EOS 600D / Rebel T3i, firmware 1.0.2, Magic Lantern Legacy Lua Fix, and the release candidate identified in the release manifest.

## Compatibility and evidence limits

- No other camera, firmware, Magic Lantern branch, loader revision or lens is validated.
- Host tests prove deterministic software contracts only; they do not prove camera ABI compatibility, optical focus quality or safe physical displacement.
- ARM compilation and `Will load on: 600D` prove build-time dependency resolution only.
- `lens_focus()` success proves an API result, not physical lens movement.
- The recorded physical observations used a Canon EF 50mm f/1.8 STM and Step Size 3. They were not a controlled comparison and do not generalize to other lenses.

## Focus behavior

- The search may fail when the lens starts far from the target.
- Step Sizes 1 and 2 did not work in the recorded EF 50mm f/1.8 STM tests; this is an observation, not a universal lens rule.
- Hunting, environmental changes, low texture, blur, invalid measurements and lighting changes can prevent convergence or pause the controller.
- `STABLE` is not a claim of perfect focus.
- Recovery and Adaptive HC remain experimental and bounded; they are not guarantees of optical recovery.

## Movie and continuous modes

- Continuous AF is not a stable, generally validated feature.
- `ACTIVE CONTINUOUS` is explicitly experimental and is not a normal-use or release-stability claim. It may hunt, stop, pause, reject an episode or exhaust its bounded acquisition budget.
- Movie Active requires explicit experimental authorization and is restricted to normal H.264 recording by source gates. RAW/custom recording and competing focus controllers remain blocked.
- Movie Dry Run is read-only and does not call the lens adapter.
- Stop is cooperative. A synchronous `lens_focus(wait=1)` call may finish after STOP.
- The Legacy loader has no general task join/kill guarantee. Unloading or reloading while a task is active is unsafe.

## Logging and diagnostics

- Persistent logging/FIO is disabled by default and has not been physically validated on an SD card.
- RAM telemetry and host logger tests do not establish physical card, cache, DMA or camera-runtime behavior.
- Diagnostic and benchmark output must be interpreted with its evidence class; `(SIMULATION)` output is not camera evidence.

## Release status

- This is a pre-release / experimental build, not a stable release.
- No automatic SD-card installation is provided.
- Do not use this module to replace firmware, `AUTOEXEC.BIN` or the Magic Lantern loader.
- Report the exact camera, firmware, Legacy revision, lens, Step Size, starting-focus condition, Build ID, SHA-256 and diagnostic status when reporting an issue.
