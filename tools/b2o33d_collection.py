#!/usr/bin/env python3
"""Host-only controlled-collection contract for B2-O3.3d.

This module models the external provenance required to use the unchanged V1
36-byte binary records for a short, controlled collection.  It deliberately
does not parse the production FIO TSV output and does not change firmware.

A valid artifact is an immutable, independently identified binary V1 capture
whose operator-provided manifest says that it contains one session only.  The
manifest is a containment assertion, not a generated camera identity.  A
hash proves artifact integrity; it does not prove that the artifact was not
mixed before hashing.
"""
from __future__ import annotations

import hashlib
import importlib.util
import re
import sys
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parents[1]
BASE_PATH = ROOT / "tools" / "b2o32_serialization.py"
SPEC = importlib.util.spec_from_file_location("b2o33d_v1_transport", BASE_PATH)
assert SPEC and SPEC.loader
BASE = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BASE
SPEC.loader.exec_module(BASE)

OBS_SNAPSHOT_V1_EVENT = BASE.OBS_SNAPSHOT_V1_EVENT
OBS_SESSION_START_EVENT = 25  # proposed B2-O3.3c event; not accepted by D
RECORD_SIZE = BASE.RECORD_SIZE
ObservationEvent = BASE.ObservationEvent
OpaqueRecord = BASE.OpaqueRecord
PairResult = BASE.PairResult
PairStatus = BASE.PairStatus
PairingReport = BASE.PairingReport
SnapshotPhase = BASE.SnapshotPhase


class CollectionError(ValueError):
    """Artifact or collection evidence is insufficient or contradictory."""


class ArtifactFormat(str, Enum):
    BINARY_V1 = "binary-v1"
    FIO_TSV9 = "fio-tsv9"


class SessionScope(str, Enum):
    SINGLE_SESSION = "single-session"
    MULTI_SESSION = "multi-session"
    CONCATENATED = "concatenated"
    UNKNOWN = "unknown"


class MarkerState(str, Enum):
    ABSENT = "absent"
    COMPLETE = "complete"
    PARTIAL = "partial"
    CONTRADICTORY = "contradictory"


class SessionIdentityStatus(str, Enum):
    UNAVAILABLE = "UNAVAILABLE"


class CollectionEligibility(str, Enum):
    ACCEPT = "ACCEPT"
    REJECT = "REJECT"


@dataclass(frozen=True)
class CollectionManifest:
    """Operator-supplied provenance; absent values are never synthesized."""

    format: ArtifactFormat
    original_filename: Optional[str]
    source_id: Optional[str]
    expected_sha256: Optional[str]
    mode_context: Optional[tuple[tuple[str, str], ...]]
    start_known: Optional[bool]
    end_known: Optional[bool]
    session_scope: SessionScope
    transfer_chain: tuple[str, ...]
    size_bytes: Optional[int] = None
    manifest_version: Optional[str] = "1"
    marker_state: MarkerState = MarkerState.ABSENT


@dataclass(frozen=True)
class CollectionArtifact:
    data: bytes
    manifest: CollectionManifest


@dataclass(frozen=True)
class AcceptedArtifact:
    source_id: str
    sha256: str
    original_filename: str


@dataclass(frozen=True)
class CollectionReport:
    pairing: PairingReport
    accepted: tuple[AcceptedArtifact, ...]
    rejected: tuple[str, ...]
    session_identity: SessionIdentityStatus
    eligibility: CollectionEligibility


_HEX64 = re.compile(r"^[0-9a-f]{64}$")


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _reject_if_missing(manifest: CollectionManifest) -> None:
    if manifest.manifest_version != "1":
        raise CollectionError("IDENTITY_UNAVAILABLE: unsupported manifest version")
    if manifest.format is not ArtifactFormat.BINARY_V1:
        raise CollectionError("FORMAT_UNSUPPORTED: only binary-v1 is accepted")
    if not manifest.original_filename:
        raise CollectionError("IDENTITY_UNAVAILABLE: original filename is missing")
    if not manifest.source_id:
        raise CollectionError("IDENTITY_UNAVAILABLE: source_id is missing")
    if not manifest.expected_sha256 or not _HEX64.fullmatch(manifest.expected_sha256):
        raise CollectionError("IDENTITY_UNAVAILABLE: valid SHA-256 is required")
    if not manifest.mode_context:
        raise CollectionError("IDENTITY_UNAVAILABLE: mode/context metadata is missing")
    if manifest.start_known is not True or manifest.end_known is not True:
        raise CollectionError("INCOMPLETE_ARTIFACT: known start and end are required")
    if not manifest.transfer_chain:
        raise CollectionError("IDENTITY_UNAVAILABLE: transfer provenance is missing")


