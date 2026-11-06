#!/usr/bin/env python3
"""Host-only OBS_SNAPSHOT_V1 transport prototype for B2-O3.2.

The module models the existing 36-byte logger record without importing or
calling production logger, Legacy, VRAM, EDMAC, MVR, or FIO code.
"""
from __future__ import annotations

import importlib.util
import struct
from collections import deque
from dataclasses import dataclass
from enum import Enum, IntEnum
from pathlib import Path
from typing import Deque, Optional, Union

ROOT = Path(__file__).resolve().parents[1]
O31_PATH = ROOT / "tools" / "b2o31_observability.py"
SPEC = importlib.util.spec_from_file_location("b2o31_observability_transport", O31_PATH)
assert SPEC and SPEC.loader
O31 = importlib.util.module_from_spec(SPEC)
import sys
sys.modules[SPEC.name] = O31
SPEC.loader.exec_module(O31)

Status = O31.Status
Profile = O31.Profile
ExposureState = O31.ExposureState
ObservationSnapshot = O31.ObservationSnapshot

RECORD_SIZE = 36
PAYLOAD_SLOTS = 6
PHYSICAL_SLOTS = 256
USABLE_SLOTS = 255
OBS_SNAPSHOT_V1_EVENT = 24
OBS_SCHEMA_VERSION = 1
UINT32_MASK = 0xFFFFFFFF
UINT16_MASK = 0xFFFF
UINT32_SENTINEL = UINT32_MASK

_RECORD = struct.Struct("<IHHI6i")


class SerializationError(ValueError):
    pass


class SnapshotPhase(IntEnum):
    BEFORE = 0
    AFTER = 1


class PairStatus(str, Enum):
    COMPLETE = "COMPLETE"
    BEFORE_MISSING = "BEFORE_MISSING"
    AFTER_MISSING = "AFTER_MISSING"


class PairTransportStatus(str, Enum):
    COMPLETE = "COMPLETE"
    BEFORE_DROPPED = "BEFORE_DROPPED"
    AFTER_DROPPED = "AFTER_DROPPED"
    BOTH_DROPPED = "BOTH_DROPPED"


class PublishAction(str, Enum):
    PUBLISH = "PUBLISH"
    SUPPRESS = "SUPPRESS"


class PublishReason(str, Enum):
    FIRST = "FIRST"
    STATUS_CHANGE = "STATUS_CHANGE"
    WINDOW_ELAPSED = "WINDOW_ELAPSED"
    MIN_WINDOW = "MIN_WINDOW"
    LAST = "LAST"


class SnapshotFlags(IntEnum):
    YUV_AVAILABLE = 1 << 0
    GEOMETRY_AVAILABLE = 1 << 1
    EDMAC_AVAILABLE = 1 << 2
    DISPLAY_AVAILABLE = 1 << 3
    EXPOSURE_AVAILABLE = 1 << 4
    MVR_AVAILABLE = 1 << 5
    TIMESTAMP_WRAPPED = 1 << 6
    MVR_WRAPPED = 1 << 7


KNOWN_FLAGS = 0xFF
EXTENDED_FLAGS = (
    int(SnapshotFlags.EDMAC_AVAILABLE)
    | int(SnapshotFlags.DISPLAY_AVAILABLE)
    | int(SnapshotFlags.EXPOSURE_AVAILABLE)
    | int(SnapshotFlags.MVR_AVAILABLE)
    | int(SnapshotFlags.MVR_WRAPPED)
)

# payload[0] control word
VERSION_SHIFT = 0
PHASE_SHIFT = 4
STATUS_SHIFT = 5
PROFILE_SHIFT = 7
FLAGS_SHIFT = 8
LOGICAL_ID_SHIFT = 16
VERSION_MASK = 0xF
PHASE_MASK = 0x1
STATUS_MASK = 0x3
PROFILE_MASK = 0x1
FLAGS_MASK = 0xFF
LOGICAL_ID_MASK = 0xFFFF
# All 32 control bits are assigned: there are no reserved bits in V1.
CONTROL_RESERVED_MASK = 0x00000000

