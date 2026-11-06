#!/usr/bin/env python3
"""Pure host-only observability contract for SmoothAF B2-O3.1.

This module deliberately has no firmware, Legacy, VRAM, logger, or ARM
dependencies. It classifies evidence observed around one measurement; it never claims
that a stable metadata window contains a complete or independent camera frame.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, IntFlag
from typing import Optional

UINT32_MASK = 0xFFFFFFFF
UINT32_HALF = 0x80000000


class Status(str, Enum):
    STABLE_METADATA = "STABLE_METADATA"
    CHANGED_DURING_MEASUREMENT = "CHANGED_DURING_MEASUREMENT"
    INSUFFICIENT_OBSERVABILITY = "INSUFFICIENT_OBSERVABILITY"
    INVALID_ENVIRONMENT = "INVALID_ENVIRONMENT"


class Profile(str, Enum):
    CORE = "CORE"
    EXTENDED = "EXTENDED"


class RecordingClass(str, Enum):
    NONE = "NONE"
    H264 = "H264"
    CUSTOM = "CUSTOM"
    RAW = "RAW"


class Field(IntFlag):
    NONE = 0
    YUV_BUFFER = 1 << 0
    GEOMETRY = 1 << 1
    LIVEVIEW = 1 << 2
    MOVIE_MODE = 1 << 3
    RECORDING_CLASS = 1 << 4
    EDMAC = 1 << 5
    DISPLAY = 1 << 6
    EXPOSURE = 1 << 7
    MVR_FRAME = 1 << 8
    TIMESTAMP = 1 << 9


CORE_FIELDS = (
    Field.YUV_BUFFER
    | Field.GEOMETRY
    | Field.LIVEVIEW
    | Field.MOVIE_MODE
    | Field.RECORDING_CLASS
    | Field.TIMESTAMP
)
EXTENDED_FIELDS = CORE_FIELDS | Field.EDMAC | Field.DISPLAY | Field.EXPOSURE | Field.MVR_FRAME


class InvalidReason(IntFlag):
    NONE = 0
    ENVIRONMENT = 1 << 0
    CANCELLED = 1 << 1
    PANIC = 1 << 2
    REQUIRED_METADATA = 1 << 3
    GEOMETRY = 1 << 4
    TIMESTAMP = 1 << 5
    MVR_COUNTER = 1 << 6
    RECORDING_TRANSITION = 1 << 7


class MvrReadStatus(str, Enum):
    AVAILABLE = "AVAILABLE"
    UNAVAILABLE = "UNAVAILABLE"
    REFUSED = "REFUSED"


@dataclass(frozen=True)
class ExposureState:
    aperture: int
    shutter: int
    iso: int
    iso_auto: int


@dataclass(frozen=True)
class MeasurementResult:
    """Raw metric outputs plus caller-computed post flags.

    ``valid`` and ``confident`` are deliberately not inferred by this module.
    They are retained exactly as supplied by the measurement caller.
    """

    score: int
    mean: int
    sad: int
    laplacian: int
    valid: bool
    confident: bool


@dataclass(frozen=True)
class ObservationSnapshot:
    timestamp_ms: int
    yuv_buffer_address: Optional[int]
    width: Optional[int]
    pitch: Optional[int]
    height: Optional[int]
    liveview: Optional[bool]
    movie_mode: Optional[bool]
    recording_class: Optional[RecordingClass]
    edmac_write_address: Optional[int]
    display_buffer_address: Optional[int]
    exposure: Optional[ExposureState]
    mvr_frame_number: Optional[int]
    environment_valid: bool = True
    invalid_reasons: InvalidReason = InvalidReason.NONE

    @classmethod
    def from_values(cls, **values: object) -> "ObservationSnapshot":
        return cls(**values)


@dataclass(frozen=True)
class MvrReadResult:
    status: MvrReadStatus
    frame_number: Optional[int]


@dataclass(frozen=True)
class Classification:
    profile: Profile
    status: Status
    available_mask: Field
    changed_mask: Field
    missing_mask: Field
    missing_required_mask: Field
    invalid_mask: Field
    invalid_reason_mask: InvalidReason
    timestamp_delta_ms: Optional[int]
    timestamp_wrapped: bool
    timestamp_ambiguous: bool
    mvr_wrapped: bool
    mvr_non_monotonic: bool
    measurement: Optional[MeasurementResult]
    frame_identity_proven: bool = False
    frame_independence_proven: bool = False


def _u32(value: int) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise TypeError("counter must be an integer")
    if value < 0 or value > UINT32_MASK:
        raise ValueError("counter must fit uint32")
    return value


def _forward_delta(before: int, after: int) -> tuple[int, bool, bool]:
    """Return modulo delta, wrap flag, and ambiguity/non-monotonic flag."""
    before = _u32(before)
    after = _u32(after)
    delta = (after - before) & UINT32_MASK
    wrapped = after < before
    ambiguous = delta > UINT32_HALF
    return delta, wrapped, ambiguous


def pack_exposure(exposure: ExposureState) -> int:
    values = (exposure.aperture, exposure.shutter, exposure.iso, exposure.iso_auto)
    if any(not isinstance(value, int) or isinstance(value, bool) or not 0 <= value <= 0xFF
           for value in values):
        raise ValueError("exposure fields must fit uint8")
    return (exposure.aperture
            | (exposure.shutter << 8)
            | (exposure.iso << 16)
            | (exposure.iso_auto << 24))


def unpack_exposure(value: int) -> ExposureState:
    value = _u32(value)
    return ExposureState(
        aperture=value & 0xFF,
        shutter=(value >> 8) & 0xFF,
        iso=(value >> 16) & 0xFF,
        iso_auto=(value >> 24) & 0xFF,
    )


def _available(snapshot: ObservationSnapshot, profile: Profile) -> Field:
    available = Field.TIMESTAMP
    if snapshot.yuv_buffer_address is not None:
        available |= Field.YUV_BUFFER
    if (snapshot.width is not None and snapshot.pitch is not None and
            snapshot.height is not None):
        available |= Field.GEOMETRY
    if snapshot.liveview is not None:
        available |= Field.LIVEVIEW
    if snapshot.movie_mode is not None:
        available |= Field.MOVIE_MODE
    if snapshot.recording_class is not None:
        available |= Field.RECORDING_CLASS
    if profile is Profile.EXTENDED:
        if snapshot.edmac_write_address is not None:
            available |= Field.EDMAC
        if snapshot.display_buffer_address is not None:
            available |= Field.DISPLAY
        if snapshot.exposure is not None:
            available |= Field.EXPOSURE
        if snapshot.mvr_frame_number is not None:
            available |= Field.MVR_FRAME
    return available & (CORE_FIELDS if profile is Profile.CORE else EXTENDED_FIELDS)


def _required(profile: Profile) -> Field:
    # CORE is the minimum safe context. EXTENDED adds optional diagnostics;
    # absent extended fields are insufficient observability, not invalidity.
    return CORE_FIELDS


def _geometry_valid(snapshot: ObservationSnapshot) -> bool:
    return (
        snapshot.width is not None and snapshot.pitch is not None and
        snapshot.height is not None and snapshot.width > 0 and
        snapshot.height > 0 and snapshot.pitch >= snapshot.width * 2
    )


def _same_or_changed(before: object, after: object) -> bool:
    return before != after


def classify(
    before: ObservationSnapshot,
    after: ObservationSnapshot,
    profile: Profile,
    measurement: Optional[MeasurementResult] = None,
) -> Classification:
    """Classify only the evidence present in two passive snapshots.

    Precedence is strict: INVALID_ENVIRONMENT, then
    CHANGED_DURING_MEASUREMENT, then INSUFFICIENT_OBSERVABILITY, then
    STABLE_METADATA.  A stable result is never frame identity evidence.
    """
    if not isinstance(profile, Profile):
        profile = Profile(profile)

    profile_fields = CORE_FIELDS if profile is Profile.CORE else EXTENDED_FIELDS
    before_available = _available(before, profile)
    after_available = _available(after, profile)
    available_mask = before_available & after_available
    missing_mask = profile_fields & ~available_mask
    required = _required(profile)
    missing_required_mask = missing_mask & required

    changed = Field.NONE
    invalid = Field.NONE

    def compare(field: Field, left: object, right: object) -> None:
        nonlocal changed
        if field & profile_fields and left != right and field in available_mask:
            changed |= field

    compare(Field.YUV_BUFFER, before.yuv_buffer_address, after.yuv_buffer_address)
    if (Field.GEOMETRY & profile_fields and
            (before.width, before.pitch, before.height) !=
            (after.width, after.pitch, after.height) and
            Field.GEOMETRY in available_mask):
        changed |= Field.GEOMETRY
    compare(Field.LIVEVIEW, before.liveview, after.liveview)
    compare(Field.MOVIE_MODE, before.movie_mode, after.movie_mode)
    compare(Field.RECORDING_CLASS, before.recording_class, after.recording_class)
    if profile is Profile.EXTENDED:
        compare(Field.EDMAC, before.edmac_write_address, after.edmac_write_address)
        compare(Field.DISPLAY, before.display_buffer_address, after.display_buffer_address)
        compare(Field.EXPOSURE, before.exposure, after.exposure)
        compare(Field.MVR_FRAME, before.mvr_frame_number, after.mvr_frame_number)

    timestamp_delta, timestamp_wrapped, timestamp_ambiguous = _forward_delta(
        before.timestamp_ms, after.timestamp_ms
    )
    if timestamp_ambiguous:
        invalid |= Field.TIMESTAMP

    mvr_wrapped = False
    mvr_non_monotonic = False
    if (profile is Profile.EXTENDED and Field.MVR_FRAME in available_mask):
        _, mvr_wrapped, mvr_non_monotonic = _forward_delta(
            before.mvr_frame_number, after.mvr_frame_number  # type: ignore[arg-type]
        )
        if mvr_non_monotonic:
            invalid |= Field.MVR_FRAME

    invalid_reasons = before.invalid_reasons | after.invalid_reasons
    if not before.environment_valid or not after.environment_valid:
        invalid_reasons |= InvalidReason.ENVIRONMENT
    if before.liveview is False or after.liveview is False:
        invalid_reasons |= InvalidReason.ENVIRONMENT
        invalid |= Field.LIVEVIEW
    if before.movie_mode is False or after.movie_mode is False:
        invalid_reasons |= InvalidReason.ENVIRONMENT
        invalid |= Field.MOVIE_MODE
    if before.recording_class != after.recording_class:
        invalid_reasons |= InvalidReason.RECORDING_TRANSITION
        if Field.RECORDING_CLASS in available_mask:
            invalid |= Field.RECORDING_CLASS
    if not _geometry_valid(before) or not _geometry_valid(after):
        invalid_reasons |= InvalidReason.GEOMETRY
        invalid |= Field.GEOMETRY
    elif (before.width, before.pitch, before.height) != (after.width, after.pitch, after.height):
        invalid_reasons |= InvalidReason.GEOMETRY
        invalid |= Field.GEOMETRY
    if missing_required_mask:
        invalid_reasons |= InvalidReason.REQUIRED_METADATA
        invalid |= missing_required_mask
    if timestamp_ambiguous:
        invalid_reasons |= InvalidReason.TIMESTAMP
    if mvr_non_monotonic:
        invalid_reasons |= InvalidReason.MVR_COUNTER

    if invalid_reasons != InvalidReason.NONE:
        status = Status.INVALID_ENVIRONMENT
    elif changed != Field.NONE:
        status = Status.CHANGED_DURING_MEASUREMENT
    elif missing_mask != Field.NONE:
        status = Status.INSUFFICIENT_OBSERVABILITY
    else:
        status = Status.STABLE_METADATA

    return Classification(
        profile=profile,
        status=status,
        available_mask=available_mask,
        changed_mask=changed,
        missing_mask=missing_mask,
        missing_required_mask=missing_required_mask,
        invalid_mask=invalid,
        invalid_reason_mask=invalid_reasons,
        timestamp_delta_ms=timestamp_delta,
        timestamp_wrapped=timestamp_wrapped,
        timestamp_ambiguous=timestamp_ambiguous,
        mvr_wrapped=mvr_wrapped,
        mvr_non_monotonic=mvr_non_monotonic,
        measurement=measurement,
    )


@dataclass
class FakeMvrAdapter:
    """Host adapter only; it never evaluates the Legacy MVR address."""

    frame_number: Optional[int] = None
    available: bool = False

    def read(
        self,
        *,
        liveview: bool,
        movie_mode: bool,
        recording_class: RecordingClass,
    ) -> MvrReadResult:
        if not (liveview and movie_mode and recording_class is RecordingClass.H264):
            return MvrReadResult(MvrReadStatus.REFUSED, None)
        if not self.available or self.frame_number is None:
            return MvrReadResult(MvrReadStatus.UNAVAILABLE, None)
        return MvrReadResult(MvrReadStatus.AVAILABLE, _u32(self.frame_number))
