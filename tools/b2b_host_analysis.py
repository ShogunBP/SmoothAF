#!/usr/bin/env python3
"""Host-only SmoothAF V2 B2-B replay and convergence analysis.

This tool is intentionally independent of production controller code. It reads
SMTH003.LOG, reconstructs the logged logical trajectory, and evaluates pure
bracket/HOLD predicates against already recorded samples. It never invents a
future camera sample and marks any changed trajectory as contrafactual.

SIMULATION: all strategy results and adversarial results are host analysis, not
camera validation. Constants are calibrated only from episodes 2 and 5 of the
same SMTH003 session and are explicitly NOT_CALIBRATED_ON_T3I.
"""
from __future__ import annotations

import argparse
import json
import math
import statistics
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Iterable


TIMEOUT_MS = 8000
# SMTH003 SESSION_START configured four algorithm commands and AUTO adds four
# bounded probe commands. Recovery was not reserved in these episodes.
CONFIGURED_ALGORITHM_BUDGET = 4
AUTO_BUDGET = 4
CONFIGURED_EPISODE_BUDGET = CONFIGURED_ALGORITHM_BUDGET + AUTO_BUDGET
# Production's absolute episode cap is algorithm + AUTO + recovery, capped at 24.
MAX_EPISODE_BUDGET = 24
CALIBRATION_EPISODES = (2, 5)
EVALUATION_EPISODES = (7, 8)

# NOT_CALIBRATED_ON_T3I: derived from SMTH003 episodes 2 and 5 only.
MIN_TEMPORAL_SAMPLES = 3
# The persistent logger has no frame identity. Distinct timestamps are only a
# temporal proxy; equal metric tuples are reported as a diagnostic, not silently
# promoted to independent optical evidence.


@dataclass(frozen=True)
class Sample:
    timestamp: int
    score: int
    mean: int
    valid: int
    confident: int


@dataclass(frozen=True)
class Command:
    index: int
    timestamp: int
    direction: int
    step: int
    lens_ms: int
    settle_ms: int
    sample_wait_ms: int
    position: int
    auto: bool
    samples: tuple[Sample, ...]

    @property
    def last_timestamp(self) -> int:
        return self.samples[-1].timestamp if self.samples else self.timestamp

    @property
    def elapsed(self) -> int:
        return self.last_timestamp - self.timestamp

    @property
    def terminal_score(self) -> int | None:
        return self.samples[-1].score if self.samples else None

    @property
    def temporal_samples(self) -> tuple[Sample, ...]:
        return tuple(s for s in self.samples if s.valid and s.confident)


@dataclass(frozen=True)
class Episode:
    episode: int
    start_timestamp: int
    baseline: int
    end_timestamp: int
    terminal_result: int
    logged_best_score: int
    logged_best_position: int
    logged_final_score: int
    logged_final_position: int
    logged_commands: int
    commands: tuple[Command, ...]
    auto_probe_decision_timestamp: int | None
    auto_probe: tuple[tuple[int, int, int, int, int], ...]
    environmental: tuple[tuple[int, ...], ...]
    limit: tuple[int, ...] | None
    reference: tuple[int, ...] | None

    @property
    def duration_ms(self) -> int:
        return self.end_timestamp - self.start_timestamp

    @property
    def decision_timestamp(self) -> int:
        """Recorded terminal decision, not the intermediate AUTO decision."""
        return self.end_timestamp

    @property
    def auto_decision_ms(self) -> int | None:
        if self.auto_probe_decision_timestamp is None:
            return None
        return self.auto_probe_decision_timestamp - self.start_timestamp

    @property
    def decision_ms(self) -> int:
        return self.decision_timestamp - self.start_timestamp

    @property
    def inversions(self) -> int:
        return sum(
            a.direction != b.direction
            for a, b in zip(self.commands, self.commands[1:])
        )


@dataclass(frozen=True)
class Calibration:
    within_visit_mad: float
    repeated_position_delta: float
    direction_arrival_delta: float
    peak_margin: float


@dataclass(frozen=True)
class Candidate:
    position: int
    score: float
    left_score: float
    right_score: float
    margin: float
    support: int
    decision_command: int
    decision_timestamp: int
    parabola_vertex: float | None


@dataclass(frozen=True)
class StrategyResult:
    strategy: str
    episode: int
    commands_to_decision: int
    time_to_decision_ms: int
    timeout_margin_ms: int
    best_candidate: int | None
    final_position: int
    inversions: int
    result: str
    confidence: str
    insufficient_data: bool
    budget_violation: bool
    contrafactual: bool
    reason: str


