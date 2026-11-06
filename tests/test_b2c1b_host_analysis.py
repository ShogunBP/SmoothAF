#!/usr/bin/env python3
"""Deterministic host-only B2-C1b robustness tests."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2c1b_host_analysis.py"
SPEC = importlib.util.spec_from_file_location("b2c1b_host_analysis", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class B2C1BHostAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.source, cls.raw, cls.rows, cls.errors = MODULE._load_canonical()
        cls.report = MODULE.build_report(cls.source, cls.raw, cls.rows, cls.errors)

    def test_reuses_canonical_parser_and_preserves_source(self) -> None:
        self.assertEqual(self.errors, [])
        self.assertEqual(len(self.rows), 367)
        self.assertEqual(cls_sha256(self.source),
                         "E08D0D90E6BA6791CDC36A5C5CFBE614FCDAD1AF9614B240151132D4448B36E7")

    def test_required_ep7_and_ep8_regressions(self) -> None:
        regressions = self.report["regressions"]
        self.assertEqual(regressions["ep7_position_0_absolute_delta"], 575)
        self.assertEqual(regressions["ep8_position_0_absolute_delta"], 520)
        self.assertIn("tail-3 medians", regressions["definition"])
        self.assertIn("non-AUTO", regressions["definition"])

    def test_estimator_definitions_and_insufficient_trimmed_mean(self) -> None:
        definitions = self.report["estimator_definitions"]
        self.assertIn("maximum individual", definitions["strict_max"])
        self.assertIn("last three", definitions["tail3_median"])
        self.assertIn("at least five", definitions["trimmed_mean"])
        self.assertEqual(self.report["margin_357_status"],
                         "INSUFFICIENT_EVIDENCE_UNCALIBRATED_GENERAL_HOLD_MARGIN")
        ep2_trimmed = self.report["estimators"]["2"]["trimmed_mean"]
        self.assertTrue(any(item["status"] == "INSUFFICIENT_EVIDENCE"
                            for item in ep2_trimmed["positions"].values()))

    def test_leave_one_out_and_duplicate_sensitivity_are_reported(self) -> None:
        sensitivity = self.report["sensitivity"]
        for episode in map(str, range(2, 10)):
            for policy in MODULE.POLICIES:
                data = sensitivity[episode][policy]
                self.assertIn("leave_one_out", data)
                self.assertIn("remove_repeated_metric", data)
                self.assertIn("single_measurement_dominated", data)

    def test_repeatability_is_a_proxy_and_three_timestamps_are_not_frames(self) -> None:
        repeatability = self.report["repeatability"]
        self.assertEqual(repeatability["7"]["0"]["tail3_absolute_delta"], 575)
        self.assertEqual(repeatability["8"]["0"]["tail3_absolute_delta"], 520)
        self.assertTrue(all("proxy" in item["proxy_status"].lower()
                            or item["proxy_status"] == "INSUFFICIENT_EVIDENCE"
                            for episode in repeatability.values()
                            for item in episode.values()))
        self.assertTrue(any("Different timestamps do not prove" in limitation
                            for limitation in self.report["limitations"]))

    def test_causal_mirroring_scaling_and_mutation_properties(self) -> None:
        properties = self.report["property_results"]
        self.assertTrue(properties["causality_prefix"]["passed"])
        self.assertTrue(properties["mirroring"]["passed"])
        self.assertTrue(properties["scaling"]["passed"])
        self.assertTrue(properties["duplicate_support"]["passed"])
        self.assertTrue(properties["leave_one_out"]["passed"])
        self.assertTrue(all(item["killed"] for item in properties["mutation_sentinels"].values()))

    def test_synthetic_positive_and_negative_cases(self) -> None:
        cases = self.report["synthetic_cases"]
        positive = cases["well_defined_peak"]["complete_median"]
        self.assertEqual(positive["winner"]["position"], 0)
        self.assertTrue(any(item["status"] == "PEAK_CANDIDATE" for item in positive["peaks"]))
        self.assertEqual(cases["missing_neighbor"]["complete_median"]["peaks"], [])
        self.assertEqual(cases["low_confidence"]["complete_median"]["winner"]["status"],
                         "INSUFFICIENT_EVIDENCE")
        self.assertTrue(all(item["status"] == "NON_DISCRIMINATED"
                            for item in cases["outlier_isolated"]["complete_median"]["peaks"]))


def cls_sha256(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes()).hexdigest().upper()


if __name__ == "__main__":
    unittest.main()
