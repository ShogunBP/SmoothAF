# Build Validation Status

The preserved FastAF baseline is physically reported as working on the stated EOS 600D/T3i setup with Step Size 3. The SmoothAF rename has not yet been physically validated.

The standalone repository contains the renamed source, host tests, menu regression, Docker inputs, and a PowerShell integration build. The full build was executed against the compatible Legacy Lua Fix tree and the private `600D_102.SYM` supplied through the ignored local `docs/references/` directory. `-LegacyRoot` and `-SymbolSource` remain available for external trees and override automatic detection.

The generated artifact is `artifacts/consolidated/smoothaf.mo`:

- Size: 12420 bytes.
- SHA-256: `722291457d3cb507b245353aab47b336dc0b0d9f9f7d6d819ec725f38c16c85a`.
- Dependency check: 23 dependencies; `Will load on: 600D`.
- Host core tests, comparative benchmark, and menu regression passed.
- Release: `artifacts/releases/smoothaf-722291457d3cb507b245353aab47b336dc0b0d9f9f7d6d819ec725f38c16c85a`.

The v0.2.0 physical observations supplied for this release used an EOS 600D / Rebel T3i, firmware 1.0.2, Magic Lantern Legacy Lua Fix, EF 50mm f/1.8 STM, Step Size 3 and Laplacian. They were not a controlled same-scene comparison:

- Legacy HC: `Search Limit`, score 2677, best 2877, mean 74, 1381 ms, 3 commands, 1 reversal.
- Adaptive HC: `Search Limit`, score 3059, best 3087, mean 55, 2374 ms, 4 commands, 3 reversals.

The reported observations confirm module loading, working menus and micro-adjustments. Large focus-distance changes were not recovered reliably, Debug Logging had no complete on-camera history viewer, and Step Sizes 1 and 2 did not work in the recorded tests. These observations do not establish Adaptive HC superiority or compatibility beyond the stated setup.

The build and host tests do not replace controlled hardware validation. Do not claim broad camera compatibility or continuous AF; continuous AF is not implemented.