_STATUS_TO_BITS = {
    Status.STABLE_METADATA: 0,
    Status.CHANGED_DURING_MEASUREMENT: 1,
    Status.INSUFFICIENT_OBSERVABILITY: 2,
    Status.INVALID_ENVIRONMENT: 3,
}
_BITS_TO_STATUS = {value: key for key, value in _STATUS_TO_BITS.items()}
_PROFILE_TO_BITS = {Profile.CORE: 0, Profile.EXTENDED: 1}
_BITS_TO_PROFILE = {value: key for key, value in _PROFILE_TO_BITS.items()}


@dataclass(frozen=True)
class LogRecord:
    timestamp_ms: int
    event_type: int
    episode_id: int
    sequence: int
    payload: tuple[int, int, int, int, int, int]


@dataclass(frozen=True)
class OpaqueRecord:
    record: LogRecord


@dataclass(frozen=True)
class ObservationEvent:
    timestamp_ms: int
    episode_id: int
    sequence: int
    logical_id: int
    phase: SnapshotPhase
    status: Status
    profile: Profile
    flags: int
    yuv_buffer_address: Optional[int]
    edmac_write_address: Optional[int]
    display_buffer_address: Optional[int]
    mvr_frame_number: Optional[int]
    exposure: Optional[ExposureState]

    @property
    def key(self) -> tuple[int, int]:
        return self.episode_id, self.logical_id

    def to_record(self) -> LogRecord:
        return LogRecord(
            timestamp_ms=self.timestamp_ms,
            event_type=OBS_SNAPSHOT_V1_EVENT,
            episode_id=self.episode_id,
            sequence=self.sequence,
            payload=(
                pack_control(self),
                _pack_optional_u32(self.yuv_buffer_address),
                _pack_optional_u32(self.edmac_write_address),
                _pack_optional_u32(self.display_buffer_address),
                _pack_optional_u32(self.mvr_frame_number),
                _pack_optional_exposure(self.exposure),
            ),
        )


@dataclass(frozen=True)
class PairResult:
    key: tuple[int, int]
    before: Optional[ObservationEvent]
    after: Optional[ObservationEvent]
    status: PairStatus
    stable_metadata: bool


@dataclass(frozen=True)
class PairingReport:
    complete: tuple[PairResult, ...]
    incomplete: tuple[PairResult, ...]
    duplicates: tuple[ObservationEvent, ...]
    invalid: tuple[str, ...]
    opaque: tuple[OpaqueRecord, ...]


@dataclass(frozen=True)
class ObservationCandidate:
    logical_id: int
    timestamp_ms: int
    status: Status


@dataclass(frozen=True)
class PublishDecision:
    action: PublishAction
    reason: PublishReason
    candidate: ObservationCandidate


@dataclass(frozen=True)
class TransportResult:
    status: PairTransportStatus
    before_published: bool
    after_published: bool


def _check_uint(value: int, maximum: int, name: str, *, allow_sentinel: bool = False) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise SerializationError(f"{name} must be an integer")
    if value < 0 or value > maximum:
        raise SerializationError(f"{name} exceeds its wire width")
    if not allow_sentinel and value == maximum:
        raise SerializationError(f"{name} uses the reserved sentinel")
    return value


def _to_signed(value: int) -> int:
    value &= UINT32_MASK
    return value - (1 << 32) if value & 0x80000000 else value


def _payload_bits(value: int) -> int:
    """Accept a signed int32 or an explicit uint32 bit pattern."""
    if not isinstance(value, int) or isinstance(value, bool):
        raise SerializationError("payload must be an integer")
    if value < -(1 << 31) or value > UINT32_MASK:
        raise SerializationError("payload exceeds its int32/uint32 wire width")
    return value & UINT32_MASK


def _wire_u32(value: int) -> int:
    """Normalize a signed or unsigned uint32 bit pattern without truncation."""
    if not isinstance(value, int) or isinstance(value, bool):
        raise SerializationError("payload is not an integer")
    if value < -(1 << 31) or value > UINT32_MASK:
        raise SerializationError("payload is not a uint32/int32 bit pattern")
    return value & UINT32_MASK


def _pack_optional_u32(value: Optional[int]) -> int:
    if value is None:
        return UINT32_SENTINEL
    return _check_uint(value, UINT32_MASK, "u32 value")


def _unpack_optional_u32(value: int) -> Optional[int]:
    return None if value == UINT32_SENTINEL else value


