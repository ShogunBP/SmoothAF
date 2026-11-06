#!/usr/bin/env python3
"""Parse lines emitted by the real C logger formatter with the TSV parser."""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o33e_tsv.py"
SPEC = importlib.util.spec_from_file_location("b2o33e_c_formatter_check", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_smoothaf_logger_tsv.py TSV_FILE")
    data = Path(sys.argv[1]).read_bytes()
    result = MODULE.parse_tsv(data)
    if result.errors:
        raise AssertionError(f"C formatter output was rejected: {result.errors}")
    if len(result.records) != 9:
        raise AssertionError(f"expected 9 C records, got {len(result.records)}")

    expected = [
        ("MEASUREMENT", 7, (-2147483648, -1, 0, 1, 2147483647, -7)),
        ("D1_SERIES_START", 8, (4294967295, 4294967294, 4294967293,
                                -4, -5, -6)),
        ("D1_SAMPLE", 8, (4294967295, -2, 123, -456, 0, 1)),
        ("D1_SERIES_END", 8, (4294967295, 4294967294, 4294967293,
                              4294967292, 4294967291, 4294967290)),
        ("D1_CONTEXT", 8, (4294967295, 4294967294, 4294967293,
                             4294967292, 4294967291, 4294967290)),
        ("D1_CONTEXT_AUX", 8, (4294967295, 4294967294, 4294967293,
                                 4294967292, 4294967291, 4294967290)),
        ("MOVIE_GATE_REJECT", 0, (4294967295, 4294967294, 4294967293,
                                   4294967292, 4294967291, 4294967290)),
        ("OBS_SNAPSHOT_V1", 9, (0x00010301, 0x1000, -1, -1, -1, -1)),
        ("UNKNOWN", 10, (-2147483648, -1, 0, 1, 2147483647, -7)),
    ]
    for record, (event, episode, payload) in zip(result.records, expected):
        if (record.event_name, record.episode_id, record.payload) != (
            event, episode, payload
        ):
            raise AssertionError(
                f"C/Python TSV mismatch: got {(record.event_name, record.episode_id, record.payload)!r}, "
                f"expected {(event, episode, payload)!r}"
            )
        if record.sequence is not None:
            raise AssertionError("TSV parser reconstructed sequence")
    if len(result.snapshots) != 1:
        raise AssertionError("OBS_SNAPSHOT_V1 was not decoded from C TSV")
    if result.snapshots[0].logical_id != 1:
        raise AssertionError("decoded OBS logical_id mismatch")
    print("C formatter TSV -> Python parser equivalence passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
