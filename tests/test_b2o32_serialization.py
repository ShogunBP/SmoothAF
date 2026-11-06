#!/usr/bin/env python3
"""Deterministic host-only tests for B2-O3.2 transport."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o32_serialization.py"
SPEC = importlib.util.spec_from_file_location("b2o32_serialization", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)
O31 = MODULE.O31


class B2O32SerializationTests(unittest.TestCase):
    def snapshot(self, **changes):
        values = {
            "timestamp_ms": 1000,
            "yuv_buffer_address": 0x1000,
            "width": 720,
            "pitch": 1440,
            "height": 480,
            "liveview": True,
            "movie_mode": True,
            "recording_class": O31.RecordingClass.H264,
            "edmac_write_address": 0x2000,
            "display_buffer_address": 0x3000,
            "exposure": O31.ExposureState(4, 80, 72, 0),
            "mvr_frame_number": 12,
        }
        values.update(changes)
        return O31.ObservationSnapshot.from_values(**values)

    def event(self, *, phase, logical_id=1, sequence=7, timestamp=1000,
              status=O31.Status.STABLE_METADATA,
              profile=O31.Profile.EXTENDED, **changes):
        snapshot = self.snapshot(timestamp_ms=timestamp, **changes)
        return MODULE.observation_event_from_snapshot(
            snapshot, phase=phase, episode_id=3, sequence=sequence,
            logical_id=logical_id, status=status, profile=profile,
        )

    def test_core_fixture_matches_python_wire_bytes(self) -> None:
        event = self.event(
            phase=MODULE.SnapshotPhase.BEFORE,
            profile=O31.Profile.CORE,
            edmac_write_address=None,
            display_buffer_address=None,
            exposure=None,
            mvr_frame_number=None,
        )
        expected = (ROOT / "tests" / "fixtures" / "obs_snapshot_v1_core.hex").read_text().strip()
        self.assertEqual(MODULE.serialize_record(event.to_record()).hex(), expected)

    def test_record_is_exactly_36_bytes_and_round_trips(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        data = MODULE.serialize_record(event.to_record())
        self.assertEqual(len(data), 36)
        decoded = MODULE.decode_snapshot(MODULE.deserialize_record(data))
        self.assertEqual(decoded, event)

    def test_wire_layout_is_little_endian_and_six_slots(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.AFTER, logical_id=0x1234,
                           sequence=0x10203040)
        record = event.to_record()
        data = MODULE.serialize_record(record)
        self.assertEqual(data[:4], (1000).to_bytes(4, "little"))
        self.assertEqual(data[4:6], MODULE.OBS_SNAPSHOT_V1_EVENT.to_bytes(2, "little"))
        self.assertEqual(data[6:8], (3).to_bytes(2, "little"))
        self.assertEqual(data[8:12], (0x10203040).to_bytes(4, "little"))
        self.assertEqual(data[12:16], (record.payload[0] & 0xFFFFFFFF).to_bytes(4, "little"))
        self.assertEqual(MODULE._RECORD.size, MODULE.RECORD_SIZE)
        self.assertEqual(len(record.payload), 6)

    def test_control_layout_contains_version_phase_status_profile_flags_and_id(self) -> None:
        event = self.event(
            phase=MODULE.SnapshotPhase.AFTER, logical_id=0x4321,
            status=O31.Status.CHANGED_DURING_MEASUREMENT,
            profile=O31.Profile.EXTENDED,
        )
        control = event.to_record().payload[0] & 0xFFFFFFFF
        logical_id, phase, status, profile, flags = MODULE.unpack_control(control)
        self.assertEqual(logical_id, 0x4321)
        self.assertEqual(phase, MODULE.SnapshotPhase.AFTER)
        self.assertEqual(status, O31.Status.CHANGED_DURING_MEASUREMENT)
        self.assertEqual(profile, O31.Profile.EXTENDED)
        self.assertEqual(flags, event.flags)
        self.assertEqual(control & MODULE.CONTROL_RESERVED_MASK, 0)

    def test_sentinel_distinguishes_missing_optional_fields(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE,
                           edmac_write_address=None, display_buffer_address=None,
                           exposure=None, mvr_frame_number=None)
        decoded = MODULE.decode_snapshot(
            MODULE.deserialize_record(MODULE.serialize_record(event.to_record()))
        )
        self.assertIsNone(decoded.edmac_write_address)
        self.assertIsNone(decoded.display_buffer_address)
        self.assertIsNone(decoded.exposure)
        self.assertIsNone(decoded.mvr_frame_number)
        self.assertFalse(decoded.flags & MODULE.SnapshotFlags.EDMAC_AVAILABLE)

    def test_unknown_event_is_preserved_as_opaque_historical_record(self) -> None:
        record = MODULE.LogRecord(1, 7, 2, 3, (1, 2, 3, 4, 5, 6))
        decoded = MODULE.decode_record(MODULE.serialize_record(record))
        self.assertIsInstance(decoded, MODULE.OpaqueRecord)
        self.assertEqual(decoded.record, record)

    def test_invalid_version_flags_reserved_bits_and_size_are_rejected(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        record = event.to_record()
        control = record.payload[0] & 0xFFFFFFFF
        for mutation in (
            control | (2 << MODULE.VERSION_SHIFT),
            control ^ (1 << MODULE.PROFILE_SHIFT),
        ):
            mutated = MODULE.LogRecord(record.timestamp_ms, record.event_type,
                                       record.episode_id, record.sequence,
                                       (MODULE._to_signed(mutation),) + record.payload[1:])
            with self.subTest(mutation=mutation):
                with self.assertRaises(MODULE.SerializationError):
                    MODULE.decode_snapshot(mutated)
        # Bits 16-31 are assigned to logical_id; mutating one is valid, not
        # an unknown/reserved-field mutation.
        logical_id_mutation = control ^ (1 << (MODULE.LOGICAL_ID_SHIFT + 1))
        logical_id, _, _, _, _ = MODULE.unpack_control(logical_id_mutation)
        self.assertEqual(logical_id, 3)
        with self.assertRaises(MODULE.SerializationError):
            MODULE.deserialize_record(b"short")

    def test_pair_complete_before_after_order_and_interleaving(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        before = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        other = self.event(phase=MODULE.SnapshotPhase.BEFORE, logical_id=2, sequence=8)
        after = self.event(phase=MODULE.SnapshotPhase.AFTER, sequence=9, timestamp=1002)
        pairer.add(other)
        pairer.add(before)
        pairer.add(MODULE.OpaqueRecord(
            MODULE.LogRecord(10, 7, 0, 99, (0, 0, 0, 0, 0, 0))))
        pairer.add(after)
        report = pairer.finish()
        self.assertEqual(len(report.complete), 1)
        self.assertEqual(report.complete[0].key, (3, 1))
        self.assertTrue(report.complete[0].stable_metadata)
        self.assertEqual(len(report.incomplete), 1)
        self.assertEqual(report.incomplete[0].status, MODULE.PairStatus.AFTER_MISSING)
        self.assertEqual(len(report.opaque), 1)

    def test_missing_sides_duplicates_and_invalid_events(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        before = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        pairer.add(before)
        pairer.add(before)
        pairer.add_bytes(b"bad")
        report = pairer.finish()
        self.assertEqual(len(report.duplicates), 1)
        self.assertEqual(report.incomplete[0].status, MODULE.PairStatus.AFTER_MISSING)
        self.assertEqual(len(report.invalid), 1)

        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER))
        report = pairer.finish()
        self.assertEqual(report.incomplete[0].status, MODULE.PairStatus.BEFORE_MISSING)
        self.assertFalse(report.incomplete[0].stable_metadata)

    def test_sequence_wrap_does_not_break_pairing(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.BEFORE, sequence=0xFFFFFFFF))
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER, sequence=0))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 1)
        self.assertEqual(report.complete[0].status, MODULE.PairStatus.COMPLETE)

    def test_reused_logical_id_is_separated_by_episode(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        for episode_id, timestamp in ((3, 1000), (4, 2000)):
            pairer.add(MODULE.observation_event_from_snapshot(
                self.snapshot(timestamp_ms=timestamp),
                phase=MODULE.SnapshotPhase.BEFORE, episode_id=episode_id,
                sequence=1, logical_id=1, status=O31.Status.STABLE_METADATA,
                profile=O31.Profile.EXTENDED,
            ))
            pairer.add(MODULE.observation_event_from_snapshot(
                self.snapshot(timestamp_ms=timestamp + 1),
                phase=MODULE.SnapshotPhase.AFTER, episode_id=episode_id,
                sequence=2, logical_id=1, status=O31.Status.STABLE_METADATA,
                profile=O31.Profile.EXTENDED,
            ))
        report = pairer.finish()
        self.assertEqual({pair.key for pair in report.complete}, {(3, 1), (4, 1)})

    def test_concurrent_episodes_are_separate_keys(self) -> None:
        first = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        second = MODULE.observation_event_from_snapshot(
            self.snapshot(timestamp_ms=2000), phase=MODULE.SnapshotPhase.BEFORE,
            episode_id=4, sequence=8, logical_id=1,
            status=O31.Status.STABLE_METADATA, profile=O31.Profile.EXTENDED,
        )
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        pairer.add(first)
        pairer.add(second)
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER, timestamp=1001))
        pairer.add(MODULE.observation_event_from_snapshot(
            self.snapshot(timestamp_ms=2001), phase=MODULE.SnapshotPhase.AFTER,
            episode_id=4, sequence=9, logical_id=1,
            status=O31.Status.STABLE_METADATA, profile=O31.Profile.EXTENDED,
        ))
        report = pairer.finish()
        self.assertEqual({pair.key for pair in report.complete}, {(3, 1), (4, 1)})

    def test_v1_reused_session_is_ambiguous_not_stable(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("file-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.BEFORE))
        pairer.end_source()
        pairer.boundary("file-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertTrue(any("AMBIGUOUS_PAIR" in message for message in report.invalid))
        self.assertEqual(report.incomplete[-1].status, MODULE.PairStatus.AMBIGUOUS_PAIR)
        self.assertFalse(report.incomplete[-1].stable_metadata)

    def test_explicit_session_boundary_prevents_cross_session_pairing(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("file-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.BEFORE))
        pairer.end_source()
        pairer.boundary("file-b")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 0)
        self.assertEqual({item.status for item in report.incomplete}, {
            MODULE.PairStatus.AFTER_MISSING, MODULE.PairStatus.BEFORE_MISSING,
        })

    def test_reused_wire_identity_is_ambiguous(self) -> None:
        pairer = MODULE.Pairer()
        pairer.boundary("session-a")
        pairer.add(self.event(phase=MODULE.SnapshotPhase.BEFORE))
        pairer.add(self.event(phase=MODULE.SnapshotPhase.AFTER, timestamp=1001))
        pairer.add(self.event(phase=MODULE.SnapshotPhase.BEFORE, timestamp=2000))
        report = pairer.finish()
        self.assertEqual(len(report.complete), 1)
        self.assertEqual(report.incomplete[-1].status, MODULE.PairStatus.AMBIGUOUS_PAIR)
        self.assertFalse(report.incomplete[-1].stable_metadata)

    def test_logical_id_wrap_skips_zero(self) -> None:
        self.assertEqual(MODULE.next_logical_id(0), 1)
        self.assertEqual(MODULE.next_logical_id(1), 2)
        self.assertEqual(MODULE.next_logical_id(0xFFFE), 1)
        self.assertEqual(MODULE.next_logical_id(0xFFFF), 1)
        with self.assertRaises(MODULE.SerializationError):
            MODULE.next_logical_id(0x10000)
        with self.assertRaises(MODULE.SerializationError):
            self.event(phase=MODULE.SnapshotPhase.BEFORE, logical_id=0).to_record()
        with self.assertRaises(MODULE.SerializationError):
            self.event(phase=MODULE.SnapshotPhase.BEFORE, logical_id=0xFFFF).to_record()

    def test_rate_limiter_first_status_change_window_burst_and_last(self) -> None:
        limiter = MODULE.RateLimiter(100)
        first = limiter.observe(MODULE.ObservationCandidate(1, 1000, O31.Status.STABLE_METADATA))
        burst = limiter.observe(MODULE.ObservationCandidate(2, 1050, O31.Status.STABLE_METADATA))
        status = limiter.observe(MODULE.ObservationCandidate(3, 1100, O31.Status.CHANGED_DURING_MEASUREMENT))
        rapid = limiter.observe(MODULE.ObservationCandidate(6, 1110, O31.Status.STABLE_METADATA))
        self.assertEqual(rapid.action, MODULE.PublishAction.SUPPRESS)
        window = limiter.observe(MODULE.ObservationCandidate(4, 1200, O31.Status.CHANGED_DURING_MEASUREMENT))
        last = limiter.observe(MODULE.ObservationCandidate(5, 1210, O31.Status.CHANGED_DURING_MEASUREMENT))
        final = limiter.finish()
        self.assertEqual(first.reason, MODULE.PublishReason.FIRST)
        self.assertEqual(burst.action, MODULE.PublishAction.SUPPRESS)
        self.assertEqual(status.reason, MODULE.PublishReason.STATUS_CHANGE)
        self.assertEqual(window.reason, MODULE.PublishReason.WINDOW_ELAPSED)
        self.assertEqual(last.action, MODULE.PublishAction.SUPPRESS)
        self.assertEqual(final.reason, MODULE.PublishReason.LAST)

    def test_rate_limiter_timestamp_wrap_is_deterministic(self) -> None:
        limiter = MODULE.RateLimiter(32)
        limiter.observe(MODULE.ObservationCandidate(1, 0xFFFFFFF0, O31.Status.STABLE_METADATA))
        decision = limiter.observe(MODULE.ObservationCandidate(2, 0x00000010, O31.Status.STABLE_METADATA))
        self.assertEqual(decision.reason, MODULE.PublishReason.WINDOW_ELAPSED)
        self.assertEqual(decision.action, MODULE.PublishAction.PUBLISH)

    def test_ring_has_256_physical_and_255_usable_slots(self) -> None:
        ring = MODULE.SimulatedRing()
        self.assertEqual(ring.physical_slots, 256)
        self.assertEqual(ring.usable_slots, 255)
        self.assertEqual(ring.record_size, 36)
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        data = MODULE.serialize_record(event.to_record())
        for _ in range(255):
            self.assertTrue(ring.publish(data))
        self.assertFalse(ring.publish(data))
        self.assertEqual(ring.dropped_count, 1)

    def test_try_lock_and_consumer_delay_drop_without_blocking(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        ring = MODULE.SimulatedRing(try_lock_available=False)
        self.assertFalse(ring.publish(MODULE.serialize_record(event.to_record())))
        self.assertEqual(ring.contention_count, 1)
        self.assertEqual(ring.dropped_count, 1)

        ring = MODULE.SimulatedRing()
        data = MODULE.serialize_record(event.to_record())
        for _ in range(5):
            self.assertTrue(ring.publish(data))
        self.assertEqual(ring.reserve(3), 3)
        self.assertEqual(ring.fill, 5)
        self.assertFalse(ring.flush(fail_after=1))
        self.assertEqual(ring.flushed_count, 1)

    def test_partial_pair_publication_is_not_complete(self) -> None:
        before = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        after = self.event(phase=MODULE.SnapshotPhase.AFTER, timestamp=1001)
        ring = MODULE.SimulatedRing()
        ring.try_lock_available = False
        result = MODULE.publish_pair(ring, before, after)
        self.assertEqual(result.status, MODULE.PairTransportStatus.BOTH_DROPPED)
        self.assertFalse(result.before_published)
        self.assertFalse(result.after_published)

        ring = MODULE.SimulatedRing()
        for _ in range(254):
            ring.publish(MODULE.serialize_record(before.to_record()))
        result = MODULE.publish_pair(ring, before, after)
        self.assertEqual(result.status, MODULE.PairTransportStatus.AFTER_DROPPED)
        self.assertTrue(result.before_published)
        self.assertFalse(result.after_published)

    def test_serialization_preserves_signed_and_unsigned_int32_bit_patterns(self) -> None:
        for value, expected_signed in (
            (-2147483648, -2147483648), (-1, -1),
            (0x80000000, -2147483648), (0xFFFFFFFF, -1),
        ):
            record = MODULE.LogRecord(0xFFFFFFFF, 7, 0xFFFF, 0xFFFFFFFF,
                                      (value, 0, 1, 2, 3, value))
            decoded = MODULE.deserialize_record(MODULE.serialize_record(record))
            self.assertEqual(decoded.payload[0], expected_signed)
            self.assertEqual(decoded.payload[5], expected_signed)
            self.assertEqual(MODULE._payload_bits(decoded.payload[0]), value & 0xFFFFFFFF)

    def test_serialization_does_not_modify_historical_record(self) -> None:
        historical = MODULE.LogRecord(0xFFFFFFFF, 7, 0xFFFF, 0xFFFFFFFF,
                                      (0, 1, 2, 3, 4, -1))
        decoded = MODULE.deserialize_record(MODULE.serialize_record(historical))
        self.assertEqual(decoded, historical)

    def test_mvr_wrap_requires_mvr_value(self) -> None:
        event = MODULE.observation_event_from_snapshot(
            self.snapshot(mvr_frame_number=None), phase=MODULE.SnapshotPhase.BEFORE,
            episode_id=3, sequence=7, logical_id=1,
            status=O31.Status.STABLE_METADATA, profile=O31.Profile.EXTENDED,
            mvr_wrapped=True,
        )
        with self.assertRaises(MODULE.SerializationError):
            MODULE.decode_snapshot(MODULE.deserialize_record(
                MODULE.serialize_record(event.to_record())
            ))

    def test_mutated_payload_availability_is_rejected(self) -> None:
        event = self.event(phase=MODULE.SnapshotPhase.BEFORE)
        record = event.to_record()
        # Clear the YUV sentinel while retaining the availability bit.
        payload = list(record.payload)
        payload[1] = 0xFFFFFFFF
        mutated = MODULE.LogRecord(record.timestamp_ms, record.event_type,
                                   record.episode_id, record.sequence,
                                   tuple(payload))
        with self.assertRaises(MODULE.SerializationError):
            MODULE.decode_snapshot(mutated)


if __name__ == "__main__":
    unittest.main()