def parse_log(path: Path) -> tuple[dict[int, dict], list[str]]:
    rows: dict[int, dict] = {}
    errors: list[str] = []
    lines = path.read_text(encoding="ascii").splitlines()
    for line_no, line in enumerate(lines, 1):
        fields = line.split("\t")
        if len(fields) != 9:
            errors.append(f"line {line_no}: expected 9 fields")
            continue
        try:
            timestamp, event, episode = int(fields[0]), fields[1], int(fields[2])
            payload = tuple(int(v) for v in fields[3:])
        except ValueError as exc:
            errors.append(f"line {line_no}: {exc}")
            continue
        if event in {"D1_SERIES_START", "D1_SAMPLE", "D1_SERIES_END", "D1_CONTEXT", "D1_CONTEXT_AUX"}:
            rows.setdefault(episode, {"events": []})["events"].append(
                (timestamp, event, payload, line_no)
            )
            continue
        rows.setdefault(episode, {"events": []})["events"].append(
            (timestamp, event, payload, line_no)
        )
    return rows, errors


def build_episodes(rows: dict[int, dict]) -> tuple[Episode, ...]:
    episodes: list[Episode] = []
    for episode, record in rows.items():
        starts = [e for e in record["events"] if e[1] == "EPISODE_START"]
        ends = [e for e in record["events"] if e[1] == "EPISODE_END"]
        if not starts or not ends or episode == 0:
            continue
        start = starts[0]
        end = ends[-1]
        commands_raw = [e for e in record["events"] if e[1] == "COMMAND"]
        measurements = [
            Sample(e[0], e[2][0], e[2][1], e[2][4], e[2][5])
            for e in record["events"]
            if e[1] == "MEASUREMENT"
        ]
        position = 0
        commands: list[Command] = []
        for i, command in enumerate(commands_raw):
            p = command[2]
            direction, step = p[1], p[2]
            position += direction * step
            next_timestamp = (
                commands_raw[i + 1][0] if i + 1 < len(commands_raw) else 2**63
            )
            command_samples = tuple(
                sample
                for sample in measurements
                if command[0] <= sample.timestamp < next_timestamp
            )
            auto = any(
                event[1] == "AUTO_PROBE" and command[0] <= event[0] < next_timestamp
                for event in record["events"]
            )
            commands.append(
                Command(
                    p[0], command[0], direction, step, p[3], p[4], p[5],
                    position, auto, command_samples,
                )
            )
        decisions = [e for e in record["events"] if e[1] == "AUTO_PROBE_DECISION"]
        auto = tuple(
            (e[2][0], e[2][1], e[2][2], e[2][3], e[2][4])
            for e in record["events"] if e[1] == "AUTO_PROBE"
        )
        env = tuple(e[2] for e in record["events"] if e[1] == "ENVIRONMENTAL")
        limits = [e[2] for e in record["events"] if e[1] == "LIMIT"]
        refs = [e[2] for e in record["events"] if e[1] == "REFERENCE_SET"]
        episodes.append(
            Episode(
                episode=episode,
                start_timestamp=start[0],
                baseline=start[2][1],
                end_timestamp=end[0],
                terminal_result=end[2][0],
                logged_best_score=end[2][1],
                logged_best_position=end[2][2],
                logged_final_score=end[2][3],
                logged_final_position=end[2][4],
                logged_commands=end[2][5],
                commands=tuple(commands),
                auto_probe_decision_timestamp=decisions[0][0] if decisions else None,
                auto_probe=auto,
                environmental=env,
                limit=limits[-1] if limits else None,
                reference=refs[-1] if refs else None,
            )
        )
    return tuple(sorted(episodes, key=lambda e: e.episode))


def median3(samples: Iterable[Sample], exclude_first: bool = False) -> tuple[float | None, int]:
    usable = [s for s in samples if s.valid and s.confident]
    if exclude_first and len(usable) >= 4:
        usable = usable[1:]
    timestamps = {s.timestamp for s in usable}
    if len(usable) < MIN_TEMPORAL_SAMPLES or len(timestamps) < MIN_TEMPORAL_SAMPLES:
        return None, len(timestamps)
    values = [s.score for s in usable[-3:]]
    return float(statistics.median(values)), len(timestamps)


def visit_summary(command: Command, exclude_first: bool) -> tuple[float | None, int]:
    return median3(command.samples, exclude_first=exclude_first)


