#!/usr/bin/env python3
"""Canonical, host-only audit of the SMTH003 persistent log for B2-C1a.

This module deliberately does not import or execute production controller code.  It
parses the nine-column log in byte/line order, derives only logical command
positions, and makes missing frame identity and gate state explicit as UNKNOWN.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, Sequence

EVENTS_WITH_PAYLOAD = 6
MIN_TEMPORAL_SAMPLES = 3
CALIBRATION_EPISODES = (2, 5)
MARGIN_B2B = 357.0


@dataclass(frozen=True)
class RawEvent:
    line: int
    timestamp: int
    event: str
    episode: int
    payload: tuple[int, ...]


@dataclass
class CanonicalRow:
    line: int
    timestamp: int
    episode: int
    event: str
    command_index: int | None = None
    direction: int | None = None
    stepsize: int | None = None
    logical_position: int | None = None
    arrival_direction: int | None = None
    visit_id: str = ""
    score: int | None = None
    luminance: int | None = None
    sad: int | None = None
    fourth_field: int | None = None
    fourth_field_semantics: str = "not_applicable"
    valid: int | None = None
    confident: int | None = None
    duplicate_flags: str = ""
    gate_state: str = "UNKNOWN"
    previous_command_line: int | None = None
    previous_measurement_line: int | None = None
    previous_relevant_line: int | None = None
    event_order: int = 0


@dataclass(frozen=True)
class Visit:
    episode: int
    command_index: int
    line: int
    timestamp: int
    position: int
    arrival_direction: int
    auto: bool
    scores: tuple[int, ...]
    means: tuple[int, ...]
    timestamps: tuple[int, ...]
    lines: tuple[int, ...]

    @property
    def valid_scores(self) -> tuple[int, ...]:
        return self.scores


@dataclass(frozen=True)
class Summary:
    n: int
    median: float | None
    mad: float | None
    amplitude: int | None
    distinct_timestamps: int
    lines: tuple[int, ...]


@dataclass(frozen=True)
class ReplayDecision:
    hypothesis: str
    classification: str
    decision_line: int | None
    timestamp: int | None
    last_command_executed: int | None
    logical_position: int | None
    supporting_scores: tuple[int, ...]
    supporting_lines: tuple[int, ...]
    first_command_not_executed: int | None
    avoided_commands: tuple[int, ...]
    real_path_elapsed_ms: int | None
    evidence_available: bool
    required_future_observation: bool
    reason: str


def parse_log(path: Path) -> tuple[tuple[RawEvent, ...], list[str]]:
    events: list[RawEvent] = []
    errors: list[str] = []
    # newline='' preserves the physical line sequence; splitlines preserves
    # equal timestamps and the logger's original ordering.
    for line_no, line in enumerate(path.read_text(encoding="ascii").splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 9:
            errors.append(f"line {line_no}: expected 9 fields, got {len(fields)}")
            continue
        try:
            timestamp = int(fields[0])
            event = fields[1]
            episode = int(fields[2])
            payload = tuple(int(value) for value in fields[3:])
        except ValueError as exc:
            errors.append(f"line {line_no}: {exc}")
            continue
        events.append(RawEvent(line_no, timestamp, event, episode, payload))
    return tuple(events), errors


def _command_context(raw: Sequence[RawEvent]) -> tuple[dict[int, dict], dict[int, list[RawEvent]]]:
    contexts: dict[int, dict] = defaultdict(lambda: {
        "active": False,
        "position": 0,
        "commands": [],
        "measurements": [],
        "last_command": None,
        "last_measurement": None,
        "visits_at_position": defaultdict(int),
        "environmental": False,
        "last_relevant": None,
    })
    by_line: dict[int, list[RawEvent]] = defaultdict(list)
    for event in raw:
        by_line[event.line].append(event)
        ctx = contexts[event.episode]
        if event.event == "EPISODE_START":
            ctx["active"] = True
            ctx["position"] = 0
            ctx["commands"] = []
            ctx["measurements"] = []
            ctx["last_command"] = None
            ctx["last_measurement"] = None
            ctx["visits_at_position"] = defaultdict(int)
            ctx["environmental"] = False
            ctx["last_relevant"] = None
        elif not ctx["active"]:
            # D1 series reuse episode identifiers 4 and 5, but are not AF
            # episodes.  They must not acquire logical positions or visits.
            continue
        elif event.event == "COMMAND":
            index, direction, step = event.payload[:3]
            ctx["position"] += direction * step
            ctx["visits_at_position"][ctx["position"]] += 1
            command = {
                "index": index, "line": event.line, "timestamp": event.timestamp,
                "direction": direction, "step": step,
                "position": ctx["position"],
                "visit_id": f"E{event.episode}:P{ctx['position']}:V{ctx['visits_at_position'][ctx['position']]}",
                "auto": False, "measurement_lines": [],
            }
            ctx["commands"].append(command)
            ctx["last_command"] = command
            ctx["last_relevant"] = event.line
        elif event.event == "AUTO_PROBE":
            if ctx["last_command"] is not None:
                ctx["last_command"]["auto"] = True
            ctx["last_relevant"] = event.line
        elif event.event == "ENVIRONMENTAL":
            ctx["environmental"] = True
            ctx["last_relevant"] = event.line
        elif event.event == "MEASUREMENT":
            if ctx["last_command"] is not None:
                ctx["last_command"]["measurement_lines"].append(event.line)
            ctx["measurements"].append(event)
            ctx["last_measurement"] = event.line
            ctx["last_relevant"] = event.line
        elif event.event in {"CONTROLLER", "COMMAND_ORIGIN", "AUTO_PROBE_DECISION", "LIMIT", "EPISODE_END", "REFERENCE_SET"}:
            ctx["last_relevant"] = event.line
            if event.event == "EPISODE_END":
                ctx["active"] = False
    return contexts, by_line


def canonical_rows(raw: Sequence[RawEvent]) -> tuple[CanonicalRow, ...]:
    contexts, _ = _command_context(raw)
    rows: list[CanonicalRow] = []
    prior_metric: dict[int, dict[tuple[int, int, int, int, int, int], int]] = defaultdict(dict)
    previous_command: dict[int, int | None] = defaultdict(lambda: None)
    previous_measurement: dict[int, int | None] = defaultdict(lambda: None)
    command_by_line: dict[int, dict] = {}
    for ctx in contexts.values():
        for command in ctx["commands"]:
            command_by_line[command["line"]] = command

    env_seen: dict[int, bool] = defaultdict(bool)
    for order, event in enumerate(raw):
        ctx = contexts[event.episode]
        command = None
        if event.event == "COMMAND":
            command = command_by_line[event.line]
        elif ctx["commands"]:
            # All non-command episode events belong to the latest command until
            # the next command, but only measurements receive position fields.
            command = ctx["commands"][-1]
            for candidate in reversed(ctx["commands"]):
                if candidate["line"] <= event.line:
                    command = candidate
                    break
        row = CanonicalRow(
            line=event.line, timestamp=event.timestamp, episode=event.episode,
            event=event.event, event_order=order,
            previous_command_line=previous_command[event.episode],
            previous_measurement_line=previous_measurement[event.episode],
            previous_relevant_line=(previous_measurement[event.episode]
                                    or previous_command[event.episode]),
        )
        if command is not None:
            row.command_index = command["index"]
            row.direction = command["direction"]
            row.stepsize = command["step"]
            row.logical_position = command["position"]
            row.arrival_direction = command["direction"]
            row.visit_id = command["visit_id"]
        if event.event == "COMMAND":
            previous_command[event.episode] = event.line
        elif event.event == "MEASUREMENT":
            score, mean, sad, laplacian, valid, confident = event.payload[:6]
            row.score = score
            row.luminance = mean
            row.sad = sad
            row.fourth_field = laplacian
            row.fourth_field_semantics = "laplacian"
            row.valid = valid
            row.confident = confident
            metric_key = (score, mean, sad, laplacian, valid, confident)
            if metric_key in prior_metric[event.episode]:
                row.duplicate_flags = "exact_metric_tuple_seen_before"
                if prior_metric[event.episode][metric_key] != event.timestamp:
                    row.duplicate_flags += ";distinct_timestamp_same_tuple"
                else:
                    row.duplicate_flags += ";same_timestamp_same_tuple"
            prior_metric[event.episode][metric_key] = event.timestamp
            previous_measurement[event.episode] = event.line
        if event.event == "ENVIRONMENTAL":
            env_seen[event.episode] = True
            row.gate_state = "ENVIRONMENTAL_EVENT"
        elif event.event == "AUTO_PROBE":
            row.gate_state = "AUTO_LIGHTING_VALID" if event.payload[4] else "AUTO_LIGHTING_INVALID"
        elif env_seen[event.episode]:
            row.gate_state = "ENVIRONMENTAL_SEEN_IN_EPISODE"
        rows.append(row)
    return tuple(rows)


def write_csv(rows: Iterable[CanonicalRow], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = list(CanonicalRow.__dataclass_fields__)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow(asdict(row))


def _visit_from_command(event_rows: Sequence[CanonicalRow], command_line: int) -> Visit | None:
    command_rows = [row for row in event_rows if row.line == command_line]
    if not command_rows:
        return None
    command = command_rows[0]
    measurements = [row for row in event_rows
                    if row.event == "MEASUREMENT" and row.command_index == command.command_index
                    and row.line > command.line]
    # command_index is unique in this log; stop at the next command to avoid
    # conflating an equal timestamp or a later visit.
    next_commands = [row.line for row in event_rows if row.event == "COMMAND" and row.line > command_line]
    stop = min(next_commands, default=10**18)
    measurements = [row for row in measurements if row.line < stop]
    if not measurements or command.logical_position is None or command.direction is None:
        return None
    return Visit(
        episode=command.episode, command_index=command.command_index or 0,
        line=command.line, timestamp=command.timestamp,
        position=command.logical_position, arrival_direction=command.direction,
        auto=any(row.event == "AUTO_PROBE" and command.line <= row.line < stop
                 for row in event_rows),
        scores=tuple(row.score for row in measurements if row.score is not None and row.valid and row.confident),
        means=tuple(row.luminance for row in measurements if row.luminance is not None and row.valid and row.confident),
        timestamps=tuple(row.timestamp for row in measurements if row.score is not None and row.valid and row.confident),
        lines=tuple(row.line for row in measurements if row.score is not None and row.valid and row.confident),
    )


def visits(rows: Sequence[CanonicalRow], episode: int) -> tuple[Visit, ...]:
    episode_rows = [row for row in rows if row.episode == episode]
    output = []
    for command in episode_rows:
        if command.event != "COMMAND":
            continue
        visit = _visit_from_command(episode_rows, command.line)
        if visit is not None:
            output.append(visit)
    return tuple(output)


def summary(values: Sequence[int], lines: Sequence[int], timestamps: Sequence[int] | None = None) -> Summary:
    if not values:
        return Summary(0, None, None, None, 0, tuple())
    med = float(statistics.median(values))
    mad = float(statistics.median(abs(value - med) for value in values))
    return Summary(len(values), med, mad, max(values) - min(values),
                   len(set(timestamps or lines)), tuple(lines))


def policy_summary(visit: Visit, policy: str) -> Summary:
    values = list(visit.scores)
    lines = list(visit.lines)
    sample_timestamps = list(visit.timestamps)
    if policy == "all":
        pass
    elif policy == "exclude_first":
        if len(values) >= 1:
            values, lines, sample_timestamps = values[1:], lines[1:], sample_timestamps[1:]
    elif policy == "harness_tail3":
        values, lines, sample_timestamps = values[-3:], lines[-3:], sample_timestamps[-3:]
    elif policy == "harness_exclude_first_tail3":
        if len(values) >= 4:
            values, lines, sample_timestamps = values[1:], lines[1:], sample_timestamps[1:]
        values, lines, sample_timestamps = values[-3:], lines[-3:], sample_timestamps[-3:]
    else:
        raise ValueError(policy)
    return summary(values, lines, sample_timestamps)


def _complete(visit: Visit, policy: str) -> bool:
    return policy_summary(visit, policy).n >= MIN_TEMPORAL_SAMPLES


def visit_analysis(rows: Sequence[CanonicalRow], episode_ids: Iterable[int]) -> list[dict]:
    output: list[dict] = []
    for episode in episode_ids:
        for visit in visits(rows, episode):
            entry = {
                "episode": episode, "command_index": visit.command_index,
                "line": visit.line, "position": visit.position,
                "arrival_direction": visit.arrival_direction, "auto": visit.auto,
                "sample_lines": list(visit.lines), "scores": list(visit.scores),
                "policies": {},
            }
            for policy in ("all", "exclude_first", "harness_tail3", "harness_exclude_first_tail3"):
                entry["policies"][policy] = asdict(policy_summary(visit, policy))
            output.append(entry)
    return output


def calibration(rows: Sequence[CanonicalRow]) -> dict:
    selected = [visit for episode in CALIBRATION_EPISODES for visit in visits(rows, episode)]
    tail = [policy_summary(visit, "harness_tail3") for visit in selected
            if not visit.auto and _complete(visit, "harness_tail3")]
    mads = [item.mad for item in tail if item.mad is not None]
    repeated: list[float] = []
    directional: list[float] = []
    for episode in CALIBRATION_EPISODES:
        grouped: dict[int, list[Visit]] = defaultdict(list)
        for visit in visits(rows, episode):
            if not visit.auto and _complete(visit, "harness_tail3"):
                grouped[visit.position].append(visit)
        for group in grouped.values():
            summaries = [policy_summary(v, "harness_tail3") for v in group]
            repeated.extend(abs(a.median - b.median) for a, b in zip(summaries, summaries[1:])
                            if a.median is not None and b.median is not None)
            plus = [s.median for v, s in zip(group, summaries) if v.arrival_direction > 0 and s.median is not None]
            minus = [s.median for v, s in zip(group, summaries) if v.arrival_direction < 0 and s.median is not None]
            if plus and minus:
                directional.append(abs(statistics.median(plus) - statistics.median(minus)))
    within = max(mads, default=0.0)
    repeated_max = max(repeated, default=0.0)
    directional_max = max(directional, default=0.0)
    return {
        "episodes": list(CALIBRATION_EPISODES),
        "within_visit_mad_max": within,
        "repeated_position_delta_max": repeated_max,
        "direction_arrival_delta_max": directional_max,
        "three_times_mad": 3 * within,
        "peak_margin": max(3 * within, repeated_max, directional_max),
        "within_visit_mads": mads,
        "repeated_deltas": repeated,
        "direction_deltas": directional,
    }


def pair_comparisons(rows: Sequence[CanonicalRow]) -> dict:
    requested = {2: (-3, 0), 5: (3, 6), 7: (0, -3), 8: (0,)}
    output: dict = {}
    for episode, positions in requested.items():
        by_position: dict[int, list[Visit]] = defaultdict(list)
        for visit in visits(rows, episode):
            if not visit.auto:
                by_position[visit.position].append(visit)
        ep_result = {}
        for position in positions:
            entries = []
            for visit in by_position.get(position, []):
                s = policy_summary(visit, "harness_tail3")
                entries.append({"command_index": visit.command_index, "direction": visit.arrival_direction,
                                "summary": asdict(s), "scores": list(visit.scores)})
            medians = [e["summary"]["median"] for e in entries if e["summary"]["median"] is not None]
            absolute = max(medians) - min(medians) if len(medians) >= 2 else None
            relative = (absolute / (statistics.mean(medians)) if absolute is not None and statistics.mean(medians) else None)
            ep_result[str(position)] = {"visits": entries, "absolute_delta": absolute, "relative_delta": relative}
        ep_result["all_non_auto_position_visits"] = {str(p): [v.command_index for v in vs]
                                                       for p, vs in by_position.items()}
        output[str(episode)] = ep_result
    return output


def _pearson(xs: Sequence[float], ys: Sequence[float]) -> float | None:
    if len(xs) != len(ys) or len(xs) < 2:
        return None
    mx, my = statistics.mean(xs), statistics.mean(ys)
    dx = [x - mx for x in xs]
    dy = [y - my for y in ys]
    denominator = math.sqrt(sum(x * x for x in dx) * sum(y * y for y in dy))
    return sum(x * y for x, y in zip(dx, dy)) / denominator if denominator else None


def score_luminance(rows: Sequence[CanonicalRow]) -> dict:
    measurement_rows = [row for row in rows if row.event == "MEASUREMENT" and row.score is not None and row.luminance is not None]
    # D1 payload is reconstructed from raw rows in the caller; episode metrics
    # are intentionally kept separate from the D1 series.
    def stats(items: Sequence[tuple[int, int]]) -> dict:
        scores = [x for x, _ in items]
        means = [y for _, y in items]
        products = [x * y for x, y in items]
        return {
            "count": len(items), "temporal_pearson_score_luminance": _pearson(scores, means),
            "product_min": min(products) if products else None,
            "product_max": max(products) if products else None,
            "product_mean": statistics.mean(products) if products else None,
            "score_range": (max(scores) - min(scores)) if scores else None,
            "luminance_range": (max(means) - min(means)) if means else None,
        }
    by_episode: dict[str, list[tuple[int, int]]] = defaultdict(list)
    for row in measurement_rows:
        by_episode[str(row.episode)].append((row.score, row.luminance))
    return {"episodes": {ep: stats(items) for ep, items in by_episode.items()},
            "all_episode_measurements": stats([(row.score, row.luminance) for row in measurement_rows])}


def d1_summary(raw: Sequence[RawEvent]) -> dict:
    by_series: dict[int, list[tuple[int, int, int]]] = defaultdict(list)
    for event in raw:
        if event.event == "D1_SAMPLE":
            # [0] sequence, [1] score, [2] mean according to logger.h.
            by_series[event.episode].append((event.payload[1], event.payload[2], event.line))
    output = {}
    for series, items in by_series.items():
        values = [(score, mean) for score, mean, _ in items]
        base = score_luminance_pairs(values)
        base["series"] = series
        base["lines"] = [line for _, _, line in items]
        output[str(series)] = base
    return output


def score_luminance_pairs(items: Sequence[tuple[int, int]]) -> dict:
    products = [score * mean for score, mean in items]
    return {"count": len(items), "temporal_pearson_score_luminance": _pearson([x for x, _ in items], [y for _, y in items]),
            "product_min": min(products) if products else None, "product_max": max(products) if products else None,
            "product_mean": statistics.mean(products) if products else None,
            "score_range": max((x for x, _ in items), default=0) - min((x for x, _ in items), default=0),
            "luminance_range": max((y for _, y in items), default=0) - min((y for _, y in items), default=0)}


def duplicate_analysis(rows: Sequence[CanonicalRow]) -> dict:
    result = {}
    for episode in sorted({row.episode for row in rows if row.episode}):
        episode_rows = [row for row in rows if row.episode == episode and row.event == "MEASUREMENT"]
        exact = [row for row in episode_rows if "exact_metric_tuple_seen_before" in row.duplicate_flags]
        pair_counter = Counter()
        for index, row in enumerate(episode_rows):
            if index and (row.score, row.luminance, row.sad, row.fourth_field) == (
                    episode_rows[index - 1].score, episode_rows[index - 1].luminance,
                    episode_rows[index - 1].sad, episode_rows[index - 1].fourth_field):
                pair_counter["consecutive_same_four_metrics"] += 1
            if index and row.score == episode_rows[index - 1].score and (
                    row.luminance, row.sad, row.fourth_field) != (
                    episode_rows[index - 1].luminance, episode_rows[index - 1].sad,
                    episode_rows[index - 1].fourth_field):
                pair_counter["consecutive_same_score_other_field_differs"] += 1
        result[str(episode)] = {"measurement_count": len(episode_rows), "exact_tuple_repeats": len(exact),
                                "consecutive_pairs": dict(pair_counter),
                                "exact_tuple_lines": [row.line for row in exact],
                                "frame_identity_evidence": "NONE_IN_LOG"}
    return result


def _stream_candidates(observed: Sequence[Visit], margin: float) -> list[dict]:
    grouped: dict[int, list[Visit]] = defaultdict(list)
    for visit in observed:
        grouped[visit.position].append(visit)
    positions = sorted(grouped)
    candidates = []
    for center in positions:
        left, right = center - 3, center + 3
        if left not in grouped or right not in grouped:
            continue
        current = grouped[center][-1]
        if not (_complete(current, "harness_tail3") and
                any(_complete(v, "harness_tail3") for v in grouped[left]) and
                any(_complete(v, "harness_tail3") for v in grouped[right])):
            continue
        if not all(v.command_index < current.command_index for v in grouped[left] + grouped[right]):
            continue
        left_s = [policy_summary(v, "harness_tail3") for v in grouped[left] if _complete(v, "harness_tail3")]
        right_s = [policy_summary(v, "harness_tail3") for v in grouped[right] if _complete(v, "harness_tail3")]
        center_s = [policy_summary(v, "harness_tail3") for v in grouped[center]
                     if v.command_index <= current.command_index and _complete(v, "harness_tail3")]
        left_score = statistics.median(s.median for s in left_s if s.median is not None)
        right_score = statistics.median(s.median for s in right_s if s.median is not None)
        center_score = statistics.median(s.median for s in center_s if s.median is not None)
        if center_score - max(left_score, right_score) > margin:
            supporting = left_s + center_s + right_s
            candidates.append({"center": center, "command_index": current.command_index,
                               "margin": center_score - max(left_score, right_score),
                               "scores": tuple(int(s.median) for s in supporting if s.median is not None),
                               "lines": tuple(line for s in supporting for line in s.lines)})
    return candidates


def replay_ep8(rows: Sequence[CanonicalRow], margin: float = MARGIN_B2B) -> tuple[ReplayDecision, ...]:
    ep_rows = [row for row in rows if row.episode == 8]
    command_rows = [row for row in ep_rows if row.event == "COMMAND"]
    observed: list[Visit] = []
    first: dict | None = None
    for row in ep_rows:
        if row.event != "MEASUREMENT" or row.command_index is None:
            continue
        commands_seen = [c for c in command_rows if c.line <= row.line]
        latest = commands_seen[-1]
        # Rebuild only the prefix. No event after this row is consulted.
        prefix = [item for item in ep_rows if item.line <= row.line]
        prefix_visits = visits(prefix, 8)
        if prefix_visits:
            observed = list(prefix_visits)
        candidates = _stream_candidates(observed, margin)
        if candidates and first is None:
            candidate = max(candidates, key=lambda item: (item["margin"], -item["command_index"]))
            first = {"line": row.line, "timestamp": row.timestamp,
                     "last_command": candidate["command_index"], "position": candidate["center"],
                     "scores": candidate["scores"], "lines": candidate["lines"]}
            break
    if first is None:
        return (ReplayDecision("A", "INCONCLUSIVO", None, None, None, None, tuple(), tuple(),
                               None, tuple(), None, False, True, "No causal bracket in the observed prefix."),)
    by_index = {row.command_index: row for row in command_rows}
    last = first["last_command"]
    later = tuple(index for index in sorted(by_index) if index > last)
    first_not = later[0] if later else None
    end_row = next((row for row in reversed(ep_rows) if row.event == "EPISODE_END"), None)
    elapsed = end_row.timestamp - first["timestamp"] if end_row else None
    a = ReplayDecision("A", "REPLAY_FIEL_COM_PARADA_ANTECIPADA", first["line"], first["timestamp"],
                       last, first["position"], first["scores"], first["lines"], first_not,
                       later, elapsed, True, False,
                       "All supporting measurements precede the decision; only later commands are simulated as stopped.")
    # B asks for an additional-visit policy absent from the production log. It
    # is causal if evaluated at its prefix, but its decision is not a recorded
    # production decision and therefore remains counterfactual.
    b_prefix = [row for row in ep_rows if row.line <= first["line"]]
    for row in ep_rows:
        if row.event == "MEASUREMENT" and row.line > first["line"] and row.command_index in {5, 7}:
            candidate_prefix = [item for item in ep_rows if item.line <= row.line]
            candidate_visits = visits(candidate_prefix, 8)
            candidates = _stream_candidates(candidate_visits, margin)
            if candidates:
                candidate = max(candidates, key=lambda item: (item["margin"], -item["command_index"]))
                b_prefix = candidate_prefix
                b = ReplayDecision("B", "CONTRAFACTUAL", row.line, row.timestamp,
                                   candidate["command_index"], candidate["center"], candidate["scores"],
                                   candidate["lines"], candidate["command_index"] + 1,
                                   tuple(index for index in sorted(by_index) if index > candidate["command_index"]),
                                   end_row.timestamp - row.timestamp if end_row else None,
                                   True, False,
                                   "Additional visits are available causally, but no production HOLD/confirmation decision was logged at this prefix.")
                return (a, b)
    b = ReplayDecision("B", "INCONCLUSIVO", None, None, None, None, tuple(), tuple(), None, tuple(),
                       None, False, True, "The additional-visit condition was not available in a causal prefix.")
    return (a, b)


def build_report_data(raw: Sequence[RawEvent], rows: Sequence[CanonicalRow], errors: Sequence[str]) -> dict:
    return {
        "schema": "B2-C1a-canonical-v1",
        "evidence_class": "HOST_ONLY",
        "log_line_order_preserved": True,
        "frame_id": "NOT_AVAILABLE",
        "parse_errors": list(errors),
        "line_count": len(raw),
        "canonical_rows": len(rows),
        "calibration": calibration(rows),
        "visits": visit_analysis(rows, range(2, 10)),
        "requested_position_comparisons": pair_comparisons(rows),
        "duplicates": duplicate_analysis(rows),
        "replay_ep8": [asdict(item) for item in replay_ep8(rows)],
        "score_luminance": score_luminance(rows),
        "d1_score_luminance": d1_summary(raw),
        "episodes": episode_summary(raw, rows),
        "premise_status": {
            "b2b_margin_357": "CONFIRMED_AS_REPRODUCIBLE_FORMULA_BUT_INSUFFICIENT_EVIDENCE_AS_GENERAL_THRESHOLD",
            "b2b_ep8_early_prefix": "CONFIRMED_CAUSALLY_AVAILABLE; OPERATIONAL_STOP_COUNTERFACTUAL",
            "b2b_frame_independence": "INCONCLUSIVE_NO_FRAME_ID",
            "b2b_best_as_robust_aggregate": "REFUTED_MAXIMUM_INDIVIDUAL_SCORE",
            "future_hold_calibration": "NO_GO_PENDING_B2-C1b_REVIEW",
        },
    }


def episode_summary(raw: Sequence[RawEvent], rows: Sequence[CanonicalRow]) -> list[dict]:
    output = []
    for episode in range(2, 10):
        events = [event for event in raw if event.episode == episode]
        start = next((event for event in events if event.event == "EPISODE_START"), None)
        end = next((event for event in events if event.event == "EPISODE_END"), None)
        commands = [event for event in events if event.event == "COMMAND"]
        env = [event for event in events if event.event == "ENVIRONMENTAL"]
        auto_decisions = [event for event in events if event.event == "AUTO_PROBE_DECISION"]
        limits = [event for event in events if event.event == "LIMIT"]
        output.append({"episode": episode, "start_line": start.line if start else None,
                       "end_line": end.line if end else None,
                       "duration_ms": end.timestamp - start.timestamp if start and end else None,
                       "command_count": len(commands),
                       "terminal_result": end.payload[0] if end else None,
                       "environmental_lines": [event.line for event in env],
                       "auto_probe_decisions": [list(event.payload) for event in auto_decisions],
                       "limit": list(limits[-1].payload) if limits else None,
                       "positions": [row.logical_position for row in rows if row.episode == episode and row.event == "COMMAND"],
                       "command_durations_to_next_command_ms": [
                           commands[index + 1].timestamp - command.timestamp
                           for index, command in enumerate(commands[:-1])
                       ]})
    return output


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--log", type=Path, default=Path("logs/SMTH003.LOG"))
    parser.add_argument("--csv", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    raw, errors = parse_log(args.log)
    rows = canonical_rows(raw)
    if args.csv:
        write_csv(rows, args.csv)
    result = build_report_data(raw, rows, errors)
    encoded = json.dumps(result, indent=2, sort_keys=True)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(encoded + "\n", encoding="utf-8")
    else:
        print(encoded)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
