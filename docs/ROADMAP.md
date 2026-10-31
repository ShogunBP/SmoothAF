# Roadmap

These are future objectives, not implemented features.

- Develop SmoothAF 0.3 Normal AF as a bounded single-shot acquisition and refinement mode.
- Audit Magic Lantern Focus Peaking and keep it separate from overlay rendering; do not assume yellow overlay points are a focus-distance metric.
- Add an on-camera paginated viewer for the fixed diagnostic history without continuous allocation or card writes.
- Calibrate focus step behavior across additional tested lenses.
- Improve recovery when the starting focus position is far from the target without unbounded scanning.
- Validate more Legacy camera ports only when real symbol tables and physical hardware are available.
- Improve release packaging while preserving manual installation and rollback.
- Add integration tests where a safe, reproducible Legacy seam exists.
