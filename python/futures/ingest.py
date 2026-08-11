"""Multi-market normalized ingestion and `record_index` assignment.

AEGIS-026 (one normalized interface for every product family) and AEGIS-014's
ingestion half. Converts raw CSV/JSON-Lines input rows into `futures_bar.v1`
records (`python/futures/schema.py`), assigning the deterministic
`record_index` tie-breaker exactly per the M2 plan of record's canonical
replay order, section 7.

# Scope boundary with quality.py (M2 slice 5)

This module normalizes *shape*: is a field present, well-typed, and on the
integer tick grid. It does not judge market-data *quality* -- a negative
price, a suspicious gap, a stale repeat are valid, well-formed records that
`python/futures/quality.py` (slice 5) evaluates separately. Conflating the two
would make "parsed but bad" indistinguishable from "could not be parsed",
which the M2 plan of record's Slice 5 section explicitly warns against.

# `record_index`, exactly

1. Input paths are made repository-relative with POSIX separators and
   deduplicated, then **sorted lexicographically** -- this is what makes the
   result independent of the order the caller happened to list them in.
2. Each row keeps its physical position: the 1-based CSV/JSONL line number.
3. Rows are normalized and the malformed/duplicate policy is applied, in
   `(source_file, physical_position)` order.
4. `record_index` is assigned to the **surviving** records, in that same
   order, contiguously from 0. Rejected records are recorded separately by
   `(source_file, physical_position)`; nothing is silently dropped or
   silently repaired.

It is persisted as an explicit field on every normalized record and is never
recomputed downstream (replay reads it, per the M2 plan of record).

# Input record shape

Raw rows are almost the normalized shape, but with `contract_symbol` as the
canonical `VENUE:ROOT:YYYYM` string (`futures.identifiers.ContractId.parse`)
and decimal-string prices rather than integer ticks. `event_time_ns` must
already be an explicit UTC integer -- this module does not parse or infer
timestamps, so a non-integer value is rejected outright as the "naive/
ambiguous timestamp" case. Vendor-specific alternate symbol spellings are out
of scope: only the canonical form parses, matching `identifiers.py`'s own
documented boundary.
"""

from __future__ import annotations

import csv
import json
from collections.abc import Iterator, Mapping, Sequence
from dataclasses import dataclass
from decimal import Decimal, InvalidOperation
from enum import StrEnum
from pathlib import Path
from typing import Any

from data.schema_registry import SchemaError, SchemaRegistry

from futures.identifiers import ContractId, InvalidContractId
from futures.instruments import InvalidProduct, Product, ProductCatalog
from futures.schema import SCHEMA_NAME, SCHEMA_VERSION, build_registry

__all__ = [
    "IngestError",
    "IngestPolicy",
    "IngestResult",
    "RecordLocation",
    "Rejection",
    "ingest",
]


class IngestPolicy(StrEnum):
    """How ingestion reacts to a malformed or duplicate record.

    STRICT (the default) raises on the very first rejection encountered, in
    deterministic `(source_file, physical_position)` order -- the same
    rejection is reported regardless of what order the caller listed input
    paths in. REPORT collects every rejection instead and excludes those
    records from the surviving set; ingestion never silently drops a record
    under either policy.
    """

    STRICT = "strict"
    REPORT = "report"


class IngestError(ValueError):
    """STRICT policy: the first malformed or duplicate record encountered."""


@dataclass(frozen=True, slots=True)
class RecordLocation:
    """Where a raw record came from: a repository-relative POSIX path and a
    physical position (1-based line number for CSV/JSONL)."""

    source_file: str
    physical_position: int

    def __str__(self) -> str:
        return f"{self.source_file}:{self.physical_position}"


@dataclass(frozen=True, slots=True)
class Rejection:
    """One record that did not survive -- malformed or duplicate, never
    silently absorbed. `kind` distinguishes the two for callers that need to
    tell them apart without parsing `reason` prose."""

    location: RecordLocation
    kind: str  # "malformed" | "duplicate"
    field: str
    reason: str


@dataclass(frozen=True, slots=True)
class IngestResult:
    records: tuple[dict[str, Any], ...]
    rejections: tuple[Rejection, ...]
    out_of_order: tuple[RecordLocation, ...]


class _FieldError(Exception):
    """Internal: one field of one raw record failed normalization."""

    def __init__(self, field: str, reason: str) -> None:
        super().__init__(f"{field}: {reason}")
        self.field = field
        self.reason = reason


