#!/usr/bin/env python3
"""Deterministic host-only regression checks for the B2-B harness."""
from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "b2b_host_analysis.py"
SPEC = importlib.util.spec_from_file_location("b2b_host_analysis", MODULE_PATH)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = MODULE
SPEC.loader.exec_module(MODULE)


class B2BHostAnalysisTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        rows, errors = MODULE.parse_log(ROOT / "logs" / "SMTH003.LOG")
        cls.assert_no_parse_errors = errors
        cls.episodes = MODULE.build_episodes(rows)
        cls.calibration = MODULE.calibrate(cls.episodes)

    def test_smth003_reconstructs_eight_af_episodes(self) -> None:
        self.assertEqual(self.assert_no_parse_errors, [])
        self.assertEqual([episode.episode for episode in self.episodes], [2, 3, 4, 5, 6, 7, 8, 9])
        by_id = {episode.episode: episode for episode in self.episodes}
        self.assertEqual(by_id[2].logged_best_position, -3)
        self.assertEqual(by_id[5].logged_final_score, 46476)
        self.assertEqual(by_id[8].terminal_result, 6)
        self.assertEqual(by_id[8].limit[0], 2)

    def test_calibration_uses_only_episode_two_and_five(self) -> None:
        self.assertEqual(self.calibration.within_visit_mad, 119)
        self.assertEqual(self.calibration.repeated_position_delta, 187)
        self.assertEqual(self.calibration.direction_arrival_delta, 187)
        self.assertEqual(self.calibration.peak_margin, 357)

    def test_frozen_replay_does_not_confirm_episode_eight_early(self) -> None:
        results = MODULE.strategy_results(self.episodes, self.calibration)
        ep8 = [result for result in results if result.episode == 8 and result.strategy != "A"]
        self.assertTrue(ep8)
        self.assertTrue(all(result.result == "HOLD_CANDIDATE" for result in ep8))
        self.assertTrue(all(result.contrafactual for result in ep8))
        self.assertTrue(all(not result.budget_violation for result in ep8))

    def test_adversarial_cases_have_no_false_converged(self) -> None:
        cases = MODULE.adversarial_cases(self.calibration)
        self.assertTrue(cases)
        self.assertEqual(sum(case["false_converged"] for case in cases), 0)


if __name__ == "__main__":
    unittest.main()