def repeated_visit_deltas(episodes: Iterable[Episode]) -> list[float]:
    deltas: list[float] = []
    for ep in episodes:
        by_position: dict[int, list[Command]] = {}
        for command in ep.commands:
            if not command.auto:
                by_position.setdefault(command.position, []).append(command)
        for visits in by_position.values():
            for a, b in zip(visits, visits[1:]):
                sa, _ = visit_summary(a, exclude_first=False)
                sb, _ = visit_summary(b, exclude_first=False)
                if sa is not None and sb is not None:
                    deltas.append(abs(sa - sb))
    return deltas


def direction_arrival_deltas(episodes: Iterable[Episode]) -> list[float]:
    deltas: list[float] = []
    for ep in episodes:
        by_position: dict[int, list[Command]] = {}
        for command in ep.commands:
            if not command.auto:
                by_position.setdefault(command.position, []).append(command)
        for visits in by_position.values():
            plus = [visit_summary(v, False)[0] for v in visits if v.direction > 0]
            minus = [visit_summary(v, False)[0] for v in visits if v.direction < 0]
            plus = [x for x in plus if x is not None]
            minus = [x for x in minus if x is not None]
            if plus and minus:
                deltas.append(abs(statistics.median(plus) - statistics.median(minus)))
    return deltas


def calibrate(episodes: tuple[Episode, ...]) -> Calibration:
    selected = [e for e in episodes if e.episode in CALIBRATION_EPISODES]
    visit_mads: list[float] = []
    for ep in selected:
        for command in ep.commands:
            if command.auto:
                continue
            values = [s.score for s in command.temporal_samples]
            if len(values) >= 3:
                med = statistics.median(values[-3:])
                visit_mads.append(statistics.median(abs(v - med) for v in values[-3:]))
    repeated = repeated_visit_deltas(selected)
    arrival = direction_arrival_deltas(selected)
    within = max(visit_mads, default=0.0)
    repeated_delta = max(repeated, default=0.0)
    arrival_delta = max(arrival, default=0.0)
    # NOT_CALIBRATED_ON_T3I: margin is the observed calibration-session
    # dispersion/arrival effect, not a universal focus threshold.
    peak_margin = max(within * 3.0, repeated_delta, arrival_delta)
    return Calibration(within, repeated_delta, arrival_delta, peak_margin)


def summaries(ep: Episode, exclude_first: bool) -> dict[int, list[tuple[Command, float, int]]]:
    by_position: dict[int, list[tuple[Command, float, int]]] = {}
    for command in ep.commands:
        value, support = visit_summary(command, exclude_first)
        if value is not None:
            by_position.setdefault(command.position, []).append((command, value, support))
    return by_position


def quadratic_vertex(left_x: float, left_y: float, center_x: float, center_y: float,
                     right_x: float, right_y: float) -> float | None:
    step = center_x - left_x
    if step == 0 or right_x - center_x != step:
        return None
    denominator = left_y - 2.0 * center_y + right_y
    if denominator >= 0 or abs(denominator) < 1e-9:
        return None
    return center_x + 0.5 * step * (left_y - right_y) / denominator


def find_candidate(
    ep: Episode,
    calibration: Calibration,
    strategy: str,
) -> Candidate | None:
    exclude_first = strategy in {"C", "D"}
    all_by_position = summaries(ep, exclude_first)
    positions = sorted(all_by_position)
    if len(positions) < 3 or ep.environmental:
        return None
    candidates: list[Candidate] = []
    for center in positions:
        left_positions = [p for p in positions if p < center]
        right_positions = [p for p in positions if p > center]
        if not left_positions or not right_positions:
            continue
        left = max(left_positions)
        right = min(right_positions)
        if center - left != right - center:
            continue
        # A bracket is only available when the current center visit is after
        # both neighbors. This makes the candidate's current position real in
        # the recorded trajectory; using a previously visited center while the
        # lens is at an outer point would falsely claim an immediate HOLD.
        for center_command, center_value, center_support in all_by_position[center]:
            prior = lambda p: [
                item for item in all_by_position[p] if item[0].index < center_command.index
            ]
            left_visits = prior(left)
            right_visits = prior(right)
            center_visits = [
                item for item in all_by_position[center]
                if item[0].index <= center_command.index
            ]
            if not left_visits or not right_visits:
                continue
            left_score = statistics.median(v for _, v, _ in left_visits)
            right_score = statistics.median(v for _, v, _ in right_visits)
            center_score = statistics.median(v for _, v, _ in center_visits)
            margin = center_score - max(left_score, right_score)
            if margin <= calibration.peak_margin:
                continue
            vertex = None
            if strategy == "D":
                vertex = quadratic_vertex(left, left_score, center, center_score, right, right_score)
                # D is only a HOLD classifier. It must not authorize a
                # fractional command or claim a global optimum.
                if vertex is None or abs(vertex - center) > (right - center):
                    continue
            support = min(
                min(s for _, _, s in visits)
                for visits in (left_visits, center_visits, right_visits)
            )
            candidates.append(
                Candidate(center, center_score, left_score, right_score, margin,
                          support, center_command.index,
                          center_command.last_timestamp, vertex)
            )
    if not candidates:
        return None
    return max(candidates, key=lambda c: (c.score, -c.decision_command))


