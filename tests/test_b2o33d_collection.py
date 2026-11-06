#!/usr/bin/env python3
"""Host-only tests for the B2-O3.3d controlled-collection contract."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o33d_collection.py"
SPEC = importlib.util.spec_from_file_location("b2o33d_collection", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = MODULE.BASE
O31 = BASE.O31


class B2O33DCollectionTests(unittest.TestCase):
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

    def stream(self, *events: BASE.ObservationEvent) -> bytes:
        return b"".join(BASE.serialize_record(event.to_record()) for event in events)

    def manifest(self, data: bytes, **changes):
        values = dict(
            format=MODULE.ArtifactFormat.BINARY_V1,
            original_filename="SMTH017.LOG.BIN",
            source_id="card-copy-017",
            expected_sha256=MODULE.sha256_hex(data),
            mode_context=(
                ("recording", "H264_REC_ON"),
                ("run_mode", "DRY_RUN"),
                ("lens_motion", "PROHIBITED"),
            ),
            start_known=True,
            end_known=True,
            session_scope=MODULE.SessionScope.SINGLE_SESSION,
            transfer_chain=("camera-card", "write-protected-card-read", "host-copy"),
            size_bytes=len(data),
            manifest_version="1",
            marker_state=MODULE.MarkerState.ABSENT,
        )
        values.update(changes)
        return MODULE.CollectionManifest(**values)

    def artifact(self, data: bytes, **changes):
        return MODULE.CollectionArtifact(data, self.manifest(data, **changes))

    def test_valid_single_session_pairs_inside_verified_artifact(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertTrue(collector.add_artifact(self.artifact(data)))
        report = collector.finish()
        self.assertEqual(len(report.accepted), 1)
        self.assertEqual(len(report.pairing.complete), 1)
        self.assertTrue(report.pairing.complete[0].stable_metadata)
        self.assertEqual(report.pairing.complete[0].source, "card-copy-017")
        self.assertEqual(report.session_identity, MODULE.SessionIdentityStatus.UNAVAILABLE)
        self.assertEqual(report.eligibility, MODULE.CollectionEligibility.ACCEPT)

    def test_two_sessions_same_file_are_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE, episode=3),
            self.event(BASE.SnapshotPhase.AFTER, episode=3, timestamp=1001),
            self.event(BASE.SnapshotPhase.BEFORE, episode=4, timestamp=2000),
            self.event(BASE.SnapshotPhase.AFTER, episode=4, timestamp=2001),
        )
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(
            self.artifact(data, session_scope=MODULE.SessionScope.MULTI_SESSION)
        ))
        report = collector.finish()
        self.assertEqual(len(report.accepted), 0)
        self.assertTrue(any("mixed" in item for item in report.rejected))

    def test_concatenated_artifact_is_rejected_even_when_hash_matches(self):
        session = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        concatenated = session + session
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(
            self.artifact(concatenated, session_scope=MODULE.SessionScope.CONCATENATED)
        ))
        self.assertEqual(len(collector.finish().pairing.complete), 0)

    def test_restarts_require_distinct_sources_and_never_cross_pair(self):
        before = self.stream(self.event(BASE.SnapshotPhase.BEFORE))
        after = self.stream(self.event(BASE.SnapshotPhase.AFTER, timestamp=1001))
        collector = MODULE.ControlledCollector()
        self.assertTrue(collector.add_artifact(self.artifact(
            before, original_filename="SMTH017.LOG.BIN", source_id="boot-a"
        )))
        self.assertTrue(collector.add_artifact(self.artifact(
            after, original_filename="SMTH018.LOG.BIN", source_id="boot-b"
        )))
        report = collector.finish()
        self.assertEqual(len(report.pairing.complete), 0)
        self.assertEqual({item.status for item in report.pairing.incomplete}, {
            BASE.PairStatus.AFTER_MISSING, BASE.PairStatus.BEFORE_MISSING,
        })

    def test_before_and_after_loss_stays_incomplete(self):
        before_only = self.stream(self.event(BASE.SnapshotPhase.BEFORE))
        after_only = self.stream(self.event(BASE.SnapshotPhase.AFTER))
        for data, expected in ((before_only, BASE.PairStatus.AFTER_MISSING),
                               (after_only, BASE.PairStatus.BEFORE_MISSING)):
            collector = MODULE.ControlledCollector()
            self.assertTrue(collector.add_artifact(self.artifact(data)))
            report = collector.finish()
            self.assertEqual(len(report.pairing.complete), 0)
            self.assertEqual(report.pairing.incomplete[0].status, expected)

    def test_incomplete_marker_is_not_accepted_as_v1_provenance(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(
            data, marker_state=MODULE.MarkerState.PARTIAL
        )))
        self.assertEqual(len(collector.finish().pairing.complete), 0)

    def test_duplicate_source_id_is_rejected(self):
        data_a = self.stream(self.event(BASE.SnapshotPhase.BEFORE))
        data_b = self.stream(self.event(BASE.SnapshotPhase.AFTER))
        collector = MODULE.ControlledCollector()
        self.assertTrue(collector.add_artifact(self.artifact(data_a)))
        self.assertFalse(collector.add_artifact(self.artifact(data_b)))
        report = collector.finish()
        self.assertTrue(any("duplicate source_id" in item for item in report.rejected))
        self.assertEqual(len(report.pairing.complete), 0)

    def test_ambiguous_duplicate_wire_key_is_not_stable(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
            self.event(BASE.SnapshotPhase.BEFORE, timestamp=2000),
        )
        collector = MODULE.ControlledCollector()
        self.assertTrue(collector.add_artifact(self.artifact(data)))
        report = collector.finish()
        self.assertEqual(len(report.pairing.complete), 1)
        self.assertEqual(report.pairing.incomplete[-1].status, BASE.PairStatus.AMBIGUOUS_PAIR)
        self.assertFalse(report.pairing.incomplete[-1].stable_metadata)

    def test_event_25_is_not_silently_accepted_by_v1_contract(self):
        token = bytes.fromhex("00112233445566778899aabbccddeeff")
        marker = BASE.LogRecord(
            timestamp_ms=1, event_type=25, episode_id=0, sequence=1,
            payload=(0x101, 0x33221100, 0x77665544, 0xbbaa9988, 0xffeeddcc, 0),
        )
        data = BASE.serialize_record(marker)
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(data)))
        report = collector.finish()
        self.assertTrue(any("FORMAT_UNSUPPORTED" in item for item in report.rejected))

    def test_truncated_stream_is_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )[:-1]
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(data)))
        report = collector.finish()
        self.assertTrue(any("whole-record" in item for item in report.rejected))

    def test_manifest_size_mismatch_is_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(data, size_bytes=len(data) - 1)))
        self.assertTrue(any("byte length" in item for item in collector.finish().rejected))

    def test_hash_mismatch_and_missing_external_metadata_are_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(data, expected_sha256="0" * 64)))
        self.assertFalse(collector.add_artifact(self.artifact(data, source_id=None)))
        self.assertFalse(collector.add_artifact(self.artifact(data, mode_context=None)))
        self.assertEqual(len(collector.finish().pairing.complete), 0)

    def test_duplicate_artifact_hash_is_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertTrue(collector.add_artifact(self.artifact(data, source_id="copy-a")))
        self.assertFalse(collector.add_artifact(self.artifact(data, source_id="copy-b")))
        report = collector.finish()
        self.assertTrue(any("duplicate artifact hash" in item for item in report.rejected))

    def test_unknown_session_scope_and_unknown_boundaries_are_rejected(self):
        data = self.stream(
            self.event(BASE.SnapshotPhase.BEFORE),
            self.event(BASE.SnapshotPhase.AFTER, timestamp=1001),
        )
        collector = MODULE.ControlledCollector()
        self.assertFalse(collector.add_artifact(self.artifact(
            data, session_scope=MODULE.SessionScope.UNKNOWN
        )))
        self.assertFalse(collector.add_artifact(self.artifact(
            data, start_known=False
        )))
        self.assertFalse(collector.add_artifact(self.artifact(
            data, end_known=False
        )))
        self.assertEqual(len(collector.finish().pairing.complete), 0)


if __name__ == "__main__":
    unittest.main()
