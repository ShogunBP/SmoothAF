# SmoothAF Roadmap

These are future objectives and hypotheses, not implemented features.

- Calibrate focus-step behavior across additional tested lenses.
- Improve bounded recovery when the starting focus position is far from the target without unbounded scanning.
- Validate additional Legacy camera ports only when matching symbol tables and physical hardware are available.
- Add safe integration tests at a reproducible Legacy seam.
- Improve release packaging while preserving manual installation and rollback.
- Investigate Canon AF Trigger, Repeat and Focus Monitor only through the staged, manual, out-of-recording PoCs in [Research](RESEARCH.md).
- Consider Vlog AF only after bounded movement, hysteresis, cooldown and exposure behavior are validated.

Continuous AF, Movie AF and recording-time movement are not roadmap commitments for the current release. Any future work must preserve the existing gates, PANIC behavior, explicit Rearm, single-task ownership and bounded budgets.
