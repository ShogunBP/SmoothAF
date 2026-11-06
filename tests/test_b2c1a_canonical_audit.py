#!/usr/bin/env python3
"""Deterministic integrity tests for the B2-C1a canonical audit."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2c1a_canonical_audit.py"
SPEC = importlib.util.spec_from_file_location("b2c1a_canonical_audit", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class B2C1ACanonicalAuditTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.raw, cls.errors = MODULE.parse_log(ROOT / "logs" / "SMTH003.LOG")
        cls.rows = MODULE.canonical_rows(cls.raw)

    def test_parser_preserves_physical_order_and_equal_timestamps(self) -> None:
        self.assertEqual(self.errors, [])
        self.assertEqual([row.line for row in self.rows], list(range(1, 368)))
        equal_timestamp_lines = [row.line for row in self.rows if row.timestamp == 319193]
        self.assertEqual(equal_timestamp_lines, [76, 77])
        self.assertEqual([row.event for row in self.rows if row.line in equal_timestamp_lines],
                         ["MEASUREMENT", "AUTO_PROBE"])

    def test_log_has_no_frame_identity_and_fourth_payload_is_laplacian(self) -> None:
        measurement = next(row for row in self.rows if row.event == "MEASUREMENT")
        self.assertEqual(measurement.fourth_field, 14759)
        self.assertEqual(measurement.fourth_field_semantics, "laplacian")
        self.assertEqual(
            {row.duplicate_flags for row in self.rows if row.duplicate_flags},
            {"exact_metric_tuple_seen_before;distinct_timestamp_same_tuple"},
        )

    def test_positions_visits_and_environmental_events_are_reconstructed(self) -> None:
        positions = [row.logical_position for row in self.rows
                     if row.episode == 8 and row.event == "COMMAND"]
        self.assertEqual(positions, [-3, 0, 3, 0, -3, 0, 3, 0])
        self.assertEqual([row.line for row in self.rows if row.event == "ENVIRONMENTAL"],
                         [140, 149, 227, 365])
        self.assertEqual([visit.command_index for visit in MODULE.visits(self.rows, 8)],
                         list(range(8)))

    def test_formula_reproduces_b2b_without_retroactive_selection(self) -> None:
        result = MODULE.calibration(self.rows)
        self.assertEqual(result["within_visit_mad_max"], 119)
        self.assertEqual(result["repeated_position_delta_max"], 187)
        self.assertEqual(result["direction_arrival_delta_max"], 187)
        self.assertEqual(result["peak_margin"], 357)
        self.assertEqual(result["three_times_mad"], 357)

    def test_ep2_best_candidates_and_selection_policies_are_visible(self) -> None:
        ep2 = [v for v in MODULE.visits(self.rows, 2) if v.command_index == 4][0]
        self.assertEqual(ep2.scores, (21116, 22187, 21198))
        self.assertEqual(MODULE.policy_summary(ep2, "all").amplitude, 1071)
        self.assertEqual(MODULE.policy_summary(ep2, "harness_tail3").median, 21198)
        self.assertEqual(MODULE.policy_summary(ep2, "harness_exclude_first_tail3").median, 21198)

    def test_requested_visit_deltas_preserve_ep7_and_ep8_statistics(self) -> None:
        comparisons = MODULE.pair_comparisons(self.rows)
        self.assertEqual(comparisons["7"]["0"]["absolute_delta"], 575)
        self.assertAlmostEqual(comparisons["7"]["0"]["relative_delta"], 0.04964385927045111)
        self.assertEqual(comparisons["8"]["0"]["absolute_delta"], 520)
        self.assertAlmostEqual(comparisons["8"]["0"]["relative_delta"], 0.04475043029259897)

    def test_ep8_first_causal_opportunity_does_not_read_future_commands(self) -> None:
        replay = MODULE.replay_ep8(self.rows)
        hypothesis_a = replay[0]
        self.assertEqual(hypothesis_a.classification, "REPLAY_FIEL_COM_PARADA_ANTECIPADA")
        self.assertEqual(hypothesis_a.decision_line, 321)
        self.assertEqual(hypothesis_a.timestamp, 358085)
        self.assertEqual(hypothesis_a.last_command_executed, 3)
        self.assertEqual(hypothesis_a.first_command_not_executed, 4)
        self.assertEqual(hypothesis_a.avoided_commands, (4, 5, 6, 7))

        prefix = tuple(row for row in self.rows if row.line <= hypothesis_a.decision_line)
        prefix_replay = MODULE.replay_ep8(prefix)[0]
        self.assertEqual(prefix_replay.classification, hypothesis_a.classification)
        self.assertEqual(prefix_replay.decision_line, hypothesis_a.decision_line)
        self.assertEqual(prefix_replay.timestamp, hypothesis_a.timestamp)
        self.assertEqual(prefix_replay.last_command_executed, hypothesis_a.last_command_executed)

    def test_reconstruction_is_deterministic_and_score_luminance_is_descriptive(self) -> None:
        again = MODULE.canonical_rows(self.raw)
        self.assertEqual(again, self.rows)
        data = MODULE.score_luminance(self.rows)
        self.assertEqual(data["all_episode_measurements"]["count"], 129)
        self.assertIsNotNone(data["all_episode_measurements"]["temporal_pearson_score_luminance"])
        d1 = MODULE.d1_summary(self.raw)
        self.assertEqual(d1["4"]["count"], 30)
        self.assertEqual(d1["5"]["count"], 30)


if __name__ == "__main__":
    unittest.main()
