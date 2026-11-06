#!/usr/bin/env python3
"""Compare bytes emitted by the C OBS serializer with the Python contract."""
from __future__ import annotations

import importlib.util
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o32_serialization.py"
SPEC = importlib.util.spec_from_file_location("b2o32_wire", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)
O31 = MODULE.O31


def python_bytes() -> bytes:
    snapshot = O31.ObservationSnapshot.from_values(
        timestamp_ms=1000,
        yuv_buffer_address=0x1000,
        width=720,
        pitch=1440,
        height=480,
        liveview=True,
        movie_mode=True,
        recording_class=O31.RecordingClass.H264,
        edmac_write_address=None,
        display_buffer_address=None,
        exposure=None,
        mvr_frame_number=None,
    )
    event = MODULE.observation_event_from_snapshot(
        snapshot,
        phase=MODULE.SnapshotPhase.BEFORE,
        episode_id=3,
        sequence=7,
        logical_id=1,
        status=O31.Status.STABLE_METADATA,
        profile=O31.Profile.CORE,
    )
    return MODULE.serialize_record(event.to_record())


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: check_smoothaf_observability_wire.py EXECUTABLE")
    result = subprocess.run(
        [sys.argv[1]], capture_output=True, text=True, check=False
    )
    stdout = result.stdout.strip()
    if result.returncode or result.stderr or not stdout:
        raise AssertionError(
            f"C harness failed: rc={result.returncode} stdout={result.stdout!r} "
            f"stderr={result.stderr!r}"
        )
    if len(stdout) != MODULE.RECORD_SIZE * 2 or any(
        char not in "0123456789abcdefABCDEF" for char in stdout
    ):
        raise AssertionError(f"invalid C hex output: {stdout!r}")
    c_bytes = bytes.fromhex(stdout)
    expected = python_bytes()
    if c_bytes != expected:
        raise AssertionError(
            f"C/Python mismatch:\nC      {c_bytes.hex()}\nPython {expected.hex()}"
        )
    print("C/Python observability wire equivalence passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
