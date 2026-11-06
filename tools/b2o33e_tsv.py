#!/usr/bin/env python3
"""Host-only parser for the production SmoothAF nine-column TSV logs.

The production consumer formats each 36-byte RAM record as ASCII TSV:

    timestamp_ms<TAB>event_name<TAB>episode_id<TAB>payload[0..5]<LF>

The TSV intentionally does not contain the binary ``sequence`` field.  This
module preserves that absence as ``None`` and never reconstructs it from line
order or timestamps.  It consumes no firmware, FIO, card, USB, or camera API.
"""
from __future__ import annotations

import importlib.util
import re
import sys
from dataclasses import dataclass, replace
from enum import Enum
from pathlib import Path
from typing import Optional, Union

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "tools" / "b2o32_serialization.py"
COLLECTION_PATH = ROOT / "tools" / "b2o33d_collection.py"


def _load(name: str, path: Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


BASE = _load("b2o33e_v1_transport", BASE_PATH)
COLLECTION = _load("b2o33e_collection_contract", COLLECTION_PATH)

ArtifactFormat = COLLECTION.ArtifactFormat
CollectionEligibility = COLLECTION.CollectionEligibility
CollectionError = COLLECTION.CollectionError
CollectionManifest = COLLECTION.CollectionManifest
SessionIdentityStatus = COLLECTION.SessionIdentityStatus
SessionScope = COLLECTION.SessionScope

OBS_SNAPSHOT_V1_EVENT = BASE.OBS_SNAPSHOT_V1_EVENT
SESSION_START_EVENT = 1
SESSION_END_EVENT = 2
RECORD_SIZE = BASE.RECORD_SIZE
UINT32_MAX = 0xFFFFFFFF
UINT16_MAX = 0xFFFF
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1

# The C formatter has no explicit case for event type 0; it emits the same
# textual UNKNOWN token used for every unrecognized numeric type.  Therefore
# event type 0 is intentionally not recoverable from TSV and is not mapped
# here.
_EVENT_NAMES = {
    1: "SESSION_START",
    2: "SESSION_END",
    3: "EPISODE_START",
    4: "EPISODE_END",
    5: "REFERENCE_SET",
    6: "AUTO_PROBE",
    7: "COMMAND",
    8: "MEASUREMENT",
    9: "CONTROLLER",
    10: "TIMEOUT",
    11: "LIMIT",
    12: "CANCELLED",
    13: "SAFETY_PAUSED",
    14: "ENVIRONMENTAL",
    15: "WRITE_ERROR",
    16: "AUTO_PROBE_DECISION",
    17: "COMMAND_ORIGIN",
    18: "D1_SERIES_START",
    19: "D1_SAMPLE",
    20: "D1_SERIES_END",
    21: "D1_CONTEXT",
    22: "D1_CONTEXT_AUX",
    23: "MOVIE_GATE_REJECT",
    24: "OBS_SNAPSHOT_V1",
}
_NAME_TO_EVENT = {name: event for event, name in _EVENT_NAMES.items()}
# This mirrors format_record() in smoothaf_logger.c.  All other events use
# append_signed_decimal for their payload slots.
_UNSIGNED_ALL = {20, 21, 22, 23}
_UNSIGNED_PREFIX_3 = {18}
_UNSIGNED_SLOT_0 = {19}
_TOKEN_RE = re.compile(r"^[A-Za-z0-9_]+$")
_DECIMAL_RE = re.compile(r"^-?(?:0|[1-9][0-9]*)$")


class TsvError(ValueError):
    """A malformed, incomplete, or unsupported TSV artifact."""


class TsvIntegrityStatus(str, Enum):
    COMPLETE = "NO_TRUNCATION"
    TAIL_TRUNCATED = "TAIL_TRUNCATED"
    PARTIAL_FINAL_LINE = "PARTIAL_FINAL_LINE"
    MALFORMED_RECORD = "MALFORMED_RECORD"


class TsvDescriptiveEligibility(str, Enum):
    """Eligibility for local, descriptive record analysis only."""

    ACCEPT_LOCAL = "ACCEPT_LOCAL"
    REJECT = "REJECT"


class TsvOperationalCalibrationEligibility(str, Enum):
    """Eligibility for calibration claims that require stronger evidence."""

    REJECT = "REJECT"


@dataclass(frozen=True)
class TsvRecord:
    line_number: int
    byte_offset: int
    raw_line: bytes
    timestamp_ms: int
    event_name: str
    event_type: Optional[int]
    episode_id: int
    payload: tuple[int, int, int, int, int, int]
    # The C formatter omits sequence.  None is intentional and is never
    # replaced by line number, timestamp, or a restarted counter.
    sequence: None = None

    @property
    def is_known_event(self) -> bool:
        return self.event_type is not None


@dataclass(frozen=True)
class TsvSnapshot:
    record: TsvRecord
    event: object


@dataclass(frozen=True)
class TsvParseResult:
    records: tuple[TsvRecord, ...]
    snapshots: tuple[object, ...]
    errors: tuple[str, ...]
    integrity: TsvIntegrityStatus
    encoding: str = "ascii"
    has_header: bool = False


@dataclass(frozen=True)
class TsvCollectionReport:
    parse: TsvParseResult
    pairing: object
    accepted_source: bool
    eligibility: CollectionEligibility
    descriptive_eligibility: TsvDescriptiveEligibility
    operational_calibration_eligibility: TsvOperationalCalibrationEligibility
    session_identity: SessionIdentityStatus
    frame_independence_proven: bool
    rejected: tuple[str, ...]


def _parse_decimal(value: str, field: str, line_number: int) -> int:
    if not _DECIMAL_RE.fullmatch(value):
        raise TsvError(f"line {line_number}: {field} is not a decimal integer")
    try:
        return int(value, 10)
    except ValueError as exc:
        raise TsvError(f"line {line_number}: invalid {field}") from exc


def _payload_unsigned(event_type: Optional[int], slot: int) -> bool:
    if event_type in _UNSIGNED_ALL:
        return True
    if event_type == 18 and slot < 3:
        return True
    if event_type == 19 and slot == 0:
        return True
    return False


def _validate_payload(value: int, event_type: Optional[int], slot: int,
                      line_number: int) -> int:
    if _payload_unsigned(event_type, slot):
        if value < 0 or value > UINT32_MAX:
            raise TsvError(
                f"line {line_number}: payload[{slot}] outside uint32 range"
            )
    elif value < INT32_MIN or value > INT32_MAX:
        raise TsvError(
            f"line {line_number}: payload[{slot}] outside int32 range"
        )
    return value


def _split_lines(data: bytes) -> tuple[list[tuple[int, int, bytes]], TsvIntegrityStatus]:
    if not isinstance(data, bytes):
        raise TsvError("input must be bytes")
    if not data:
        raise TsvError("INCOMPLETE_ARTIFACT: empty TSV artifact")
    parts = data.split(b"\n")
    if parts[-1] != b"":
        return [], TsvIntegrityStatus.PARTIAL_FINAL_LINE
    parts.pop()
    rows: list[tuple[int, int, bytes]] = []
    offset = 0
    for line_number, raw in enumerate(parts, 1):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        rows.append((line_number, offset, line))
        offset += len(raw) + 1
    return rows, TsvIntegrityStatus.COMPLETE


def parse_tsv(data: bytes) -> TsvParseResult:
    """Parse raw FIO bytes without normalizing or losing physical line order."""

    if not isinstance(data, bytes):
        raise TsvError("input must be bytes")
    if not data:
        raise TsvError("INCOMPLETE_ARTIFACT: empty TSV artifact")
    rows, integrity = _split_lines(data)
    if integrity is not TsvIntegrityStatus.COMPLETE:
        return TsvParseResult((), (), ("TAIL_TRUNCATED: final TSV line has no LF",), integrity)

    records: list[TsvRecord] = []
    snapshots: list[object] = []
    errors: list[str] = []
    for line_number, byte_offset, raw_line in rows:
        try:
            text = raw_line.decode("ascii")
            fields = text.split("\t")
            if len(fields) != 9:
                raise TsvError(
                    f"line {line_number}: expected 9 fields, got {len(fields)}"
                )
            timestamp = _parse_decimal(fields[0], "timestamp_ms", line_number)
            if timestamp < 0 or timestamp > UINT32_MAX:
                raise TsvError(f"line {line_number}: timestamp outside uint32 range")
            event_name = fields[1]
            if not event_name or not _TOKEN_RE.fullmatch(event_name):
                raise TsvError(f"line {line_number}: invalid event name")
            event_type = _NAME_TO_EVENT.get(event_name)
            episode = _parse_decimal(fields[2], "episode_id", line_number)
            if episode < 0 or episode > UINT16_MAX:
                raise TsvError(f"line {line_number}: episode_id outside uint16 range")
            payload_values = tuple(
                _validate_payload(
                    _parse_decimal(fields[index], f"payload[{index - 3}]", line_number),
                    event_type, index - 3, line_number,
                )
                for index in range(3, 9)
            )
            record = TsvRecord(
                line_number=line_number,
                byte_offset=byte_offset,
                raw_line=raw_line,
                timestamp_ms=timestamp,
                event_name=event_name,
                event_type=event_type,
                episode_id=episode,
                payload=payload_values,
            )
            records.append(record)
            if event_type == OBS_SNAPSHOT_V1_EVENT:
                wire_record = BASE.LogRecord(
                    timestamp_ms=timestamp,
                    event_type=event_type,
                    episode_id=episode,
                    sequence=0,
                    payload=payload_values,
                )
                try:
                    snapshot = replace(BASE.decode_snapshot(wire_record), sequence=None)
                except BASE.SerializationError as exc:
                    raise TsvError(
                        f"line {line_number}: invalid OBS_SNAPSHOT_V1 payload"
                    ) from exc
                snapshots.append(snapshot)
        except UnicodeDecodeError as exc:
            errors.append(f"line {line_number}: non-ASCII encoding")
        except TsvError as exc:
            errors.append(str(exc))

    status = (TsvIntegrityStatus.MALFORMED_RECORD if errors
              else TsvIntegrityStatus.COMPLETE)
    return TsvParseResult(tuple(records), tuple(snapshots), tuple(errors), status)


def _manifest_errors(data: bytes, manifest: CollectionManifest) -> list[str]:
    errors: list[str] = []
    if manifest.format is not ArtifactFormat.FIO_TSV9:
        errors.append("FORMAT_UNSUPPORTED: expected fio-tsv9")
    if manifest.manifest_version != "1":
        errors.append("IDENTITY_UNAVAILABLE: unsupported manifest version")
    if not manifest.source_id:
        errors.append("IDENTITY_UNAVAILABLE: source_id is missing")
    if not manifest.original_filename:
        errors.append("IDENTITY_UNAVAILABLE: original filename is missing")
    if not manifest.expected_sha256:
        errors.append("IDENTITY_UNAVAILABLE: expected SHA-256 is missing")
    elif COLLECTION.sha256_hex(data) != manifest.expected_sha256:
        errors.append("INTEGRITY_FAILURE: SHA-256 does not match original artifact")
    if manifest.size_bytes is None:
        errors.append("IDENTITY_UNAVAILABLE: expected byte length is missing")
    elif manifest.size_bytes != len(data):
        errors.append("INTEGRITY_FAILURE: byte length does not match manifest")
    if not manifest.mode_context:
        errors.append("IDENTITY_UNAVAILABLE: mode/context metadata is missing")
    if not manifest.transfer_chain:
        errors.append("IDENTITY_UNAVAILABLE: transfer provenance is missing")
    if manifest.start_known is not True or manifest.end_known is not True:
        errors.append("INCOMPLETE_ARTIFACT: known start and end are required")
    return errors


def _pair_tsv_snapshots(source_id: str, records: tuple[TsvRecord, ...]):
    """Pair only inside lifecycle segments; no segment is a session identity."""

    pairer = BASE.Pairer()
    segment = 0
    active_source = f"{source_id}#segment-{segment}"
    pairer.boundary(active_source)
    for record in records:
        if record.event_type == SESSION_START_EVENT:
            pairer.end_source()
            segment += 1
            active_source = f"{source_id}#segment-{segment}"
            pairer.boundary(active_source)
            continue
        if record.event_type == SESSION_END_EVENT:
            pairer.end_source()
            segment += 1
            active_source = f"{source_id}#post-end-{segment}"
            pairer.boundary(active_source)
            continue
        if record.event_type != OBS_SNAPSHOT_V1_EVENT:
            continue
        wire_record = BASE.LogRecord(
            record.timestamp_ms, OBS_SNAPSHOT_V1_EVENT, record.episode_id, 0,
            record.payload,
        )
        try:
            event = replace(BASE.decode_snapshot(wire_record), sequence=None)
        except BASE.SerializationError:
            continue
        pairer.add(event)
    pairer.end_source()
    return pairer.finish()


def collect_tsv(data: bytes, manifest: CollectionManifest,
                *, duplicate_source_ids: Optional[set[str]] = None) -> TsvCollectionReport:
    """Validate provenance and return structural pairs, never verified identity."""

    rejected = _manifest_errors(data, manifest)
    parsed = parse_tsv(data)
    rejected.extend(parsed.errors)
    if parsed.integrity is not TsvIntegrityStatus.COMPLETE:
        rejected.append(f"INCOMPLETE_ARTIFACT: {parsed.integrity.value}")
    if manifest.session_scope is not SessionScope.SINGLE_SESSION:
        rejected.append("AMBIGUOUS_PAIR: TSV contains multiple/unknown session scope")
    if manifest.marker_state is not COLLECTION.MarkerState.ABSENT:
        rejected.append("AMBIGUOUS_PAIR: marker metadata is not valid for V1 TSV")
    if not manifest.source_id:
        rejected.append("IDENTITY_UNAVAILABLE: source boundary is unavailable")
    if duplicate_source_ids and manifest.source_id in duplicate_source_ids:
        rejected.append(f"AMBIGUOUS_PAIR: duplicate source_id {manifest.source_id!r}")

    pairing = _pair_tsv_snapshots(manifest.source_id or "unavailable", parsed.records)
    # Descriptive analysis can remain local even when session scope is
    # MULTI_SESSION/UNKNOWN: it may count and inspect records without making
    # a cross-session pair claim.  Provenance and structural errors still
    # reject it; scope/marker ambiguity is reserved for pairing eligibility.
    descriptive_ok = (
        not _manifest_errors(data, manifest) and
        not parsed.errors and
        parsed.integrity is TsvIntegrityStatus.COMPLETE
    )
    return TsvCollectionReport(
        parse=parsed,
        pairing=pairing,
        accepted_source=not rejected,
        # V1 TSV has no demonstrated session identity even when a source is
        # complete and structurally pairs.  It is never eligible as evidence.
        eligibility=CollectionEligibility.REJECT,
        descriptive_eligibility=(
            TsvDescriptiveEligibility.ACCEPT_LOCAL if descriptive_ok
            else TsvDescriptiveEligibility.REJECT
        ),
        operational_calibration_eligibility=(
            TsvOperationalCalibrationEligibility.REJECT
        ),
        session_identity=SessionIdentityStatus.UNAVAILABLE,
        frame_independence_proven=False,
        rejected=tuple(rejected),
    )
