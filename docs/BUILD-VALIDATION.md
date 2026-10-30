# Build Validation Status

The preserved FastAF baseline is physically reported as working on the stated EOS 600D/T3i setup with Step Size 3. The SmoothAF rename has not yet been physically validated.

The standalone repository contains the renamed source, host tests, menu regression, Docker inputs, and a PowerShell integration scaffold. A full `smoothaf.mo` build requires a compatible Legacy Lua Fix tree and the private `600D_102.SYM` supplied through `-LegacyRoot` and `-SymbolSource`.

Do not report the renamed module as camera-validated until that build and a controlled hardware test pass.