def _pack_optional_exposure(value: Optional[ExposureState]) -> int:
    if value is None:
        return UINT32_SENTINEL
    try:
        packed = O31.pack_exposure(value)
    except (TypeError, ValueError) as exc:
        raise SerializationError("invalid exposure payload") from exc
    if packed == UINT32_SENTINEL:
        raise SerializationError("packed exposure uses the reserved sentinel")
    return packed


def _unpack_optional_exposure(value: int) -> Optional[ExposureState]:
    return None if value == UINT32_SENTINEL else O31.unpack_exposure(value)


def _flags_for_event(event: ObservationEvent) -> int:
    flags = event.flags
    if flags & ~KNOWN_FLAGS:
        raise SerializationError("unknown snapshot flags")
    return flags


def pack_control(event: ObservationEvent) -> int:
    _check_uint(event.logical_id, UINT16_MASK, "logical_id")
    _validate_logical_id(event.logical_id)
    flags = _flags_for_event(event)
    try:
        status_bits = _STATUS_TO_BITS[event.status]
        profile_bits = _PROFILE_TO_BITS[event.profile]
    except KeyError as exc:
        raise SerializationError("unknown status or profile") from exc
    if event.phase not in (SnapshotPhase.BEFORE, SnapshotPhase.AFTER):
        raise SerializationError("unknown snapshot phase")
    return (
        (OBS_SCHEMA_VERSION << VERSION_SHIFT)
        | (int(event.phase) << PHASE_SHIFT)
        | (status_bits << STATUS_SHIFT)
        | (profile_bits << PROFILE_SHIFT)
        | (flags << FLAGS_SHIFT)
        | (event.logical_id << LOGICAL_ID_SHIFT)
    )


def unpack_control(control: int) -> tuple[int, SnapshotPhase, Status, Profile, int]:
    control = _wire_u32(control)
    if control & CONTROL_RESERVED_MASK:
        raise SerializationError("control contains reserved bits")
    version = (control >> VERSION_SHIFT) & VERSION_MASK
    if version != OBS_SCHEMA_VERSION:
        raise SerializationError(f"unsupported snapshot schema version: {version}")
    phase_bits = (control >> PHASE_SHIFT) & PHASE_MASK
    status_bits = (control >> STATUS_SHIFT) & STATUS_MASK
    profile_bits = (control >> PROFILE_SHIFT) & PROFILE_MASK
    flags = (control >> FLAGS_SHIFT) & FLAGS_MASK
    logical_id = (control >> LOGICAL_ID_SHIFT) & LOGICAL_ID_MASK
    if logical_id in (0, UINT16_MASK):
        raise SerializationError("logical_id value is reserved")
    if flags & ~KNOWN_FLAGS:
        raise SerializationError("unknown snapshot flags")
    try:
        phase = SnapshotPhase(phase_bits)
        status = _BITS_TO_STATUS[status_bits]
        profile = _BITS_TO_PROFILE[profile_bits]
    except (ValueError, KeyError) as exc:
        raise SerializationError("invalid control enum") from exc
    return logical_id, phase, status, profile, flags


def serialize_record(record: LogRecord) -> bytes:
    timestamp = _check_uint(record.timestamp_ms, UINT32_MASK, "timestamp", allow_sentinel=True)
    event_type = _check_uint(record.event_type, UINT16_MASK, "event_type", allow_sentinel=True)
    episode_id = _check_uint(record.episode_id, UINT16_MASK, "episode_id", allow_sentinel=True)
    sequence = _check_uint(record.sequence, UINT32_MASK, "sequence", allow_sentinel=True)
    if len(record.payload) != PAYLOAD_SLOTS:
        raise SerializationError("payload must contain six slots")
    payload = tuple(_to_signed(_payload_bits(value)) for value in record.payload)
    return _RECORD.pack(timestamp, event_type, episode_id, sequence, *payload)


def deserialize_record(data: bytes) -> LogRecord:
    if len(data) != RECORD_SIZE:
        raise SerializationError(f"record must be exactly {RECORD_SIZE} bytes")
    values = _RECORD.unpack(data)
    return LogRecord(
        timestamp_ms=values[0], event_type=values[1], episode_id=values[2],
        sequence=values[3], payload=tuple(values[4:]),
    )


def _expected_flags(event: ObservationEvent) -> int:
    return event.flags