def strategy_results(episodes: tuple[Episode, ...], calibration: Calibration) -> list[StrategyResult]:
    results: list[StrategyResult] = []
    for ep in episodes:
        # A is the recorded controller outcome; no new trajectory is invented.
        results.append(StrategyResult(
            "A", ep.episode, ep.logged_commands, ep.decision_ms,
            TIMEOUT_MS - ep.decision_ms, ep.logged_best_position,
            ep.logged_final_position, ep.inversions,
            {3: "UNCERTAIN", 4: "CONVERGED", 6: "BLOCKED", 11: "SEARCH_LIMIT"}.get(
                ep.terminal_result, f"RESULT_{ep.terminal_result}"
            ), "RECORDED", False, ep.logged_commands > MAX_EPISODE_BUDGET, False,
            "Recorded production terminal event; baseline is not a new classifier.",
        ))
        for strategy in ("B", "C", "D"):
            candidate = find_candidate(ep, calibration, strategy)
            if candidate is None:
                results.append(StrategyResult(
                    strategy, ep.episode, ep.logged_commands, ep.decision_ms,
                    TIMEOUT_MS - ep.decision_ms, None, ep.logged_final_position,
                    ep.inversions, "INSUFFICIENT_DATA", "LOW", True,
                    ep.logged_commands > MAX_EPISODE_BUDGET, False,
                    "No bracketed candidate with valid temporal evidence and calibrated margin.",
                ))
                continue
            # Candidate detection is evaluated on the existing trajectory. If
            # the candidate appears before the logged terminal command, the
            # early HOLD result is a counterfactual early stop, not camera data.
            changed = candidate.decision_command < ep.logged_commands - 1
            results.append(StrategyResult(
                strategy, ep.episode, candidate.decision_command + 1,
                candidate.decision_timestamp - ep.start_timestamp,
                TIMEOUT_MS - (candidate.decision_timestamp - ep.start_timestamp),
                candidate.position, candidate.position, ep.inversions,
                "HOLD_CANDIDATE", "MEDIUM", False,
                candidate.decision_command + 1 > MAX_EPISODE_BUDGET, changed,
                f"bracket margin={candidate.margin:.1f}, support={candidate.support}, "
                f"left={candidate.left_score:.1f}, center={candidate.score:.1f}, "
                f"right={candidate.right_score:.1f}",
            ))
    return results


def adversarial_cases(calibration: Calibration) -> list[dict]:
    # Pure synthetic cases. No result is camera evidence.
    def evaluate(name: str, values: dict[int, list[int]], valid: bool = True,
                 environmental: bool = False) -> dict:
        if not valid or environmental:
            return {"name": name, "result": "INSUFFICIENT_DATA", "false_converged": False}
        positions = sorted(values)
        if len(positions) != 3 or positions[1] - positions[0] != positions[2] - positions[1]:
            return {"name": name, "result": "INSUFFICIENT_DATA", "false_converged": False}
        center = statistics.median(values[positions[1]])
        neighbor = max(statistics.median(values[positions[0]]), statistics.median(values[positions[2]]))
        margin = center - neighbor
        hold = margin > calibration.peak_margin
        return {"name": name, "result": "HOLD_CANDIDATE" if hold else "INSUFFICIENT_DATA",
                "false_converged": False, "margin": margin}
    cases = [
        evaluate("isolated_false_peak", {-3: [100, 101, 99], 0: [150, 151, 149]}),
        evaluate("wide_plateau", {-3: [150, 151, 149], 0: [152, 151, 153], 3: [150, 149, 151]}),
        evaluate("noise_3x", {-3: [100, 130, 70], 0: [120, 90, 150], 3: [100, 130, 70]}),
        evaluate("low_texture", {-3: [0, 0, 0], 0: [0, 0, 0], 3: [0, 0, 0]}, valid=False),
        evaluate("environmental_variation", {-3: [100, 101, 99], 0: [180, 181, 179], 3: [100, 101, 99]}, environmental=True),
        evaluate("hysteresis_above_margin", {-3: [100, 101, 99], 0: [140, 141, 139], 3: [100, 101, 99]}),
        evaluate("duplicate_same_frame", {-3: [100, 100, 100], 0: [150, 150, 150], 3: [100, 100, 100]}, valid=False),
        evaluate("post_move_transient", {-3: [60, 100, 101, 99], 0: [150, 151, 149, 150], 3: [100, 101, 99, 100]}),
        evaluate("inconsistent_return", {-3: [100, 101, 99], 0: [150, 151, 149], 3: [100, 101, 99]}, environmental=True),
    ]
    return cases


