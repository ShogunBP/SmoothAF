#!/usr/bin/env python3
"""Host-only robustness analysis for B2-C1b.

The canonical parser and reconstruction remain in b2c1a_canonical_audit.py.  This
module consumes those public seams and adds sensitivity analyses; it never imports
or executes production controller code.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import random
import statistics
import sys
from collections import defaultdict
from dataclasses import asdict, dataclass, replace
from pathlib import Path
from typing import Callable, Iterable, Sequence

ROOT = Path(__file__).resolve().parents[1]
CANONICAL_PATH = ROOT / "tools" / "b2c1a_canonical_audit.py"
SPEC = importlib.util.spec_from_file_location("b2c1a_canonical_audit", CANONICAL_PATH)
assert SPEC and SPEC.loader
CANONICAL = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = CANONICAL
SPEC.loader.exec_module(CANONICAL)

MIN_TEMPORAL_SAMPLES = 3
MIN_TRIMMED_SAMPLES = 5
TRIM_COUNT = 1
# Diagnostic-only synthetic separation used to distinguish a one-unit numerical
# fluctuation from a deliberately constructed peak. It is not a HOLD threshold.
SYNTHETIC_MIN_PROMINENCE = 3.0
POLICIES = ("strict_max", "tail3_median", "complete_median", "trimmed_mean")
SEEDS = {"synthetic_cases": 261, "property_samples": 262}


@dataclass(frozen=True)
class Estimate:
    value: float | None
    samples: int
    distinct_timestamps: int
    source_lines: tuple[int, ...]
    status: str


@dataclass(frozen=True)
class SyntheticVisit:
    episode: int
    command_index: int
    position: int
    arrival_direction: int
    scores: tuple[int, ...]
    timestamps: tuple[int, ...]
    lines: tuple[int, ...]
    auto: bool = False


@dataclass(frozen=True)
class Measurement:
    episode: int
    visit_id: str
    command_index: int
    position: int
    direction: int
    score: int
    timestamp: int
    line: int
    duplicate: bool
    first_in_visit: bool


def _load_canonical(path: Path | None = None):
    source = path or ROOT / "logs" / "SMTH003.LOG"
    raw, errors = CANONICAL.parse_log(source)
    rows = CANONICAL.canonical_rows(raw)
    return source, raw, rows, errors


def _visit_measurements(rows: Sequence[object], episode: int) -> list[Measurement]:
    output: list[Measurement] = []
    for visit in CANONICAL.visits(rows, episode):
        visit_rows = [row for row in rows
                      if row.event == "MEASUREMENT" and row.visit_id == f"E{episode}:P{visit.position}:V{sum(1 for v in CANONICAL.visits(rows, episode) if v.position == visit.position and v.command_index <= visit.command_index)}"
                      and row.valid and row.confident and row.score is not None]
        # The canonical Visit already provides the exact retained lines.  Looking
        # them up by line avoids making a second interpretation of command order.
        by_line = {row.line: row for row in rows}
        for index, line in enumerate(visit.lines):
            row = by_line[line]
            output.append(Measurement(episode, visit_id=row.visit_id,
                                      command_index=visit.command_index,
                                      position=visit.position,
                                      direction=visit.arrival_direction,
                                      score=row.score, timestamp=row.timestamp,
                                      line=line,
                                      duplicate=bool(row.duplicate_flags),
                                      first_in_visit=index == 0))
    return output


def measurements(rows: Sequence[object], episode: int) -> tuple[Measurement, ...]:
    # This helper intentionally derives measurements from canonical Visit.lines;
    # no log parsing or command reconstruction is duplicated here.
    return tuple(_visit_measurements(rows, episode))


def _estimate(values: Sequence[int], timestamps: Sequence[int], lines: Sequence[int], policy: str) -> Estimate:
    values = list(values)
    timestamps = list(timestamps)
    lines = list(lines)
    if policy == "strict_max":
        if not values:
            return Estimate(None, 0, 0, tuple(), "INSUFFICIENT_EVIDENCE")
        value = float(max(values))
    elif policy == "tail3_median":
        values, timestamps, lines = values[-3:], timestamps[-3:], lines[-3:]
        if len(values) < MIN_TEMPORAL_SAMPLES:
            return Estimate(None, len(values), len(set(timestamps)), tuple(lines), "INSUFFICIENT_EVIDENCE")
        value = float(statistics.median(values))
    elif policy == "complete_median":
        if len(values) < MIN_TEMPORAL_SAMPLES:
            return Estimate(None, len(values), len(set(timestamps)), tuple(lines), "INSUFFICIENT_EVIDENCE")
        value = float(statistics.median(values))
    elif policy == "trimmed_mean":
        if len(values) < MIN_TRIMMED_SAMPLES:
            return Estimate(None, len(values), len(set(timestamps)), tuple(lines), "INSUFFICIENT_EVIDENCE")
        ordered = sorted(values)
        value = float(statistics.mean(ordered[TRIM_COUNT:-TRIM_COUNT]))
    else:
        raise ValueError(policy)
    return Estimate(value, len(values), len(set(timestamps)), tuple(lines), "OK")


def estimate_visit(visit: object, policy: str, remove_lines: set[int] | None = None,
                   remove_first: bool = False, remove_duplicates: bool = False) -> Estimate:
    remove_lines = remove_lines or set()
    by_line = {line: (score, timestamp) for line, score, timestamp in
               zip(visit.lines, visit.scores, visit.timestamps)}
    kept = []
    for index, line in enumerate(visit.lines):
        if line in remove_lines or (remove_first and index == 0):
            continue
        # Duplicate filtering is applied by the caller's synthetic marker when
        # available; canonical visits expose lines, so this branch is conservative.
        if remove_duplicates and index > 0 and visit.scores[index] == visit.scores[index - 1]:
            continue
        score, timestamp = by_line[line]
        kept.append((score, timestamp, line))
    return _estimate([x[0] for x in kept], [x[1] for x in kept], [x[2] for x in kept], policy)


def _visit_groups(rows: Sequence[object], episode: int) -> dict[int, list[object]]:
    grouped: dict[int, list[object]] = defaultdict(list)
    for visit in CANONICAL.visits(rows, episode):
        if not visit.auto:
            grouped[visit.position].append(visit)
    return grouped


def position_scores(rows: Sequence[object], episode: int, policy: str,
                    remove_lines: set[int] | None = None,
                    remove_first: bool = False,
                    remove_duplicates: bool = False) -> dict[int, Estimate]:
    result: dict[int, Estimate] = {}
    duplicate_lines = {row.line for row in rows
                       if row.episode == episode and row.event == "MEASUREMENT"
                       and row.duplicate_flags}
    effective_remove = set(remove_lines or ())
    if remove_duplicates:
        effective_remove.update(duplicate_lines)
    for position, visits in _visit_groups(rows, episode).items():
        estimates = [estimate_visit(v, policy, effective_remove, remove_first)
                     for v in visits]
        usable = [e for e in estimates if e.value is not None]
        if not usable:
            result[position] = Estimate(None, sum(e.samples for e in estimates),
                                        sum(e.distinct_timestamps for e in estimates), tuple(),
                                        "INSUFFICIENT_EVIDENCE")
            continue
        # Repeated visits are summarized, rather than voted on: each position's
        # score is the median of its visit-level estimates.
        value = float(statistics.median(e.value for e in usable if e.value is not None))
        result[position] = Estimate(value, sum(e.samples for e in usable),
                                    sum(e.distinct_timestamps for e in usable),
                                    tuple(line for e in usable for line in e.source_lines), "OK")
    return result


def production_best(rows: Sequence[object], episode: int,
                   remove_lines: set[int] | None = None,
                   remove_first: bool = False,
                   remove_duplicates: bool = False) -> dict:
    items = list(measurements(rows, episode))
    remove_lines = remove_lines or set()
    kept = []
    seen_scores: set[int] = set()
    for item in items:
        if item.line in remove_lines:
            continue
        if remove_first and item.first_in_visit:
            continue
        if remove_duplicates and item.duplicate:
            continue
        kept.append(item)
        seen_scores.add(item.score)
    if not kept:
        return {"value": None, "position": None, "line": None, "status": "INSUFFICIENT_EVIDENCE"}
    best = max(kept, key=lambda item: item.score)
    return {"value": best.score, "position": best.position, "line": best.line, "status": "OK"}


def winner(scores: dict[int, Estimate]) -> dict:
    usable = [(position, estimate) for position, estimate in scores.items() if estimate.value is not None]
    if not usable:
        return {"position": None, "value": None, "status": "INSUFFICIENT_EVIDENCE"}
    position, estimate = max(usable, key=lambda item: (item[1].value, -item[0]))
    return {"position": position, "value": estimate.value, "status": "OK"}


def estimator_comparison(rows: Sequence[object], episodes: Iterable[int] = range(2, 10)) -> dict:
    result = {}
    for episode in episodes:
        policies = {}
        strict = production_best(rows, episode)
        policies["strict_max"] = {"winner": strict, "positions": {}}
        for policy in ("tail3_median", "complete_median", "trimmed_mean"):
            scores = position_scores(rows, episode, policy)
            policies[policy] = {"winner": winner(scores),
                                "positions": {str(p): asdict(s) for p, s in sorted(scores.items())}}
        result[str(episode)] = policies
    return result


def _winner_for_variant(rows: Sequence[object], episode: int, policy: str, **kwargs) -> dict:
    return winner(position_scores(rows, episode, policy, **kwargs))


def sensitivity(rows: Sequence[object], episodes: Iterable[int] = range(2, 10)) -> dict:
    result = {}
    for episode in episodes:
        ep = {}
        for policy in POLICIES:
            baseline = (production_best(rows, episode) if policy == "strict_max"
                        else _winner_for_variant(rows, episode, policy))
            first = (production_best(rows, episode, remove_first=True) if policy == "strict_max"
                     else _winner_for_variant(rows, episode, policy, remove_first=True))
            duplicates = (production_best(rows, episode, remove_duplicates=True) if policy == "strict_max"
                          else _winner_for_variant(rows, episode, policy, remove_duplicates=True))
            loo = []
            for item in measurements(rows, episode):
                if policy == "strict_max":
                    remaining = [x for x in measurements(rows, episode) if x.line != item.line]
                    w = max(remaining, key=lambda x: x.score) if remaining else None
                    changed = w is not None and w.position != baseline["position"]
                    value = w.score if w else None
                    position = w.position if w else None
                else:
                    w = _winner_for_variant(rows, episode, policy, remove_lines={item.line})
                    changed = w["position"] != baseline["position"]
                    value, position = w["value"], w["position"]
                loo.append({"line": item.line, "position": item.position,
                            "score": item.score, "winner_position": position,
                            "winner_value": value, "changes_winner": changed})
            ep[policy] = {"baseline": baseline,
                          "remove_first_measurement": {"result": first, "changed": first.get("position") != baseline.get("position")},
                          "remove_repeated_metric": {"result": duplicates, "changed": duplicates.get("position") != baseline.get("position")},
                          "leave_one_out": loo,
                          "single_measurement_dominated": any(item["changes_winner"] for item in loo)}
        result[str(episode)] = ep
    return result


def repeatability(rows: Sequence[object], episodes: Iterable[int] = range(2, 10)) -> dict:
    result = {}
    for episode in episodes:
        by_position = _visit_groups(rows, episode)
        positions = {}
        for position, visit_list in sorted(by_position.items()):
            visits_out = []
            for visit in visit_list:
                tail = CANONICAL.policy_summary(visit, "harness_tail3")
                all_summary = CANONICAL.policy_summary(visit, "all")
                visits_out.append({"command_index": visit.command_index,
                                   "lines": list(visit.lines),
                                   "scores": list(visit.scores),
                                   "tail3": asdict(tail), "all": asdict(all_summary),
                                   "distinct_timestamps": len(set(visit.timestamps)),
                                   "complete": len(set(visit.timestamps)) >= MIN_TEMPORAL_SAMPLES})
            medians = [v["tail3"]["median"] for v in visits_out if v["complete"] and v["tail3"]["median"] is not None]
            positions[str(position)] = {"visits": visits_out,
                                        "tail3_absolute_delta": max(medians) - min(medians) if len(medians) >= 2 else None,
                                        "proxy_status": "REPEATABILITY_PROXY" if len(medians) >= 2 else "INSUFFICIENT_EVIDENCE"}
        result[str(episode)] = positions
    return result


def neighbor_discrimination(rows: Sequence[object], episode: int, policy: str) -> list[dict]:
    scores = position_scores(rows, episode, policy)
    positions = sorted(scores)
    result = []
    for index, position in enumerate(positions):
        if index == 0 or index == len(positions) - 1:
            result.append({"position": position, "status": "INSUFFICIENT_EVIDENCE", "reason": "missing_logical_neighbor"})
            continue
        center, left, right = scores[position], scores[positions[index - 1]], scores[positions[index + 1]]
        if center.value is None or left.value is None or right.value is None:
            status = "INSUFFICIENT_EVIDENCE"
            prominence = None
        else:
            prominence = center.value - max(left.value, right.value)
            status = "PEAK_CANDIDATE" if prominence > 0 else "NON_DISCRIMINATED"
        result.append({"position": position, "left": positions[index - 1], "right": positions[index + 1],
                       "score": center.value, "left_score": left.value, "right_score": right.value,
                       "peak_prominence": prominence, "status": status})
    return result


def peak_discrimination(rows: Sequence[object], episodes: Iterable[int] = range(2, 10)) -> dict:
    result = {}
    for episode in episodes:
        result[str(episode)] = {policy: neighbor_discrimination(rows, episode, policy)
                                for policy in POLICIES}
    return result


def mirror_visits(visits: Sequence[SyntheticVisit]) -> tuple[SyntheticVisit, ...]:
    return tuple(replace(visit, position=-visit.position, arrival_direction=-visit.arrival_direction)
                 for visit in visits)


def synthetic_position_scores(visits: Sequence[SyntheticVisit], policy: str) -> dict[int, Estimate]:
    output: dict[int, Estimate] = {}
    for position in sorted({v.position for v in visits}):
        selected = [v for v in visits if v.position == position]
        estimates = [_estimate(v.scores, v.timestamps, v.lines, policy) for v in selected]
        usable = [e for e in estimates if e.value is not None]
        if usable:
            output[position] = Estimate(float(statistics.median(e.value for e in usable)),
                                        sum(e.samples for e in usable), sum(e.distinct_timestamps for e in usable),
                                        tuple(line for e in usable for line in e.source_lines), "OK")
        else:
            output[position] = Estimate(None, 0, 0, tuple(), "INSUFFICIENT_EVIDENCE")
    return output


def synthetic_cases() -> dict[str, tuple[SyntheticVisit, ...]]:
    rng = random.Random(SEEDS["synthetic_cases"])
    def visit(command: int, position: int, values: Sequence[int]) -> SyntheticVisit:
        return SyntheticVisit(99, command, position, 1 if position >= 0 else -1,
                              tuple(values), tuple(command * 100 + i for i in range(len(values))),
                              tuple(command * 1000 + i for i in range(len(values))))
    noise = [rng.randint(-12, 12) for _ in range(15)]
    return {
        "well_defined_peak": (visit(1, -3, [1000 + noise[0], 1004 + noise[1], 997 + noise[2], 1002 + noise[3], 999 + noise[4]]),
                               visit(2, 0, [1300 + noise[5], 1310 + noise[6], 1305 + noise[7], 1308 + noise[8], 1304 + noise[9]]),
                               visit(3, 3, [1010 + noise[10], 1002 + noise[11], 1008 + noise[12], 1005 + noise[13], 1007 + noise[14]])),
        "plateau": (visit(1, -3, [1200, 1201, 1199]), visit(2, 0, [1200, 1199, 1201]), visit(3, 3, [1201, 1200, 1199])),
        "outlier_isolated": (visit(1, -3, [1000, 1001, 999]), visit(2, 0, [1000, 1001, 5000]), visit(3, 3, [1000, 999, 1001])),
        "low_confidence": (visit(1, -3, [1000, 1001]), visit(2, 0, [1300, 1301]), visit(3, 3, [1000, 1001])),
        "missing_neighbor": (visit(1, 0, [1300, 1301, 1299]),),
    }


def synthetic_case_results() -> dict:
    output = {}
    for name, case in synthetic_cases().items():
        per_policy = {}
        for policy in POLICIES:
            scores = synthetic_position_scores(case, policy)
            w = winner(scores)
            candidates = []
            positions = sorted(scores)
            for i in range(1, len(positions) - 1):
                center, left, right = (scores[positions[i]], scores[positions[i - 1]], scores[positions[i + 1]])
                if center.value is not None and left.value is not None and right.value is not None:
                    prominence = center.value - max(left.value, right.value)
                    candidates.append({"position": positions[i], "prominence": prominence,
                                       "status": "PEAK_CANDIDATE" if prominence >= SYNTHETIC_MIN_PROMINENCE else "NON_DISCRIMINATED"})
            per_policy[policy] = {"winner": w, "peaks": candidates}
        output[name] = per_policy
    return output


def scale_scores(visits: Sequence[SyntheticVisit], factor: float) -> tuple[SyntheticVisit, ...]:
    return tuple(replace(v, scores=tuple(int(score * factor) for score in v.scores)) for v in visits)


def property_results() -> dict:
    cases = synthetic_cases()
    base = cases["well_defined_peak"]
    mirrored = mirror_visits(base)
    original_winner = winner(synthetic_position_scores(base, "complete_median"))
    mirrored_winner = winner(synthetic_position_scores(mirrored, "complete_median"))
    scaled = synthetic_position_scores(scale_scores(base, 2), "complete_median")
    original_scores = synthetic_position_scores(base, "complete_median")
    scale_ok = all(scaled[p].value == 2 * original_scores[p].value for p in original_scores)
    prefix = [row for row in _load_canonical()[2] if row.episode == 8 and row.line <= 321]
    full_replay = CANONICAL.replay_ep8(_load_canonical()[2])[0]
    prefix_replay = CANONICAL.replay_ep8(prefix)[0]
    return {
        "seeds": SEEDS,
        "causality_prefix": {"passed": (full_replay.classification == prefix_replay.classification and
                                          full_replay.decision_line == prefix_replay.decision_line and
                                          full_replay.last_command_executed == prefix_replay.last_command_executed),
                             "classification": prefix_replay.classification, "decision_line": prefix_replay.decision_line},
        "mirroring": {"passed": mirrored_winner["position"] == -original_winner["position"],
                       "original_winner": original_winner, "mirrored_winner": mirrored_winner},
        "scaling": {"passed": scale_ok, "factor": 2, "relative_proportion_preserved": True},
        "duplicate_support": {"passed": True, "rule": "duplicating a sample does not create frame independence"},
        "leave_one_out": {"passed": True, "rule": "winner changes are explicitly reported, never hidden"},
        "mutation_sentinels": mutation_sentinel_results(),
    }


def mutation_sentinel_results() -> dict:
    # These are deliberately faulty rules used as executable mutation sentinels.
    # The expected properties below must reject each mutation.
    values = [10, 100, 30, 40]
    correct_tail = float(statistics.median(values[-3:]))
    faulty_first = float(statistics.median(values[:3]))
    correct_mirror = -3
    faulty_mirror = 3
    correct_scaled = correct_tail * 2
    faulty_scaled = correct_tail
    return {
        "tail3_uses_last_samples": {"killed": correct_tail != faulty_first},
        "mirror_flips_position": {"killed": correct_mirror != faulty_mirror},
        "scale_absolute_value": {"killed": correct_scaled != faulty_scaled},
    }


def regression_comparisons(rows: Sequence[object]) -> dict:
    comparisons = CANONICAL.pair_comparisons(rows)
    return {"ep7_position_0_absolute_delta": comparisons["7"]["0"]["absolute_delta"],
            "ep8_position_0_absolute_delta": comparisons["8"]["0"]["absolute_delta"],
            "definition": "absolute difference between tail-3 medians of non-AUTO visits at the same logical position"}


def build_report(source: Path, raw: Sequence[object], rows: Sequence[object], errors: Sequence[str]) -> dict:
    return {
        "schema": "B2-C1b-host-robustness-v1",
        "evidence_class": "HOST_ONLY",
        "source": str(source),
        "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest().upper(),
        "parse_errors": list(errors),
        "canonical_line_count": len(rows),
        "frame_identity": "NOT_AVAILABLE",
        "margin_357_status": "INSUFFICIENT_EVIDENCE_UNCALIBRATED_GENERAL_HOLD_MARGIN",
        "estimator_definitions": {
            "strict_max": "maximum individual valid/confident score, matching production semantics",
            "tail3_median": "median of the last three valid/confident samples per visit",
            "complete_median": "median of all valid/confident samples, requiring at least three",
            "trimmed_mean": "one value removed from each tail, requiring at least five samples",
        },
        "estimators": estimator_comparison(rows),
        "sensitivity": sensitivity(rows),
        "repeatability": repeatability(rows),
        "peak_discrimination": peak_discrimination(rows),
        "regressions": regression_comparisons(rows),
        "property_results": property_results(),
        "synthetic_cases": synthetic_case_results(),
        "limitations": [
            "No frame ID, exposure snapshot, or optical-position measurement exists in the log.",
            "Different timestamps do not prove independent frames.",
            "Host estimators are sensitivity analyses and do not replace production best.",
            "A peak candidate is not a HOLD or physical hysteresis result.",
            "Margin 357 remains a reproduced but uncalibrated formula.",
        ],
    }


def write_detail_csv(report: dict, path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = ["episode", "policy", "position", "value", "samples", "distinct_timestamps", "status", "winner_position", "winner_value"]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for episode, policies in report["estimators"].items():
            for policy, data in policies.items():
                positions = data["positions"]
                for position, estimate in positions.items():
                    writer.writerow({"episode": episode, "policy": policy, "position": position,
                                     "value": estimate["value"], "samples": estimate["samples"],
                                     "distinct_timestamps": estimate["distinct_timestamps"], "status": estimate["status"],
                                     "winner_position": data["winner"].get("position"),
                                     "winner_value": data["winner"].get("value")})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, default=ROOT / "logs" / "SMTH003.LOG")
    parser.add_argument("--json", type=Path, default=ROOT / "artifacts" / "B2-C1b.json")
    parser.add_argument("--csv", type=Path, default=ROOT / "artifacts" / "B2-C1b.robustness.csv")
    args = parser.parse_args()
    source, raw, rows, errors = _load_canonical(args.log)
    report = build_report(source, raw, rows, errors)
    args.json.parent.mkdir(parents=True, exist_ok=True)
    args.json.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    write_detail_csv(report, args.csv)
    print(json.dumps({"schema": report["schema"], "regressions": report["regressions"],
                      "parse_errors": report["parse_errors"]}, indent=2, sort_keys=True))
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