def observation_event_from_snapshot(
    snapshot: ObservationSnapshot,
    *,
    phase: SnapshotPhase,
    episode_id: int,
    sequence: int,
    logical_id: int,
    status: Status,
    profile: Profile,
    timestamp_wrapped: bool = False,
    mvr_wrapped: bool = False,
) -> ObservationEvent:
    _check_uint(episode_id, UINT16_MASK, "episode_id", allow_sentinel=True)
    _check_uint(sequence, UINT32_MASK, "sequence", allow_sentinel=True)
    _check_uint(logical_id, UINT16_MASK, "logical_id")
    _validate_logical_id(logical_id)
    _check_uint(snapshot.timestamp_ms, UINT32_MASK, "timestamp", allow_sentinel=True)
    flags = 0
    if snapshot.yuv_buffer_address is not None:
        flags |= SnapshotFlags.YUV_AVAILABLE
    if (snapshot.width is not None and snapshot.pitch is not None and
            snapshot.height is not None):
        flags |= SnapshotFlags.GEOMETRY_AVAILABLE
    if profile is Profile.EXTENDED:
        if snapshot.edmac_write_address is not None:
            flags |= SnapshotFlags.EDMAC_AVAILABLE
        if snapshot.display_buffer_address is not None:
            flags |= SnapshotFlags.DISPLAY_AVAILABLE
        if snapshot.exposure is not None:
            flags |= SnapshotFlags.EXPOSURE_AVAILABLE
        if snapshot.mvr_frame_number is not None:
            flags |= SnapshotFlags.MVR_AVAILABLE
    if timestamp_wrapped:
        flags |= SnapshotFlags.TIMESTAMP_WRAPPED
    if mvr_wrapped:
        flags |= SnapshotFlags.MVR_WRAPPED
    return ObservationEvent(
        timestamp_ms=snapshot.timestamp_ms,
        episode_id=episode_id,
        sequence=sequence,
        logical_id=logical_id,
        phase=phase,
        status=status,
        profile=profile,
        flags=int(flags),
        yuv_buffer_address=snapshot.yuv_buffer_address,
        edmac_write_address=snapshot.edmac_write_address if profile is Profile.EXTENDED else None,
        display_buffer_address=snapshot.display_buffer_address if profile is Profile.EXTENDED else None,
        mvr_frame_number=snapshot.mvr_frame_number if profile is Profile.EXTENDED else None,
        exposure=snapshot.exposure if profile is Profile.EXTENDED else None,
    )


def decode_snapshot(record: LogRecord) -> ObservationEvent:
    if record.event_type != OBS_SNAPSHOT_V1_EVENT:
        raise SerializationError("record is not OBS_SNAPSHOT_V1")
    logical_id, phase, status, profile, flags = unpack_control(record.payload[0])
    payload = tuple(_wire_u32(value) for value in record.payload[1:])
    values = {
        "yuv_buffer_address": _unpack_optional_u32(payload[0]),
        "edmac_write_address": _unpack_optional_u32(payload[1]),
        "display_buffer_address": _unpack_optional_u32(payload[2]),
        "mvr_frame_number": _unpack_optional_u32(payload[3]),
        "exposure": _unpack_optional_exposure(payload[4]),
    }
    flag_pairs = (
        (SnapshotFlags.YUV_AVAILABLE, values["yuv_buffer_address"]),
        (SnapshotFlags.EDMAC_AVAILABLE, values["edmac_write_address"]),
        (SnapshotFlags.DISPLAY_AVAILABLE, values["display_buffer_address"]),
        (SnapshotFlags.MVR_AVAILABLE, values["mvr_frame_number"]),
        (SnapshotFlags.EXPOSURE_AVAILABLE, values["exposure"]),
    )
    for flag, value in flag_pairs:
        if bool(flags & flag) != (value is not None):
            raise SerializationError("availability flag and sentinel disagree")
    if flags & SnapshotFlags.MVR_WRAPPED and not flags & SnapshotFlags.MVR_AVAILABLE:
        raise SerializationError("MVR wrap flag requires an MVR value")
    if profile is Profile.CORE and flags & EXTENDED_FLAGS:
        raise SerializationError("CORE snapshot contains EXTENDED-only fields")
    if not (flags & SnapshotFlags.YUV_AVAILABLE) and profile is Profile.CORE:
        raise SerializationError("CORE snapshot must carry a YUV buffer")
    return ObservationEvent(
        timestamp_ms=record.timestamp_ms,
        episode_id=record.episode_id,
        sequence=record.sequence,
        logical_id=logical_id,
        phase=phase,
        status=status,
        profile=profile,
        flags=flags,
        **values,
    )


