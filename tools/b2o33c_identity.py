#!/usr/bin/env python3
"""Host-only identity contract prototype for B2-O3.3c.

This module does not change the 36-byte historical logger ABI.  It models an
additive OBS_SESSION_START record plus the external provenance required to
make that marker meaningful across files, logger restarts, and camera boots.

The session token is deliberately opaque and is not a counter.  A production
source must provide it from a trusted identity source, or the consumer must
supply a provenance record.  The host generator accepts injected entropy only
to make those assumptions explicit and testable.
"""
from __future__ import annotations

import importlib.util
import struct
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional, Union

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "tools" / "b2o32_serialization.py"
SPEC = importlib.util.spec_from_file_location("b2o33c_base_wire", BASE_PATH)
assert SPEC and SPEC.loader
BASE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BASE
SPEC.loader.exec_module(BASE)

LogRecord = BASE.LogRecord
ObservationEvent = BASE.ObservationEvent
OpaqueRecord = BASE.OpaqueRecord
SnapshotPhase = BASE.SnapshotPhase
Status = BASE.Status
SnapshotFlags = BASE.SnapshotFlags
SerializationError = BASE.SerializationError

SESSION_START_EVENT = 25
SESSION_SCHEMA_VERSION = 1
SESSION_MARKER_KIND_START = 1
SESSION_CONTROL = SESSION_SCHEMA_VERSION | (SESSION_MARKER_KIND_START << 8)
SESSION_TOKEN_SIZE = 16
SESSION_PAYLOAD_SLOTS = 6


class IdentityError(ValueError):
    """Invalid or insufficient identity evidence."""


class IdentityPairStatus(str, Enum):
    COMPLETE = "COMPLETE"
    BEFORE_MISSING = "BEFORE_MISSING"
    AFTER_MISSING = "AFTER_MISSING"
    AMBIGUOUS_PAIR = "AMBIGUOUS_PAIR"
    IDENTITY_UNAVAILABLE = "IDENTITY_UNAVAILABLE"


@dataclass(frozen=True)
class SessionIdentity:
    """Identity used by the parser; source_id is external provenance.

    ``source_id`` must identify an immutable acquired file/segment, not merely
    a reusable filename.  ``token`` is scoped by source_id, so a token collision
    across two distinct trusted sources is not a collision of the pair.
    """

    source_id: str
    token: bytes
    origin: str  # "wire-marker" or "external-provenance"

    @property
    def key(self) -> tuple[str, bytes]:
        return self.source_id, self.token


@dataclass(frozen=True)
class IdentityPair:
    key: tuple[SessionIdentity, int, int]
    before: Optional[ObservationEvent]
    after: Optional[ObservationEvent]
    status: IdentityPairStatus
    stable_metadata: bool
    identity_verified: bool
    # A session marker says nothing about whether BEFORE and AFTER used
    # independent YUV frames.  This remains explicitly unproven here.
    frame_independence_proven: bool = False


@dataclass(frozen=True)
class IdentityReport:
    complete: tuple[IdentityPair, ...]
    incomplete: tuple[IdentityPair, ...]
    invalid: tuple[str, ...]
    opaque: tuple[OpaqueRecord, ...]


def _token(value: bytes) -> bytes:
    if not isinstance(value, bytes) or len(value) != SESSION_TOKEN_SIZE:
        raise IdentityError("session token must be exactly 16 bytes")
    if value == b"\0" * SESSION_TOKEN_SIZE:
        raise IdentityError("all-zero session token is reserved")
    return value


def _token_words(value: bytes) -> tuple[int, int, int, int]:
    return struct.unpack("<4I", _token(value))


def session_start_record(token: bytes, *, timestamp_ms: int, sequence: int) -> LogRecord:
    """Create the additive OBS_SESSION_START record.

    Payload contract, all six slots retained:
      payload[0] = schema version + marker kind (low/high byte)
      payload[1:5] = opaque 128-bit token, little-endian uint32 words
      payload[5] = zero, reserved
    """

    words = _token_words(token)
    return LogRecord(
        timestamp_ms=timestamp_ms,
        event_type=SESSION_START_EVENT,
        episode_id=0,
        sequence=sequence,
        payload=(SESSION_CONTROL, words[0], words[1], words[2], words[3], 0),
    )


def encode_session_start(token: bytes, *, timestamp_ms: int, sequence: int) -> bytes:
    return BASE.serialize_record(session_start_record(
        token, timestamp_ms=timestamp_ms, sequence=sequence
    ))


