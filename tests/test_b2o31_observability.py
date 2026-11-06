#!/usr/bin/env python3
"""Deterministic host-only tests for the B2-O3.1 observability contract."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2o31_observability.py"
SPEC = importlib.util.spec_from_file_location("b2o31_observability", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class B2O31ObservabilityTests(unittest.TestCase):
    def snapshot(self, **changes):
        values = {
            "timestamp_ms": 1000,
            "yuv_buffer_address": 0x1000,
            "width": 720,
            "pitch": 1440,
            "height": 480,
            "liveview": True,
            "movie_mode": True,
            "recording_class": MODULE.RecordingClass.H264,
            "edmac_write_address": 0x2000,
            "display_buffer_address": 0x3000,
            "exposure": MODULE.ExposureState(4, 80, 72, 0),
            "mvr_frame_number": None,
            "environment_valid": True,
        }
        values.update(changes)
        return MODULE.ObservationSnapshot.from_values(**values)

    def test_core_stable_metadata_does_not_claim_frame_identity(self) -> None:
        before = self.snapshot()
        after = self.snapshot(timestamp_ms=1032)
        result = MODULE.classify(before, after, MODULE.Profile.CORE)

        self.assertEqual(result.status, MODULE.Status.STABLE_METADATA)
        self.assertEqual(result.changed_mask, MODULE.Field.NONE)
        self.assertEqual(result.missing_mask, MODULE.Field.NONE)
        self.assertEqual(result.available_mask, MODULE.CORE_FIELDS)
        self.assertFalse(result.frame_identity_proven)
        self.assertFalse(result.frame_independence_proven)
        self.assertEqual(result.timestamp_delta_ms, 32)

    def test_extended_missing_mvr_is_insufficient_not_stable(self) -> None:
        result = MODULE.classify(
            self.snapshot(), self.snapshot(timestamp_ms=1001), MODULE.Profile.EXTENDED
        )

        self.assertEqual(result.status, MODULE.Status.INSUFFICIENT_OBSERVABILITY)
        self.assertTrue(result.missing_mask & MODULE.Field.MVR_FRAME)
        self.assertFalse(result.missing_mask & MODULE.Field.EDMAC)
        self.assertFalse(result.missing_mask & MODULE.Field.DISPLAY)
        self.assertFalse(result.missing_mask & MODULE.Field.EXPOSURE)

    def test_optional_absence_does_not_change_core_result(self) -> None:
        full = MODULE.classify(
            self.snapshot(), self.snapshot(timestamp_ms=1001), MODULE.Profile.CORE
        )
        reduced = MODULE.classify(
            self.snapshot(edmac_write_address=None, display_buffer_address=None,
                          exposure=None),
            self.snapshot(timestamp_ms=1001, edmac_write_address=None,
                          display_buffer_address=None, exposure=None),
            MODULE.Profile.CORE,
        )

        self.assertEqual(full.status, MODULE.Status.STABLE_METADATA)
        self.assertEqual(reduced.status, MODULE.Status.STABLE_METADATA)
        self.assertEqual(reduced.changed_mask, MODULE.Field.NONE)

    def test_edmac_display_exposure_changes_are_preserved(self) -> None:
        after = self.snapshot(
            timestamp_ms=1004,
            edmac_write_address=0x2100,
            display_buffer_address=0x3100,
            exposure=MODULE.ExposureState(4, 81, 72, 0),
        )
        result = MODULE.classify(
            self.snapshot(), after, MODULE.Profile.EXTENDED
        )

        self.assertEqual(result.status, MODULE.Status.CHANGED_DURING_MEASUREMENT)
        self.assertTrue(result.changed_mask & MODULE.Field.EDMAC)
        self.assertTrue(result.changed_mask & MODULE.Field.DISPLAY)
        self.assertTrue(result.changed_mask & MODULE.Field.EXPOSURE)
        self.assertEqual(result.missing_mask, MODULE.Field.MVR_FRAME)

    def test_required_core_field_missing_is_invalid_environment(self) -> None:
        result = MODULE.classify(
            self.snapshot(yuv_buffer_address=None),
            self.snapshot(timestamp_ms=1001, yuv_buffer_address=None),
            MODULE.Profile.CORE,
        )

        self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
        self.assertTrue(result.missing_mask & MODULE.Field.YUV_BUFFER)
        self.assertEqual(result.missing_required_mask, MODULE.Field.YUV_BUFFER)

    def test_invalid_environment_precedes_optional_missing_and_changes(self) -> None:
        result = MODULE.classify(
            self.snapshot(environment_valid=False, edmac_write_address=0x2010),
            self.snapshot(timestamp_ms=1001, edmac_write_address=0x2020,
                          display_buffer_address=None, exposure=None),
            MODULE.Profile.EXTENDED,
        )

        self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
        self.assertTrue(result.changed_mask & MODULE.Field.EDMAC)
        self.assertTrue(result.missing_mask & MODULE.Field.DISPLAY)
        self.assertTrue(result.missing_mask & MODULE.Field.EXPOSURE)

    def test_recording_change_is_invalid_environment(self) -> None:
        result = MODULE.classify(
            self.snapshot(),
            self.snapshot(timestamp_ms=1001,
                          recording_class=MODULE.RecordingClass.NONE),
            MODULE.Profile.CORE,
        )

        self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
        self.assertTrue(result.changed_mask & MODULE.Field.RECORDING_CLASS)

    def test_lv_movie_and_geometry_changes_are_invalid_environment(self) -> None:
        for changes, field in (
            ({"liveview": False}, MODULE.Field.LIVEVIEW),
            ({"movie_mode": False}, MODULE.Field.MOVIE_MODE),
            ({"width": 704}, MODULE.Field.GEOMETRY),
            ({"pitch": 1000}, MODULE.Field.GEOMETRY),
            ({"height": 0}, MODULE.Field.GEOMETRY),
        ):
            with self.subTest(field=field):
                result = MODULE.classify(
                    self.snapshot(),
                    self.snapshot(timestamp_ms=1001, **changes),
                    MODULE.Profile.CORE,
                )
                self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
                self.assertTrue(result.changed_mask & field or
                                result.invalid_mask & field)

    def test_cancel_and_panic_are_invalid_environment(self) -> None:
        for reason in (MODULE.InvalidReason.CANCELLED, MODULE.InvalidReason.PANIC):
            with self.subTest(reason=reason):
                result = MODULE.classify(
                    self.snapshot(invalid_reasons=reason),
                    self.snapshot(timestamp_ms=1001),
                    MODULE.Profile.CORE,
                )
                self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
                self.assertTrue(result.invalid_reason_mask & reason)

    def test_timestamp_wrap_is_valid_and_preserved(self) -> None:
        result = MODULE.classify(
            self.snapshot(timestamp_ms=0xFFFFFFF0),
            self.snapshot(timestamp_ms=0x00000010),
            MODULE.Profile.CORE,
        )

        self.assertEqual(result.status, MODULE.Status.STABLE_METADATA)
        self.assertEqual(result.timestamp_delta_ms, 32)
        self.assertTrue(result.timestamp_wrapped)
        self.assertFalse(result.timestamp_ambiguous)

    def test_timestamp_non_monotonic_is_invalid(self) -> None:
        result = MODULE.classify(
            self.snapshot(timestamp_ms=1000),
            self.snapshot(timestamp_ms=500),
            MODULE.Profile.CORE,
        )

        self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
        self.assertTrue(result.timestamp_ambiguous)

    def test_mvr_forward_change_and_wrap_are_metadata_changes(self) -> None:
        changed = MODULE.classify(
            self.snapshot(mvr_frame_number=10),
            self.snapshot(timestamp_ms=1001, mvr_frame_number=11),
            MODULE.Profile.EXTENDED,
        )
        wrapped = MODULE.classify(
            self.snapshot(mvr_frame_number=0xFFFFFFFF),
            self.snapshot(timestamp_ms=1001, mvr_frame_number=0),
            MODULE.Profile.EXTENDED,
        )

        self.assertEqual(changed.status, MODULE.Status.CHANGED_DURING_MEASUREMENT)
        self.assertTrue(changed.changed_mask & MODULE.Field.MVR_FRAME)
        self.assertEqual(wrapped.status, MODULE.Status.CHANGED_DURING_MEASUREMENT)
        self.assertTrue(wrapped.mvr_wrapped)

    def test_mvr_non_monotonic_is_invalid(self) -> None:
        result = MODULE.classify(
            self.snapshot(mvr_frame_number=100),
            self.snapshot(timestamp_ms=1001, mvr_frame_number=50),
            MODULE.Profile.EXTENDED,
        )

        self.assertEqual(result.status, MODULE.Status.INVALID_ENVIRONMENT)
        self.assertTrue(result.mvr_non_monotonic)

    def test_exposure_round_trip_and_validation(self) -> None:
        exposure = MODULE.ExposureState(4, 80, 72, 0)
        packed = MODULE.pack_exposure(exposure)
        self.assertEqual(MODULE.unpack_exposure(packed), exposure)
        with self.assertRaises(ValueError):
            MODULE.pack_exposure(MODULE.ExposureState(256, 80, 72, 0))

    def test_mvr_adapter_is_initially_unavailable_and_refuses_wrong_context(self) -> None:
        adapter = MODULE.FakeMvrAdapter(frame_number=12, available=False)
        unavailable = adapter.read(liveview=True, movie_mode=True,
                                   recording_class=MODULE.RecordingClass.H264)
        refused = MODULE.FakeMvrAdapter(frame_number=12, available=True).read(
            liveview=True, movie_mode=False,
            recording_class=MODULE.RecordingClass.H264,
        )

        self.assertEqual(unavailable.status, MODULE.MvrReadStatus.UNAVAILABLE)
        self.assertEqual(refused.status, MODULE.MvrReadStatus.REFUSED)
        self.assertIsNone(unavailable.frame_number)
        self.assertIsNone(refused.frame_number)

    def test_measurement_raw_values_are_distinct_from_post_flags(self) -> None:
        measurement = MODULE.MeasurementResult(
            score=900, mean=50, sad=100, laplacian=900,
            valid=False, confident=True,
        )
        result = MODULE.classify(
            self.snapshot(), self.snapshot(timestamp_ms=1001), MODULE.Profile.CORE,
            measurement=measurement,
        )

        self.assertEqual(result.measurement, measurement)
        self.assertEqual(result.measurement.score, 900)
        self.assertFalse(result.measurement.valid)
        self.assertTrue(result.measurement.confident)
        self.assertEqual(result.status, MODULE.Status.STABLE_METADATA)

    def test_mutation_of_each_declared_field_is_detected(self) -> None:
        mutations = (
            ("yuv_buffer_address", MODULE.Field.YUV_BUFFER),
            ("width", MODULE.Field.GEOMETRY),
            ("pitch", MODULE.Field.GEOMETRY),
            ("height", MODULE.Field.GEOMETRY),
            ("liveview", MODULE.Field.LIVEVIEW),
            ("movie_mode", MODULE.Field.MOVIE_MODE),
            ("recording_class", MODULE.Field.RECORDING_CLASS),
            ("edmac_write_address", MODULE.Field.EDMAC),
            ("display_buffer_address", MODULE.Field.DISPLAY),
            ("exposure", MODULE.Field.EXPOSURE),
            ("mvr_frame_number", MODULE.Field.MVR_FRAME),
        )
        for name, field in mutations:
            with self.subTest(name=name):
                before = self.snapshot(mvr_frame_number=10)
                value = {
                    "yuv_buffer_address": 0x1010,
                    "width": 704,
                    "pitch": 1410,
                    "height": 478,
                    "liveview": False,
                    "movie_mode": False,
                    "recording_class": MODULE.RecordingClass.NONE,
                    "edmac_write_address": 0x2010,
                    "display_buffer_address": 0x3010,
                    "exposure": MODULE.ExposureState(4, 81, 72, 0),
                    "mvr_frame_number": 11,
                }[name]
                after_values = {"timestamp_ms": 1001, "mvr_frame_number": 11,
                                name: value}
                after = self.snapshot(**after_values)
                result = MODULE.classify(before, after, MODULE.Profile.EXTENDED)
                self.assertTrue(result.changed_mask & field)


if __name__ == "__main__":
    unittest.main()
