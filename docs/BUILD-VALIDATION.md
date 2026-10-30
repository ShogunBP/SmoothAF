# Build Validation Status

The preserved FastAF baseline is physically reported as working on the stated EOS 600D/T3i setup with Step Size 3. The SmoothAF rename has not yet been physically validated.

The standalone repository contains the renamed source, host tests, menu regression, Docker inputs, and a PowerShell integration build. The full build was executed against the compatible Legacy Lua Fix tree and the private `600D_102.SYM` supplied through the ignored local `docs/references/` directory. `-LegacyRoot` and `-SymbolSource` remain available for external trees and override automatic detection.

The generated artifact is `artifacts/consolidated/smoothaf.mo`:

- Size: 12420 bytes.
- SHA-256: `722291457d3cb507b245353aab47b336dc0b0d9f9f7d6d819ec725f38c16c85a`.
- Dependency check: 23 dependencies; `Will load on: 600D`.
- Host core tests, comparative benchmark, and menu regression passed.
- Release: `artifacts/releases/smoothaf-722291457d3cb507b245353aab47b336dc0b0d9f9f7d6d819ec725f38c16c85a`.

The renamed module has not been physically validated on a camera. Do not report it as camera-validated until a controlled hardware test passes.
