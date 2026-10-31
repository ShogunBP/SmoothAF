# Changelog

## SmoothAF v0.2.0 — Experimental

- Preserved Legacy HC as the default bounded focus controller.
- Added experimental Adaptive HC as an explicit selectable mode.
- Added configurable fixed-size debug logging with a 32-entry circular buffer.
- Added deterministic host tests and a Legacy HC versus Adaptive HC benchmark.
- Added automatic local detection of the ignored Legacy Lua Fix tree and private `600D_102.SYM`, while retaining external path overrides.
- Validated the ARM build with 23 dependencies for `600D`.

### Physical test record

The reported physical observations used an EOS 600D / Rebel T3i with firmware 1.0.2, Magic Lantern Legacy Lua Fix, EF 50mm f/1.8 STM, Step Size 3 and Laplacian. They were not a controlled same-scene comparison:

- Legacy HC: `Search Limit`, score 2677, best 2877, mean 74, 1381 ms, 3 commands, 1 reversal.
- Adaptive HC: `Search Limit`, score 3059, best 3087, mean 55, 2374 ms, 4 commands, 3 reversals.

The module loaded, menus worked and micro-adjustments were observed. Large focus-distance changes were not recovered reliably. Debug Logging had no complete on-camera history viewer. Step Sizes 1 and 2 did not work in the recorded EF 50mm f/1.8 STM tests.

This release is experimental. It does not implement continuous AF and does not claim compatibility with other cameras, firmware, lenses or Legacy revisions. Host benchmarks are simulations and do not prove physical focus performance.

## Historical baseline

FastAF 0.1 artifacts and validation records remain unchanged.