def d1_summary(rows: dict[int, dict]) -> dict:
    output: dict = {}
    for series in (4, 5):
        samples = [
            (e[0], e[2][1], e[2][2])
            for e in rows.get(series, {}).get("events", []) if e[1] == "D1_SAMPLE"
        ]
        scores = [s[1] for s in samples]
        means = [s[2] for s in samples]
        tail = scores[-20:]
        output[str(series)] = {
            "count": len(scores),
            "score_first": scores[0] if scores else None,
            "score_last": scores[-1] if scores else None,
            "score_first_to_tail_median_pct": (
                100.0 * (statistics.median(tail) - scores[0]) / scores[0]
                if scores and tail and scores[0] else None
            ),
            "tail_range_pct": 100.0 * (max(tail) - min(tail)) / statistics.median(tail) if tail else None,
            "tail_std_pct": 100.0 * statistics.pstdev(tail) / statistics.mean(tail) if len(tail) > 1 else None,
            "mean_first": means[0] if means else None,
            "mean_last": means[-1] if means else None,
            "mean_delta": means[-1] - means[0] if means else None,
        }
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, default=Path("logs/SMTH003.LOG"))
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    rows, errors = parse_log(args.log)
    episodes = build_episodes(rows)
    calibration = calibrate(episodes)
    result = {
        "evidence_class": "SIMULATION / HOST-ONLY",
        "log": str(args.log),
        "parse_errors": errors,
        "calibration_episodes": CALIBRATION_EPISODES,
        "evaluation_episodes": EVALUATION_EPISODES,
        "configured_algorithm_budget": CONFIGURED_ALGORITHM_BUDGET,
        "auto_budget": AUTO_BUDGET,
        "configured_episode_budget": CONFIGURED_EPISODE_BUDGET,
        "max_episode_budget": MAX_EPISODE_BUDGET,
        "calibration": asdict(calibration),
        "d1": d1_summary(rows),
        "episodes": [
            {
                "episode": e.episode,
                "start": e.start_timestamp,
                "end": e.end_timestamp,
                "duration_ms": e.duration_ms,
                "decision_ms": e.decision_ms,
                "auto_decision_ms": e.auto_decision_ms,
                "terminal_result": e.terminal_result,
                "logged_best": [e.logged_best_score, e.logged_best_position],
                "logged_final": [e.logged_final_score, e.logged_final_position],
                "commands": [
                    {
                        "index": c.index, "direction": c.direction, "step": c.step,
                        "position": c.position, "timestamp": c.timestamp,
                        "scores": [s.score for s in c.samples],
                        "means": [s.mean for s in c.samples],
                    } for c in e.commands
                ],
                "inversions": e.inversions,
                "configured_episode_budget": CONFIGURED_EPISODE_BUDGET,
                "max_episode_budget": MAX_EPISODE_BUDGET,
                "budget_exceeded_configured": e.logged_commands > CONFIGURED_EPISODE_BUDGET,
                "budget_exceeded_max": e.logged_commands > MAX_EPISODE_BUDGET,
                "auto_probe": e.auto_probe,
                "environmental": e.environmental,
                "limit": e.limit,
                "reference": e.reference,
            } for e in episodes
        ],
        "strategies": [asdict(r) for r in strategy_results(episodes, calibration)],
        "adversarial": adversarial_cases(calibration),
    }
    encoded = json.dumps(result, indent=2, sort_keys=True)
    if args.json:
        args.json.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
