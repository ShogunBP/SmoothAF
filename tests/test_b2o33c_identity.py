#!/usr/bin/env python3
"""Host-only tests for the B2-O3.3c identity prototype."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o33c_identity.py"
SPEC = importlib.util.spec_from_file_location("b2o33c_identity", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = MODULE.BASE
O31 = BASE.O31


class B2O33CIdentityTests(unittest.TestCase):
    def snapshot(self, timestamp: int = 1000):
        return O31.ObservationSnapshot.from_values(
            timestamp_ms=timestamp,
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

    def event(self, phase, *, episode=3, logical=1, timestamp=1000):
        return BASE.observation_event_from_snapshot(
            self.snapshot(timestamp), phase=phase, episode_id=episode,
            sequence=7, logical_id=logical, status=O31.Status.STABLE_METADATA,
            profile=O31.Profile.CORE,
        )

    def test_session_marker_is_additive_36_byte_record(self):
        token = bytes.fromhex("00112233445566778899aabbccddeeff")
        encoded = MODULE.encode_session_start(token, timestamp_ms=10, sequence=4)
        self.assertEqual(len(encoded), BASE.RECORD_SIZE)
        record = BASE.deserialize_record(encoded)
        self.assertEqual(MODULE.decode_session_start(record), token)
        self.assertEqual(record.event_type, MODULE.SESSION_START_EVENT)
        self.assertEqual(record.episode_id, 0)

    def test_generator_requires_injected_entropy_and_is_not_counter(self):
        first = bytes.fromhex("10000000000000000000000000000001")
        second = bytes.fromhex("ffffffff000000000000000000000001")
        generator = MODULE.SimulatedSessionIdGenerator([first, second])
        self.assertEqual(generator.new(), first)
        self.assertEqual(generator.new(), second)
        with self.assertRaises(MODULE.IdentityError):
            generator.new()

    def test_marker_pairs_only_inside_verified_session(self):
        token = bytes.fromhex("00112233445566778899aabbccddeeff")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        unverified = pairer.finish()
        self.assertEqual(len(unverified.complete), 0)
        self.assertEqual(unverified.incomplete[0].status,
                         MODULE.IdentityPairStatus.IDENTITY_UNAVAILABLE)

        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token, timestamp_ms=1, sequence=1))
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 1)
        self.assertTrue(report.complete[0].identity_verified)
        self.assertFalse(report.complete[0].frame_independence_proven)

    def test_marker_loss_is_fail_closed(self):
        token = bytes.fromhex("00112233445566778899aabbccddeeff")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertEqual(len(report.incomplete), 2)
        self.assertTrue(all(item.status == MODULE.IdentityPairStatus.IDENTITY_UNAVAILABLE
                            for item in report.incomplete))

        # An external sidecar may safely restore the exact token without
        # inferring it from timestamp or wire IDs.
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a", session_token=token)
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        self.assertEqual(len(pairer.finish().complete), 1)

    def test_concatenation_and_reopen_are_not_cross_paired(self):
        token_a = bytes.fromhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        token_b = bytes.fromhex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token_a, timestamp_ms=1, sequence=1))
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.end_source()
        pairer.begin_source("file-b")
        pairer.add(MODULE.session_start_record(token_b, timestamp_ms=2, sequence=2))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertEqual({item.status for item in report.incomplete}, {
            MODULE.IdentityPairStatus.AFTER_MISSING,
            MODULE.IdentityPairStatus.BEFORE_MISSING,
        })

        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token_a, timestamp_ms=1, sequence=1))
        pairer.end_source()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token_a, timestamp_ms=2, sequence=2))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertTrue(any("AMBIGUOUS_PAIR" in msg for msg in report.invalid))

    def test_partial_marker_record_is_not_identity_evidence(self):
        token = bytes.fromhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        malformed = MODULE.session_start_record(token, timestamp_ms=1, sequence=1)
        malformed = BASE.LogRecord(
            malformed.timestamp_ms, malformed.event_type, malformed.episode_id,
            malformed.sequence, malformed.payload[:-1] + (1,)
        )
        pairer.add(malformed)
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertTrue(any("invalid OBS_SESSION_START" in item for item in report.invalid))

    def test_same_token_in_distinct_sources_never_cross_pairs(self):
        token = bytes.fromhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("segment-a", session_token=token)
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.end_source()
        pairer.begin_source("segment-b", session_token=token)
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertEqual({item.status for item in report.incomplete}, {
            MODULE.IdentityPairStatus.AFTER_MISSING,
            MODULE.IdentityPairStatus.BEFORE_MISSING,
        })

    def test_duplicate_marker_poisoning_is_fail_closed(self):
        token = bytes.fromhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        marker = MODULE.session_start_record(token, timestamp_ms=1, sequence=1)
        pairer.add(marker)
        pairer.add(marker)
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertTrue(any(item.status == MODULE.IdentityPairStatus.AMBIGUOUS_PAIR
                            for item in report.incomplete))
        self.assertTrue(all(not item.stable_metadata for item in report.incomplete))

    def test_contradictory_duplicate_and_wrap_evidence_is_ambiguous(self):
        token_a = bytes.fromhex("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
        token_b = bytes.fromhex("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a", session_token=token_a)
        pairer.add(MODULE.session_start_record(token_b, timestamp_ms=1, sequence=0xFFFFFFFF))
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertTrue(any("AMBIGUOUS_PAIR" in msg for msg in report.invalid))

        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token_a, timestamp_ms=1, sequence=0xFFFFFFFF))
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE))
        pairer.add(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        pairer.add(self.event(BASE.SnapshotPhase.BEFORE, timestamp=2000))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 1)
        self.assertEqual(report.incomplete[-1].status, MODULE.IdentityPairStatus.AMBIGUOUS_PAIR)
        self.assertFalse(report.incomplete[-1].stable_metadata)

    def test_v1_historical_records_remain_opaque(self):
        token = bytes.fromhex("00112233445566778899aabbccddeeff")
        pairer = MODULE.SessionPairer()
        pairer.begin_source("file-a")
        pairer.add(MODULE.session_start_record(token, timestamp_ms=1, sequence=1))
        historical = BASE.LogRecord(2, 8, 3, 2, (1, 2, 3, 4, 5, 6))
        pairer.add(historical)
        report = pairer.finish()
        self.assertEqual(len(report.opaque), 1)
        self.assertEqual(report.opaque[0].record, historical)


if __name__ == "__main__":
    unittest.main()