def repo_relative_posix(root: Path, path: Path) -> str:
    """Repository-relative POSIX path, where applicable (M2 plan of record
    section 7). A path outside ``root`` -- a test fixture in a temp
    directory, say -- falls back to its resolved absolute POSIX form, which
    is still a stable, sortable, deduplicatable identity; it simply is not
    repository-relative."""
    resolved = (path if path.is_absolute() else (root / path)).resolve()
    try:
        return resolved.relative_to(root.resolve()).as_posix()
    except ValueError:
        return resolved.as_posix()


def _read_csv_rows(path: Path) -> Iterator[tuple[int, dict[str, Any] | None, str | None]]:
    with path.open("r", encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(handle)
        if reader.fieldnames is None:
            return
        for row in reader:
            if None in row:  # extra, unnamed columns -- csv.DictReader's own signal
                yield reader.line_num, None, f"row has more fields than the header ({row[None]!r})"
                continue
            yield reader.line_num, dict(row), None


def _read_jsonl_rows(path: Path) -> Iterator[tuple[int, dict[str, Any] | None, str | None]]:
    text = path.read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            payload = json.loads(line)
        except ValueError as exc:
            yield lineno, None, f"not valid JSON: {exc}"
            continue
        if not isinstance(payload, dict):
            yield lineno, None, "line is not a JSON object"
            continue
        yield lineno, payload, None


def _read_rows(path: Path) -> Iterator[tuple[int, dict[str, Any] | None, str | None]]:
    suffix = path.suffix.lower()
    if suffix == ".csv":
        yield from _read_csv_rows(path)
    elif suffix in (".jsonl", ".ndjson"):
        yield from _read_jsonl_rows(path)
    else:
        raise IngestError(f"{path}: unsupported input format {suffix!r} (expected .csv or .jsonl)")


def _require_int(raw: Mapping[str, Any], field: str) -> int:
    """An explicit integer, never inferred from a float or ambiguous string.

    Accepts a native ``int`` (JSONL) or a string of pure decimal digits
    (CSV, which has no native types). Anything else -- a float, a decimal
    string, an ISO timestamp -- is exactly the "naive/ambiguous timestamp"
    case this module refuses to guess at.
    """
    if field not in raw or raw[field] in (None, ""):
        raise _FieldError(field, "is required")
    value = raw[field]
    if isinstance(value, bool):
        raise _FieldError(field, f"must be an integer, got a bool ({value!r})")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.lstrip("-").isdigit():
        return int(value)
    raise _FieldError(field, f"must be an explicit integer, got {value!r}")


def _optional_int(raw: Mapping[str, Any], field: str) -> int | None:
    if field not in raw or raw[field] in (None, ""):
        return None
    return _require_int(raw, field)


def _price_to_ticks(raw: Mapping[str, Any], field: str, tick_size: Decimal) -> int:
    if field not in raw or raw[field] in (None, ""):
        raise _FieldError(field, "is required")
    value = raw[field]
    if not isinstance(value, str):
        raise _FieldError(field, f"must be a decimal string, got {type(value).__name__}")
    try:
        decimal_price = Decimal(value)
    except InvalidOperation as exc:
        raise _FieldError(field, f"is not a valid decimal string: {value!r}") from exc
    quotient = decimal_price / tick_size
    if quotient != quotient.to_integral_value():
        raise _FieldError(
            field, f"{value} is not an exact multiple of the product's tick_size ({tick_size})"
        )
    return int(quotient)


def _optional_price_to_ticks(raw: Mapping[str, Any], field: str, tick_size: Decimal) -> int | None:
    if field not in raw or raw[field] in (None, ""):
        return None
    return _price_to_ticks(raw, field, tick_size)


def _normalize_raw(raw: Mapping[str, Any], catalog: ProductCatalog) -> dict[str, Any]:
    """Raise :class:`_FieldError` naming the offending field, or return a
    normalized record (without `schema_version`/`record_index` -- the caller
    adds those once the record's place in the survivor order is known)."""
    symbol = raw.get("contract_symbol")
    if not isinstance(symbol, str):
        raise _FieldError("contract_symbol", f"must be a string, got {type(symbol).__name__}")
    try:
        contract_id = ContractId.parse(symbol)
    except InvalidContractId as exc:
        raise _FieldError("contract_symbol", str(exc)) from exc

    try:
        product: Product = catalog.get(contract_id.venue, contract_id.product_root)
    except InvalidProduct as exc:
        raise _FieldError("contract_symbol", f"no product registered: {exc}") from exc

    event_time_ns = _require_int(raw, "event_time_ns")
    source_sequence = _require_int(raw, "source_sequence")
    if event_time_ns < 0:
        raise _FieldError("event_time_ns", f"must be >= 0, got {event_time_ns}")
    if source_sequence < 0:
        raise _FieldError("source_sequence", f"must be >= 0, got {source_sequence}")

    tick_size = product.tick_size
    open_ticks = _price_to_ticks(raw, "open", tick_size)
    high_ticks = _price_to_ticks(raw, "high", tick_size)
    low_ticks = _price_to_ticks(raw, "low", tick_size)
    close_ticks = _price_to_ticks(raw, "close", tick_size)
    settlement_ticks = _optional_price_to_ticks(raw, "settlement_price", tick_size)

    volume = _optional_int(raw, "volume")
    open_interest = _optional_int(raw, "open_interest")
    if volume is not None and volume < 0:
        raise _FieldError("volume", f"must be >= 0 or absent, got {volume}")
    if open_interest is not None and open_interest < 0:
        raise _FieldError("open_interest", f"must be >= 0 or absent, got {open_interest}")

    return {
        "venue": contract_id.venue,
        "product_root": contract_id.product_root,
        "contract_symbol": contract_id.canonical,
        "event_time_ns": event_time_ns,
        "open_ticks": open_ticks,
        "high_ticks": high_ticks,
        "low_ticks": low_ticks,
        "close_ticks": close_ticks,
        "volume": volume,
        "open_interest": open_interest,
        "settlement_price_ticks": settlement_ticks,
        "source_sequence": source_sequence,
    }


def ingest(
    root: Path,
    paths: Sequence[str | Path],
    catalog: ProductCatalog,
    policy: IngestPolicy = IngestPolicy.STRICT,
    registry: SchemaRegistry | None = None,
) -> IngestResult:
    """Ingest every row of every path into normalized `futures_bar.v1` records.

    ``paths`` may be given in any order and may contain duplicates -- both
    are normalized away before anything is read, which is what makes the
    result independent of the caller's argument order.
    """
    if registry is None:
        registry = build_registry(root)

    unique_files = sorted({repo_relative_posix(root, Path(p)) for p in paths})

    prepared: list[tuple[RecordLocation, dict[str, Any] | None, str | None]] = []
    for rel in unique_files:
        for lineno, raw, parse_error in _read_rows(root / rel):
            location = RecordLocation(source_file=rel, physical_position=lineno)
            prepared.append((location, raw, parse_error))

    survivors: list[dict[str, Any]] = []
    rejections: list[Rejection] = []
    out_of_order: list[RecordLocation] = []
    seen_identity: dict[tuple[str, int], RecordLocation] = {}
    previous_key: tuple[int, int] | None = None

    def reject(location: RecordLocation, kind: str, field: str, reason: str) -> None:
        rejection = Rejection(location=location, kind=kind, field=field, reason=reason)
        if policy is IngestPolicy.STRICT:
            raise IngestError(f"{location}: [{kind}] {field}: {reason}")
        rejections.append(rejection)

    for location, raw, parse_error in prepared:
        if parse_error is not None:
            reject(location, "malformed", "__line__", parse_error)
            continue
        assert raw is not None  # parse_error is None => _read_rows always supplied a dict

        try:
            normalized = _normalize_raw(raw, catalog)
        except _FieldError as exc:
            reject(location, "malformed", exc.field, exc.reason)
            continue

        identity = (normalized["contract_symbol"], normalized["source_sequence"])
        if identity in seen_identity:
            first = seen_identity[identity]
            reject(
                location,
                "duplicate",
                "source_sequence",
                f"duplicate (contract_symbol, source_sequence) {identity}; first seen at {first}",
            )
            continue
        seen_identity[identity] = location

        candidate = dict(normalized)
        candidate["schema_version"] = SCHEMA_VERSION
        try:
            registry.validate(SCHEMA_NAME, {**candidate, "record_index": 0})
        except SchemaError as exc:  # pragma: no cover - defensive; a real defect, not bad input
            raise IngestError(f"{location}: internal schema defect: {exc}") from exc

        current_key = (normalized["event_time_ns"], normalized["source_sequence"])
        if previous_key is not None and current_key < previous_key:
            out_of_order.append(location)
        previous_key = current_key

        survivors.append(candidate)

    records = tuple(
        {**record, "record_index": index} for index, record in enumerate(survivors)
    )
    return IngestResult(records=records, rejections=tuple(rejections), out_of_order=tuple(out_of_order))
