# SmoothAF Roadmap

These are future objectives and hypotheses, not implemented features.

- Calibrate focus-step behavior across additional tested lenses.
- Improve bounded recovery when the starting focus position is far from the target without unbounded scanning.
- Validate additional Legacy camera ports only when matching symbol tables and physical hardware are available.
- Add safe integration tests at a reproducible Legacy seam.
- Improve release packaging while preserving manual installation and rollback.
- Before `v0.8.0-beta.1`, perform repository hygiene: audit tracked/untracked/ignored content, keep source/tests/reproducible tools/public documentation, exclude local logs, agent configuration, experimental artifacts and private build inputs, and publish the verified `smoothaf.mo` only as a GitHub Release attachment.
- Keep the v0.4 Canon AF Assist adapter unavailable until a verified C API, ABI, symbol resolution, cancellation contract and controlled hardware evidence exist; then investigate Trigger, Repeat and Focus Monitor only through staged, manual, out-of-recording PoCs in [Research](RESEARCH.md).
- Consider an asynchronous Vlog Monitor only after the passive v0.5 viewer is validated. A future task must measure periodically without any automatic lens movement and must first establish DIGIC IV cost, interval, smoothing, hysteresis and invalid-frame handling.

Continuous AF, Movie AF and recording-time movement remain disabled for the current release. The current investigation adds only a read-only H.264 Movie Dry Run probe; any future Movie Active work must preserve the existing gates, cooperative Stop, single-task ownership and bounded budgets.