def decode_session_start(record: LogRecord) -> bytes:
    if record.event_type != SESSION_START_EVENT:
        raise IdentityError("record is not OBS_SESSION_START")
    if record.episode_id != 0:
        raise IdentityError("OBS_SESSION_START must be session-level")
    control = BASE._wire_u32(record.payload[0])
    if control != SESSION_CONTROL or record.payload[5] != 0:
        raise IdentityError("invalid OBS_SESSION_START control or reserved slot")
    value = struct.pack("<4I", *(BASE._wire_u32(v) for v in record.payload[1:5]))
    return _token(value)


def decode_identity_record(data: bytes) -> Union[ObservationEvent, LogRecord, OpaqueRecord]:
    record = BASE.deserialize_record(data)
    if record.event_type == SESSION_START_EVENT:
        decode_session_start(record)
        return record
    if record.event_type == BASE.OBS_SNAPSHOT_V1_EVENT:
        return BASE.decode_snapshot(record)
    return OpaqueRecord(record)


class SimulatedSessionIdGenerator:
    """Generator with injected entropy; never manufactures a global counter ID."""

    def __init__(self, entropy_values: list[bytes]) -> None:
        if not entropy_values:
            raise IdentityError("at least one injected entropy value is required")
        self._values = [_token(value) for value in entropy_values]
        self._index = 0

    def new(self) -> bytes:
        if self._index >= len(self._values):
            raise IdentityError("simulated entropy source exhausted")
        value = self._values[self._index]
        self._index += 1
        return value