def decode_record(data: bytes) -> Union[ObservationEvent, OpaqueRecord]:
    record = deserialize_record(data)
    if record.event_type == OBS_SNAPSHOT_V1_EVENT:
        return decode_snapshot(record)
    return OpaqueRecord(record)


def next_logical_id(current: int) -> int:
    _check_uint(current, UINT16_MASK, "logical_id", allow_sentinel=True)
    return 1 if current in (0, UINT16_MASK - 1, UINT16_MASK) else current + 1


def _validate_logical_id(value: int) -> None:
    if value in (0, UINT16_MASK):
        raise SerializationError("logical_id value is reserved")


def _stable_pair(before: ObservationEvent, after: ObservationEvent) -> bool:
    required = SnapshotFlags.YUV_AVAILABLE | SnapshotFlags.GEOMETRY_AVAILABLE
    if before.profile is Profile.EXTENDED or after.profile is Profile.EXTENDED:
        required |= (SnapshotFlags.EDMAC_AVAILABLE | SnapshotFlags.DISPLAY_AVAILABLE |
                     SnapshotFlags.EXPOSURE_AVAILABLE | SnapshotFlags.MVR_AVAILABLE)
    return (
        before.status is Status.STABLE_METADATA and
        after.status is Status.STABLE_METADATA and
        before.profile is after.profile and
        (before.flags & required) == required and
        (after.flags & required) == required
    )


class Pairer:
    def __init__(self) -> None:
        self._pending: dict[tuple[int, int], dict[SnapshotPhase, ObservationEvent]] = {}
        self._complete: list[PairResult] = []
        self._duplicates: list[ObservationEvent] = []
        self._invalid: list[str] = []
        self._opaque: list[OpaqueRecord] = []

    def add(self, event: Union[ObservationEvent, OpaqueRecord]) -> None:
        if isinstance(event, OpaqueRecord):
            self._opaque.append(event)
            return
        _validate_logical_id(event.logical_id)
        key = event.key
        bucket = self._pending.setdefault(key, {})
        if event.phase in bucket:
            self._duplicates.append(event)
            return
        bucket[event.phase] = event
        if SnapshotPhase.BEFORE in bucket and SnapshotPhase.AFTER in bucket:
            before = bucket.pop(SnapshotPhase.BEFORE)
            after = bucket.pop(SnapshotPhase.AFTER)
            self._pending.pop(key, None)
            self._complete.append(PairResult(
                key=key, before=before, after=after,
                status=PairStatus.COMPLETE,
                stable_metadata=_stable_pair(before, after),
            ))

    def add_bytes(self, data: bytes) -> None:
        try:
            self.add(decode_record(data))
        except SerializationError as exc:
            self._invalid.append(str(exc))

    def finish(self) -> PairingReport:
        incomplete: list[PairResult] = []
        for key, bucket in sorted(self._pending.items()):
            before = bucket.get(SnapshotPhase.BEFORE)
            after = bucket.get(SnapshotPhase.AFTER)
            status = PairStatus.AFTER_MISSING if before is not None else PairStatus.BEFORE_MISSING
            incomplete.append(PairResult(
                key=key, before=before, after=after,
                status=status, stable_metadata=False,
            ))
        return PairingReport(
            complete=tuple(self._complete),
            incomplete=tuple(incomplete),
            duplicates=tuple(self._duplicates),
            invalid=tuple(self._invalid),
            opaque=tuple(self._opaque),
        )


