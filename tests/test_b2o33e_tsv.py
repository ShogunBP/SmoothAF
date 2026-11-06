#!/usr/bin/env python3
"""Host-only tests for the production nine-column TSV projection."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o33e_tsv.py"
SPEC = importlib.util.spec_from_file_location("b2o33e_tsv", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)

BASE = MODULE.BASE


class B2O33ETsvTests(unittest.TestCase):
    def line(self, timestamp, event, episode, payload):
        return (f"{timestamp}\t{event}\t{episode}\t" +
                "\t".join(str(value) for value in payload) + "\n").encode("ascii")

    def manifest(self, data, **changes):
        values = dict(
            format=MODULE.ArtifactFormat.FIO_TSV9,
            original_filename="SMTH017.LOG",
            source_id="acquisition-017",
            expected_sha256=MODULE.COLLECTION.sha256_hex(data),
            mode_context=(("recording", "H264_REC_ON"), ("run", "DRY_RUN")),
            start_known=True,
            end_known=True,
            session_scope=MODULE.SessionScope.SINGLE_SESSION,
            transfer_chain=("card-reader", "host-copy"),
            size_bytes=len(data),
            marker_state=MODULE.COLLECTION.MarkerState.ABSENT,
        )
        values.update(changes)
        return MODULE.CollectionManifest(**values)

    def valid_snapshot_lines(self):
        before = (0x00010301, 0x1000, -1, -1, -1, -1)
        after = (0x00010311, 0x1000, -1, -1, -1, -1)
        return (
            self.line(100, "OBS_SNAPSHOT_V1", 3, before) +
            self.line(100, "OBS_SNAPSHOT_V1", 3, after)
        )

    def test_schema_is_nine_ascii_tsv_fields_and_sequence_is_absent(self):
        data = self.line(10, "SESSION_START", 0, (0, 100, 4, 0, 0, 0))
        result = MODULE.parse_tsv(data)
        self.assertEqual(result.integrity, MODULE.TsvIntegrityStatus.COMPLETE)
        self.assertEqual(len(result.records), 1)
        self.assertIsNone(result.records[0].sequence)
        self.assertEqual(result.records[0].event_type, 1)
        self.assertEqual(result.records[0].payload, (0, 100, 4, 0, 0, 0))

    def test_snapshot_before_after_are_decoded_in_physical_order_with_equal_timestamps(self):
        data = self.valid_snapshot_lines()
        result = MODULE.parse_tsv(data)
        self.assertEqual(result.errors, ())
        self.assertEqual([item.timestamp_ms for item in result.snapshots], [100, 100])
        self.assertEqual([item.phase for item in result.snapshots],
                         [BASE.SnapshotPhase.BEFORE, BASE.SnapshotPhase.AFTER])
        self.assertEqual([item.sequence for item in result.snapshots], [None, None])

    def test_collection_uses_provenance_but_does_not_claim_session_identity(self):
        data = self.valid_snapshot_lines()
        report = MODULE.collect_tsv(data, self.manifest(data))
        self.assertTrue(report.accepted_source)
        self.assertEqual(len(report.pairing.complete), 1)
        self.assertEqual(report.pairing.complete[0].source,
                         "acquisition-017#segment-0")
        self.assertEqual(report.session_identity, MODULE.SessionIdentityStatus.UNAVAILABLE)
        self.assertEqual(report.eligibility, MODULE.CollectionEligibility.REJECT)
        self.assertEqual(report.descriptive_eligibility,
                         MODULE.TsvDescriptiveEligibility.ACCEPT_LOCAL)
        self.assertEqual(report.operational_calibration_eligibility,
                         MODULE.TsvOperationalCalibrationEligibility.REJECT)
        self.assertFalse(report.frame_independence_proven)

    def test_multiple_sessions_and_concatenation_are_rejected(self):
        data = self.line(1, "SESSION_START", 0, (0, 100, 4, 0, 0, 0))
        data += self.valid_snapshot_lines()
        data += self.line(2, "SESSION_END", 0, (1, 100, 1, 0, 0, 0))
        data += self.line(3, "SESSION_START", 0, (0, 100, 4, 0, 0, 0))
        report = MODULE.collect_tsv(data, self.manifest(
            data, session_scope=MODULE.SessionScope.MULTI_SESSION
        ))
        self.assertFalse(report.accepted_source)
        self.assertTrue(any("multiple" in item for item in report.rejected))
        self.assertEqual(report.descriptive_eligibility,
                         MODULE.TsvDescriptiveEligibility.ACCEPT_LOCAL)
        self.assertEqual(report.operational_calibration_eligibility,
                         MODULE.TsvOperationalCalibrationEligibility.REJECT)

    def test_before_after_loss_and_duplicate_key_are_preserved(self):
        before = self.line(100, "OBS_SNAPSHOT_V1", 3,
                           (0x00010301, 0x1000, -1, -1, -1, -1))
        report = MODULE.collect_tsv(before, self.manifest(before))
        self.assertEqual(report.pairing.incomplete[0].status, BASE.PairStatus.AFTER_MISSING)

        duplicate = self.valid_snapshot_lines() + before
        report = MODULE.collect_tsv(duplicate, self.manifest(duplicate))
        self.assertTrue(any(item.status == BASE.PairStatus.AMBIGUOUS_PAIR
                            for item in report.pairing.incomplete))

    def test_invalid_field_count_value_range_payload_and_unknown_event(self):
        invalid = b"100\tOBS_SNAPSHOT_V1\t3\t1\t2\n"
        result = MODULE.parse_tsv(invalid)
        self.assertEqual(result.integrity, MODULE.TsvIntegrityStatus.MALFORMED_RECORD)
        self.assertEqual(len(result.records), 0)

        out_of_range = self.line(100, "SESSION_START", 0,
                                 (0, 0, 0, 0, 0, 2147483648))
        result = MODULE.parse_tsv(out_of_range)
        self.assertTrue(result.errors)

        unknown = self.line(100, "FUTURE_EVENT", 0, (0, 1, 2, 3, 4, 5))
        result = MODULE.parse_tsv(unknown)
        self.assertEqual(result.errors, ())
        self.assertIsNone(result.records[0].event_type)
        self.assertEqual(len(result.records), 1)

    def test_signed_and_unsigned_contracts(self):
        signed = self.line(100, "MEASUREMENT", 1, (-1, -2, -3, -4, -5, -6))
        self.assertFalse(MODULE.parse_tsv(signed).errors)
        unsigned = self.line(100, "D1_SAMPLE", 1, (4294967295, -2, -3, -4, -5, -6))
        self.assertFalse(MODULE.parse_tsv(unsigned).errors)
        invalid_unsigned = self.line(100, "D1_SAMPLE", 1, (-1, -2, -3, -4, -5, -6))
        self.assertTrue(MODULE.parse_tsv(invalid_unsigned).errors)

    def test_final_line_without_lf_is_truncation(self):
        result = MODULE.parse_tsv(self.valid_snapshot_lines().rstrip(b"\n"))
        self.assertEqual(result.integrity, MODULE.TsvIntegrityStatus.PARTIAL_FINAL_LINE)
        self.assertTrue(result.errors)

    def test_non_ascii_and_crlf_are_handled_deterministically(self):
        crlf = self.valid_snapshot_lines().replace(b"\n", b"\r\n")
        self.assertFalse(MODULE.parse_tsv(crlf).errors)
        non_ascii = b"100\tUNKNOWN\t0\t0\t0\t0\t0\t0\t\xff\n"
        self.assertTrue(MODULE.parse_tsv(non_ascii).errors)

    def test_missing_and_duplicate_provenance_are_rejected(self):
        data = self.valid_snapshot_lines()
        missing = MODULE.collect_tsv(data, self.manifest(data, source_id=None))
        self.assertFalse(missing.accepted_source)
        duplicate = MODULE.collect_tsv(
            data, self.manifest(data, source_id="already-used"),
            duplicate_source_ids={"already-used"},
        )
        self.assertFalse(duplicate.accepted_source)

    def test_b2o33d_binary_contract_rejects_tsv_format(self):
        data = self.valid_snapshot_lines()
        binary_manifest = MODULE.CollectionManifest(
            format=MODULE.ArtifactFormat.BINARY_V1,
            original_filename="SMTH017.LOG",
            source_id="binary-mislabel",
            expected_sha256=MODULE.COLLECTION.sha256_hex(data),
            mode_context=(("recording", "H264_REC_ON"),),
            start_known=True, end_known=True,
            session_scope=MODULE.SessionScope.SINGLE_SESSION,
            transfer_chain=("card-reader",), size_bytes=len(data),
        )
        with self.assertRaises(Exception):
            MODULE.COLLECTION.validate_artifact(
                MODULE.COLLECTION.CollectionArtifact(data, binary_manifest)
            )

    def test_event_25_and_event_zero_are_not_recoverable_from_unknown_text(self):
        for episode in (0, 1):
            data = self.line(1, "UNKNOWN", episode, (257, 1, 2, 3, 4, 5))
            result = MODULE.parse_tsv(data)
            self.assertEqual(result.errors, ())
            self.assertIsNone(result.records[0].event_type)

    def test_timestamp_wrap_and_historical_events_are_not_identity(self):
        data = self.line(0xFFFFFFFF, "MEASUREMENT", 3, (-1, -2, -3, -4, -5, -6))
        data += self.line(0, "MEASUREMENT", 3, (-1, -2, -3, -4, -5, -6))
        data += self.line(1, "SESSION_START", 0, (0, 0, 0, 0, 0, 0))
        result = MODULE.parse_tsv(data)
        self.assertFalse(result.errors)
        self.assertEqual([record.timestamp_ms for record in result.records],
                         [0xFFFFFFFF, 0, 1])
        self.assertTrue(all(snapshot.sequence is None for snapshot in result.snapshots))


if __name__ == "__main__":
    unittest.main()
