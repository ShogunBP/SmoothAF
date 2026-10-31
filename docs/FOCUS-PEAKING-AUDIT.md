# Focus Peaking Audit

## Scope

The audit used the local Magic Lantern Legacy Lua Fix reference tree. The reference tree and `600D_102.SYM` remain ignored and are not part of SmoothAF commits or releases.

## Findings

The Legacy implementation is in `src/zebra.c`:

- `calc_peak()` and `peak_d2xy()` use a five-point second-derivative kernel: `4 * center - up - down - left - right`.
- `focus_peaking_filter_edges` optionally reduces strong first-derivative edges, which are treated as possible false positives.
- `peak_disp_filter()` and `focus_found_pixel()` consume the result to modify or draw display overlays.
- Focus Peaking configuration includes a percentage threshold and display/color options.

The implementation is coupled to Magic Lantern display-filter and overlay paths. It does not expose a small, stable, public metrics-only API that SmoothAF can safely call from its module task. The number of colored overlay points is therefore not treated as a focus-distance or lens-position metric.

## SmoothAF decision for v0.3

Normal AF continues to use the independent integer-only SAD/Laplacian metrics over the bounded central 64×64 UYVY ROI. No overlay routine is called, no private Focus Peaking symbol is imported, and no `Peaking Experimental` menu option is enabled in this iteration.

The existing Laplacian metric follows the same broad second-derivative principle, but it remains independently implemented and testable on the host. A future Peaking Experimental metric may be considered only after a separate cost, overflow, noise and camera validation review.

## Constraints

This audit does not establish physical focus performance or ABI compatibility. It does establish that reusing the overlay renderer as a Normal AF metric would couple the module to unsupported display internals and could add unnecessary DIGIC IV cost.