def decode_binary_v1(data: bytes) -> tuple[ObservationEvent | OpaqueRecord, ...]:
    """Decode the exact binary V1 stream; no TSV decoding is provided."""

    if not isinstance(data, bytes) or len(data) == 0:
        raise CollectionError("INCOMPLETE_ARTIFACT: binary capture is empty")
    if len(data) % RECORD_SIZE:
        raise CollectionError(
            "INCOMPLETE_ARTIFACT: binary capture is not a whole-record stream"
        )
    decoded: list[ObservationEvent | OpaqueRecord] = []
    for offset in range(0, len(data), RECORD_SIZE):
        chunk = data[offset:offset + RECORD_SIZE]
        try:
            record = BASE.deserialize_record(chunk)
            if record.event_type == OBS_SESSION_START_EVENT:
                raise CollectionError(
                    "FORMAT_UNSUPPORTED: OBS_SESSION_START requires the B2-O3.3c parser"
                )
            decoded.append(BASE.decode_record(chunk))
        except CollectionError:
            raise
        except BASE.SerializationError as exc:
            raise CollectionError(
                f"INVALID_ARTIFACT: V1 record at offset {offset} is invalid"
            ) from exc
    return tuple(decoded)


def validate_artifact(artifact: CollectionArtifact) -> AcceptedArtifact:
    manifest = artifact.manifest
    _reject_if_missing(manifest)
    if manifest.session_scope is not SessionScope.SINGLE_SESSION:
        raise CollectionError(
            "AMBIGUOUS_PAIR: artifact is mixed, concatenated, or has unknown session scope"
        )
    if manifest.marker_state is not MarkerState.ABSENT:
        raise CollectionError(
            "AMBIGUOUS_PAIR: controlled V1 contract does not use partial or contradictory markers"
        )
    actual = sha256_hex(artifact.data)
    if actual != manifest.expected_sha256:
        raise CollectionError("INTEGRITY_FAILURE: SHA-256 does not match original artifact")
    if manifest.size_bytes is None:
        raise CollectionError("IDENTITY_UNAVAILABLE: expected byte length is required")
    if manifest.size_bytes != len(artifact.data):
        raise CollectionError("INTEGRITY_FAILURE: byte length does not match manifest")
    decode_binary_v1(artifact.data)
    return AcceptedArtifact(
        source_id=manifest.source_id or "",
        sha256=actual,
        original_filename=manifest.original_filename or "",
    )


class ControlledCollector:
    """Pair unchanged V1 snapshots only inside validated artifact boundaries."""

    def __init__(self) -> None:
        self._pairer = BASE.Pairer()
        self._accepted: list[AcceptedArtifact] = []
        self._source_ids: set[str] = set()
        self._hashes: set[str] = set()
        self._rejected: list[str] = []

    def add_artifact(self, artifact: CollectionArtifact) -> bool:
        try:
            accepted = validate_artifact(artifact)
        except CollectionError as exc:
            self._rejected.append(str(exc))
            return False
        if accepted.source_id in self._source_ids:
            self._rejected.append(
                f"AMBIGUOUS_PAIR: duplicate source_id {accepted.source_id!r}"
            )
            return False
        if accepted.sha256 in self._hashes:
            self._rejected.append(
                f"AMBIGUOUS_PAIR: duplicate artifact hash {accepted.sha256}"
            )
            return False
        self._source_ids.add(accepted.source_id)
        self._hashes.add(accepted.sha256)
        self._accepted.append(accepted)
        self._pairer.boundary(accepted.source_id)
        for event in decode_binary_v1(artifact.data):
            self._pairer.add(event)
        self._pairer.end_source()
        return True

    def finish(self) -> CollectionReport:
        pairing = self._pairer.finish()
        eligibility = (CollectionEligibility.ACCEPT
                       if self._accepted and not self._rejected and not pairing.invalid
                       else CollectionEligibility.REJECT)
        return CollectionReport(
            pairing=pairing,
            accepted=tuple(self._accepted),
            rejected=tuple(self._rejected),
            session_identity=SessionIdentityStatus.UNAVAILABLE,
            eligibility=eligibility,
        )
