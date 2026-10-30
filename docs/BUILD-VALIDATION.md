# Build Validation Status

The preserved FastAF baseline is physically reported as working on the stated EOS 600D/T3i setup with Step Size 3. The SmoothAF rename has not yet been physically validated.

The standalone repository contains the renamed source, host tests, menu regression, Docker inputs, and a PowerShell integration build. The full build was executed against the compatible Legacy Lua Fix tree and the private `600D_102.SYM` supplied through `-LegacyRoot` and `-SymbolSource`.

The generated artifact is `artifacts/consolidated/smoothaf.mo`:

- Size: 10468 bytes.
- SHA-256: `472416dc2d8a6ac37c1c66bf8d506f61a78ce6064c08870652700498aa0b5fbb`.
- Dependency check: 23 dependencies; `Will load on: 600D`.
- Host core tests and menu regression passed.

The renamed module has not been physically validated on a camera. Do not report it as camera-validated until a controlled hardware test passes.