class SessionPairer:
    """Fail-closed pairer for V1 snapshots with additive session evidence.

    A trusted immutable source boundary is mandatory.  A source may then be
    activated by a wire OBS_SESSION_START marker or by an external sidecar
    that supplies the exact token when the marker was lost.  V1 snapshots
    without either form of evidence are never paired.
    """

    def __init__(self) -> None:
        self._source_id: Optional[str] = None
        self._expected_token: Optional[bytes] = None
        self._active: Optional[SessionIdentity] = None
        self._seen_tokens: set[bytes] = set()
        self._wire_marker_seen = False
        self._poisoned = False
        self._pending: dict[tuple[SessionIdentity, int, int], dict[SnapshotPhase, ObservationEvent]] = {}
        self._seen_keys: set[tuple[SessionIdentity, int, int]] = set()
        self._complete: list[IdentityPair] = []
        self._incomplete: list[IdentityPair] = []
        self._invalid: list[str] = []
        self._opaque: list[OpaqueRecord] = []
        self._ended_sources: set[str] = set()

    def begin_source(self, source_id: Optional[str], *, session_token: Optional[bytes] = None) -> None:
        """Begin an immutable file/segment provenance scope.

        ``session_token`` is optional external sidecar evidence.  If absent,
        the source must contain a valid OBS_SESSION_START before snapshots can
        be paired.  Reusing a source_id after end is ambiguous, even if the
        token differs.
        """

        self._finish_pending()
        self._active = None
        self._expected_token = None
        self._seen_tokens.clear()
        self._wire_marker_seen = False
        self._poisoned = False
        if not isinstance(source_id, str) or not source_id:
            self._source_id = None
            self._invalid.append("IDENTITY_UNAVAILABLE: immutable source provenance is required")
            return
        if source_id in self._ended_sources:
            self._source_id = None
            self._invalid.append(f"AMBIGUOUS_PAIR: reused source provenance {source_id!r}")
            return
        self._source_id = source_id
        if session_token is not None:
            self._expected_token = _token(session_token)
            self._activate(self._expected_token, "external-provenance")

    def end_source(self) -> None:
        self._finish_pending()
        if self._source_id is not None:
            self._ended_sources.add(self._source_id)
        self._source_id = None
        self._expected_token = None
        self._active = None
        self._seen_tokens.clear()
        self._wire_marker_seen = False
        self._poisoned = False

    def _activate(self, token: bytes, origin: str) -> None:
        token = _token(token)
        if origin == "wire-marker" and self._wire_marker_seen:
            self._invalid.append("AMBIGUOUS_PAIR: multiple session markers in one source")
            self._active = None
            self._poisoned = True
            return
        if self._source_id is None:
            self._invalid.append("IDENTITY_UNAVAILABLE: no immutable source boundary")
            return
        if self._expected_token is not None and token != self._expected_token:
            self._invalid.append("AMBIGUOUS_PAIR: marker disagrees with external session metadata")
            self._active = None
            self._poisoned = True
            return
        if token in self._seen_tokens:
            # External provenance plus one matching wire marker is corroborating
            # evidence.  A second marker is contradictory/duplicate evidence.
            if (self._active is not None and self._active.token == token and
                    self._active.origin == "external-provenance" and origin == "wire-marker"):
                self._active = SessionIdentity(self._source_id, token, "wire-marker+external")
                return
            self._invalid.append("AMBIGUOUS_PAIR: duplicate session marker/token")
            self._active = None
            self._poisoned = True
            return
        self._seen_tokens.add(token)
        self._active = SessionIdentity(self._source_id, token, origin)

    def _add_marker(self, record: LogRecord) -> None:
        try:
            token = decode_session_start(record)
        except IdentityError as exc:
            self._invalid.append(str(exc))
            self._active = None
            return
        self._finish_pending()
        self._activate(token, "wire-marker")
        if self._active is not None and self._active.token == token:
            self._wire_marker_seen = True

    def _pair_key(self, event: ObservationEvent) -> Optional[tuple[SessionIdentity, int, int]]:
        if self._poisoned:
            self._invalid.append(
                "AMBIGUOUS_PAIR: source identity was poisoned by contradictory evidence"
            )
            self._incomplete.append(IdentityPair(
                key=(SessionIdentity(self._source_id or "", b"\0" * 16, "ambiguous"),
                     event.episode_id, event.logical_id),
                before=event if event.phase is SnapshotPhase.BEFORE else None,
                after=event if event.phase is SnapshotPhase.AFTER else None,
                status=IdentityPairStatus.AMBIGUOUS_PAIR,
                stable_metadata=False,
                identity_verified=False,
            ))
            return None
        if self._active is None:
            self._invalid.append(
                "IDENTITY_UNAVAILABLE: OBS_SNAPSHOT_V1 has no verified active session"
            )
            self._incomplete.append(IdentityPair(
                key=(SessionIdentity(self._source_id or "", b"\0" * 16, "none"),
                     event.episode_id, event.logical_id),
                before=event if event.phase is SnapshotPhase.BEFORE else None,
                after=event if event.phase is SnapshotPhase.AFTER else None,
                status=IdentityPairStatus.IDENTITY_UNAVAILABLE,
                stable_metadata=False,
                identity_verified=False,
            ))
            return None
        return self._active, event.episode_id, event.logical_id

    def add(self, event: Union[ObservationEvent, LogRecord, OpaqueRecord]) -> None:
        if isinstance(event, OpaqueRecord):
            self._opaque.append(event)
            return
        if isinstance(event, LogRecord):
            if event.event_type == SESSION_START_EVENT:
                self._add_marker(event)
            else:
                self._opaque.append(OpaqueRecord(event))
            return
        key = self._pair_key(event)
        if key is None:
            return
        if key in self._seen_keys:
            self._invalid.append(f"AMBIGUOUS_PAIR: reused identity key {key[1:]!r}")
            self._incomplete.append(IdentityPair(
                key=key,
                before=event if event.phase is SnapshotPhase.BEFORE else None,
                after=event if event.phase is SnapshotPhase.AFTER else None,
                status=IdentityPairStatus.AMBIGUOUS_PAIR,
                stable_metadata=False,
                identity_verified=False,
            ))
            return
        bucket = self._pending.setdefault(key, {})
        if event.phase in bucket:
            self._invalid.append("AMBIGUOUS_PAIR: duplicate phase for identity key")
            before = bucket.get(SnapshotPhase.BEFORE)
            after = bucket.get(SnapshotPhase.AFTER)
            self._incomplete.append(IdentityPair(
                key=key, before=before, after=after,
                status=IdentityPairStatus.AMBIGUOUS_PAIR,
                stable_metadata=False, identity_verified=False,
            ))
            self._pending.pop(key, None)
            return
        bucket[event.phase] = event
        if SnapshotPhase.BEFORE in bucket and SnapshotPhase.AFTER in bucket:
            before = bucket[SnapshotPhase.BEFORE]
            after = bucket[SnapshotPhase.AFTER]
            self._pending.pop(key, None)
            self._seen_keys.add(key)
            self._complete.append(IdentityPair(
                key=key, before=before, after=after,
                status=IdentityPairStatus.COMPLETE,
                stable_metadata=BASE._stable_pair(before, after),
                identity_verified=True,
            ))

    def add_bytes(self, data: bytes) -> None:
        try:
            self.add(decode_identity_record(data))
        except (BASE.SerializationError, IdentityError) as exc:
            self._invalid.append(str(exc))

    def _finish_pending(self) -> None:
        for key, bucket in sorted(self._pending.items(), key=lambda item: repr(item[0])):
            before = bucket.get(SnapshotPhase.BEFORE)
            after = bucket.get(SnapshotPhase.AFTER)
            status = (IdentityPairStatus.AFTER_MISSING if before is not None
                      else IdentityPairStatus.BEFORE_MISSING)
            self._incomplete.append(IdentityPair(
                key=key, before=before, after=after,
                status=status, stable_metadata=False,
                identity_verified=True,
            ))
        self._pending.clear()

    def finish(self) -> IdentityReport:
        self._finish_pending()
        return IdentityReport(
            complete=tuple(self._complete),
            incomplete=tuple(self._incomplete),
            invalid=tuple(self._invalid),
            opaque=tuple(self._opaque),
        )