class RateLimiter:
    """Deterministic selector, intentionally independent of classification and ring state."""

    def __init__(self, minimum_window_ms: int) -> None:
        if minimum_window_ms < 0 or minimum_window_ms > UINT32_MASK:
            raise ValueError("minimum_window_ms must fit uint32")
        self.minimum_window_ms = minimum_window_ms
        self._last_published: Optional[ObservationCandidate] = None
        self._pending_last: Optional[ObservationCandidate] = None

    def observe(self, candidate: ObservationCandidate) -> PublishDecision:
        _check_uint(candidate.logical_id, UINT16_MASK, "logical_id")
        _validate_logical_id(candidate.logical_id)
        _check_uint(candidate.timestamp_ms, UINT32_MASK, "timestamp", allow_sentinel=True)
        if self._last_published is None:
            self._last_published = candidate
            self._pending_last = None
            return PublishDecision(PublishAction.PUBLISH, PublishReason.FIRST, candidate)
        delta, _, ambiguous = _u32_delta(self._last_published.timestamp_ms, candidate.timestamp_ms)
        if candidate.status is not self._last_published.status:
            if not ambiguous and delta >= self.minimum_window_ms:
                self._last_published = candidate
                self._pending_last = None
                return PublishDecision(PublishAction.PUBLISH, PublishReason.STATUS_CHANGE, candidate)
            self._pending_last = candidate
            return PublishDecision(PublishAction.SUPPRESS, PublishReason.MIN_WINDOW, candidate)
        if not ambiguous and delta >= self.minimum_window_ms:
            self._last_published = candidate
            self._pending_last = None
            return PublishDecision(PublishAction.PUBLISH, PublishReason.WINDOW_ELAPSED, candidate)
        self._pending_last = candidate
        return PublishDecision(PublishAction.SUPPRESS, PublishReason.MIN_WINDOW, candidate)

    def finish(self) -> Optional[PublishDecision]:
        if self._pending_last is None:
            return None
        candidate = self._pending_last
        self._pending_last = None
        self._last_published = candidate
        return PublishDecision(PublishAction.PUBLISH, PublishReason.LAST, candidate)


def _u32_delta(before: int, after: int) -> tuple[int, bool, bool]:
    before = _check_uint(before, UINT32_MASK, "counter", allow_sentinel=True)
    after = _check_uint(after, UINT32_MASK, "counter", allow_sentinel=True)
    delta = (after - before) & UINT32_MASK
    return delta, after < before, delta > 0x80000000


class SimulatedRing:
    """Bounded nonblocking ring plus delayed consumer/flush state."""

    physical_slots = PHYSICAL_SLOTS
    usable_slots = USABLE_SLOTS
    record_size = RECORD_SIZE

    def __init__(self, *, try_lock_available: bool = True) -> None:
        self.try_lock_available = try_lock_available
        self.records: Deque[bytes] = deque()
        self.pending_flush: Deque[bytes] = deque()
        self.dropped_count = 0
        self.contention_count = 0
        self.published_count = 0
        self.flushed_count = 0

    @property
    def fill(self) -> int:
        return len(self.records) + len(self.pending_flush)

    def publish(self, data: bytes) -> bool:
        if len(data) != RECORD_SIZE:
            raise SerializationError("ring accepts only 36-byte records")
        if not self.try_lock_available:
            self.contention_count += 1
            self.dropped_count += 1
            return False
        if self.fill >= self.usable_slots:
            self.dropped_count += 1
            return False
        self.records.append(data)
        self.published_count += 1
        return True

    def reserve(self, limit: int) -> int:
        if limit < 0:
            raise ValueError("limit must be non-negative")
        count = min(limit, len(self.records))
        for _ in range(count):
            self.pending_flush.append(self.records.popleft())
        return count

    def flush(self, limit: Optional[int] = None, *, fail_after: Optional[int] = None) -> bool:
        if not self.pending_flush:
            self.reserve(limit if limit is not None else self.usable_slots)
        if limit is None:
            limit = len(self.pending_flush)
        if fail_after is not None:
            limit = min(limit, max(fail_after, 0))
        count = min(limit, len(self.pending_flush))
        for _ in range(count):
            self.pending_flush.popleft()
            self.flushed_count += 1
        return not self.pending_flush


def publish_pair(ring: SimulatedRing, before: ObservationEvent, after: ObservationEvent) -> TransportResult:
    first = ring.publish(serialize_record(before.to_record()))
    second = ring.publish(serialize_record(after.to_record()))
    if first and second:
        status = PairTransportStatus.COMPLETE
    elif first and not second:
        status = PairTransportStatus.AFTER_DROPPED
    elif not first and second:
        status = PairTransportStatus.BEFORE_DROPPED
    else:
        status = PairTransportStatus.BOTH_DROPPED
    if first != second and status not in (PairTransportStatus.BEFORE_DROPPED,
                                          PairTransportStatus.AFTER_DROPPED):
        status = PairTransportStatus.PARTIAL
    return TransportResult(status, first, second)
