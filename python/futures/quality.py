"""Data-quality detection over normalized `futures_bar.v1` records.

AEGIS-025 (bad-data detection) and AEGIS-014's data-quality-report half.
`python/futures/ingest.py` already rejects a record that could not be
*parsed* -- an off-tick price, a negative volume, an unregistered product.
Everything here operates on records that already survived that gate: this
module judges *quality*, not shape. AEGIS-014's acceptance wording is exactly
this distinction -- "identifies missing, stale, and contradictory values" in
data that ingestion already accepted as well-formed. Conflating "could not be
parsed" with "parsed but suspicious" would make evidence for one look like
evidence for the other; the M2 plan of record's slice 5 section is explicit
that the two must not be merged.

Nine detector categories -- the narrowest set that literally covers both
AEGIS-025's and AEGIS-014's acceptance wording (see ADR-0016's slice 5
addendum): duplicate observations, gaps, invalid prices, contradictory OHLC
relationships, impossible timestamps, contract metadata conflicts, missing
volume, missing open interest, and stale (repeated, zero-volume) observations.
A tenth category the M2 prompt names conditionally -- "contradictory
volume/OI values where frozen criterion requires" -- is deliberately not
implemented: neither AEGIS-014 nor AEGIS-025's frozen acceptance text names a
specific cross-field volume/OI rule, and inventing one would be exactly the
kind of unrequested economic assertion CLAUDE.md's roll-policy guidance warns
against elsewhere in this same milestone. A non-negative-count rule already
exists, at ingestion (shape), not here (quality) -- see ADR-0016.
"""

from __future__ import annotations

import itertools
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from typing import Any, Final

from futures.chain import ContractChain, UnknownContract
from futures.identifiers import ContractId, InvalidContractId

__all__ = [
    "DEFAULT_GAP_TOLERANCE",
    "DEFAULT_STALE_THRESHOLD",
    "IssueType",
    "QualityIssue",
    "QualityReport",
    "Severity",
    "run_quality_checks",
]

DEFAULT_GAP_TOLERANCE: Final[float] = 1.5
DEFAULT_STALE_THRESHOLD: Final[int] = 3


class IssueType(StrEnum):
    DUPLICATE_OBSERVATION = "duplicate_observation"
    GAP = "gap"
    INVALID_PRICE = "invalid_price"
    CONTRADICTORY_OHLC = "contradictory_ohlc"
    IMPOSSIBLE_TIMESTAMP = "impossible_timestamp"
    CONTRACT_METADATA_CONFLICT = "contract_metadata_conflict"
    MISSING_VOLUME = "missing_volume"
    MISSING_OPEN_INTEREST = "missing_open_interest"
    STALE_OBSERVATION = "stale_observation"


class Severity(StrEnum):
    ERROR = "error"
    WARNING = "warning"


_SEVERITY_BY_TYPE: Final[dict[IssueType, Severity]] = {
    IssueType.DUPLICATE_OBSERVATION: Severity.ERROR,
    IssueType.GAP: Severity.WARNING,
    IssueType.INVALID_PRICE: Severity.ERROR,
    IssueType.CONTRADICTORY_OHLC: Severity.ERROR,
    IssueType.IMPOSSIBLE_TIMESTAMP: Severity.ERROR,
    IssueType.CONTRACT_METADATA_CONFLICT: Severity.ERROR,
    IssueType.MISSING_VOLUME: Severity.WARNING,
    IssueType.MISSING_OPEN_INTEREST: Severity.WARNING,
    IssueType.STALE_OBSERVATION: Severity.WARNING,
}


@dataclass(frozen=True, slots=True)
class QualityIssue:
    """One detected issue, tied to the exact record it came from.

    ``record_identifier`` is ``(contract_symbol, event_time_ns, record_index)``
    -- record_index (from ingestion) makes it unique even across two records
    that otherwise share a contract and timestamp, which is itself sometimes
    the issue being reported (DUPLICATE_OBSERVATION).
    """

    issue_type: IssueType
    record_identifier: tuple[str, int, int]
    fields: tuple[str, ...]
    severity: Severity
    reason: str


@dataclass(frozen=True, slots=True)
class QualityReport:
    issues: tuple[QualityIssue, ...]
    counts_by_type: Mapping[IssueType, int]

    @property
    def total(self) -> int:
        return len(self.issues)


def _sort_key(issue: QualityIssue) -> tuple[str, int, int, str]:
    return (*issue.record_identifier, issue.issue_type.value)


def _ns_to_utc_date(event_time_ns: int) -> datetime:
    seconds, _ = divmod(event_time_ns, 1_000_000_000)
    return datetime.fromtimestamp(seconds, tz=UTC)


def _issue(
    issue_type: IssueType, record: Mapping[str, Any], fields: tuple[str, ...], reason: str
) -> QualityIssue:
    identifier = (
        str(record["contract_symbol"]),
        int(record["event_time_ns"]),
        int(record["record_index"]),
    )
    return QualityIssue(
        issue_type=issue_type,
        record_identifier=identifier,
        fields=fields,
        severity=_SEVERITY_BY_TYPE[issue_type],
        reason=reason,
    )


def _detect_duplicates(records: Sequence[Mapping[str, Any]]) -> list[QualityIssue]:
    seen: dict[tuple[str, int], Mapping[str, Any]] = {}
    issues: list[QualityIssue] = []
    for record in records:
        key = (str(record["contract_symbol"]), int(record["event_time_ns"]))
        if key in seen:
            issues.append(
                _issue(
                    IssueType.DUPLICATE_OBSERVATION,
                    record,
                    ("contract_symbol", "event_time_ns"),
                    f"another surviving record already reports an observation for "
                    f"{key[0]} at event_time_ns {key[1]}",
                )
            )
        else:
            seen[key] = record
    return issues


def _detect_price_issues(records: Sequence[Mapping[str, Any]]) -> list[QualityIssue]:
    issues: list[QualityIssue] = []
    for record in records:
        open_t, high_t, low_t, close_t = (
            record["open_ticks"],
            record["high_ticks"],
            record["low_ticks"],
            record["close_ticks"],
        )
        settlement_t = record.get("settlement_price_ticks")

        non_positive = [
            name
            for name, value in (
                ("open_ticks", open_t),
                ("high_ticks", high_t),
                ("low_ticks", low_t),
                ("close_ticks", close_t),
                ("settlement_price_ticks", settlement_t),
            )
            if value is not None and value <= 0
        ]
        if non_positive:
            issues.append(
                _issue(
                    IssueType.INVALID_PRICE,
                    record,
                    tuple(non_positive),
                    f"{', '.join(non_positive)} must be strictly positive, got "
                    f"{[record[f] for f in non_positive]}",
                )
            )

        expected_high = max(open_t, low_t, close_t)
        expected_low = min(open_t, high_t, close_t)
        if high_t < expected_high or low_t > expected_low or high_t < low_t:
            issues.append(
                _issue(
                    IssueType.CONTRADICTORY_OHLC,
                    record,
                    ("open_ticks", "high_ticks", "low_ticks", "close_ticks"),
                    f"high ({high_t}) must be >= max(open, low, close) ({expected_high}) and "
                    f"low ({low_t}) must be <= min(open, high, close) ({expected_low})",
                )
            )
    return issues


def _detect_metadata_and_timestamp_issues(
    records: Sequence[Mapping[str, Any]],
    chains: Mapping[tuple[str, str], ContractChain],
) -> list[QualityIssue]:
    issues: list[QualityIssue] = []
    for record in records:
        symbol = str(record["contract_symbol"])
        try:
            contract_id = ContractId.parse(symbol)
        except InvalidContractId as exc:
            issues.append(
                _issue(IssueType.CONTRACT_METADATA_CONFLICT, record, ("contract_symbol",), str(exc))
            )
            continue

        chain = chains.get((contract_id.venue, contract_id.product_root))
        if chain is None:
            issues.append(
                _issue(
                    IssueType.CONTRACT_METADATA_CONFLICT,
                    record,
                    ("contract_symbol",),
                    f"no contract chain registered for {contract_id.venue}:{contract_id.product_root}",
                )
            )
            continue

        try:
            contract = chain.lookup(contract_id)
        except UnknownContract as exc:
            issues.append(
                _issue(IssueType.CONTRACT_METADATA_CONFLICT, record, ("contract_symbol",), str(exc))
            )
            continue

        as_of = _ns_to_utc_date(int(record["event_time_ns"])).date()
        if not (contract.first_trade_date <= as_of <= contract.expiry):
            issues.append(
                _issue(
                    IssueType.IMPOSSIBLE_TIMESTAMP,
                    record,
                    ("event_time_ns",),
                    f"{as_of.isoformat()} is outside {contract.canonical}'s listed window "
                    f"[{contract.first_trade_date.isoformat()}, {contract.expiry.isoformat()}]",
                )
            )
    return issues


def _detect_missing_fields(records: Sequence[Mapping[str, Any]]) -> list[QualityIssue]:
    issues: list[QualityIssue] = []
    for record in records:
        if record.get("volume") is None:
            issues.append(
                _issue(IssueType.MISSING_VOLUME, record, ("volume",), "volume is null")
            )
        if record.get("open_interest") is None:
            issues.append(
                _issue(
                    IssueType.MISSING_OPEN_INTEREST, record, ("open_interest",), "open_interest is null"
                )
            )
    return issues


def _detect_gaps(
    records: Sequence[Mapping[str, Any]],
    expected_interval_ns: int,
    tolerance: float,
) -> list[QualityIssue]:
    issues: list[QualityIssue] = []
    by_contract: dict[str, list[Mapping[str, Any]]] = {}
    for record in records:
        by_contract.setdefault(str(record["contract_symbol"]), []).append(record)

    threshold_ns = int(expected_interval_ns * tolerance)
    for series in by_contract.values():
        ordered = sorted(series, key=lambda r: int(r["event_time_ns"]))
        for previous, current in itertools.pairwise(ordered):
            delta = int(current["event_time_ns"]) - int(previous["event_time_ns"])
            if delta > threshold_ns:
                issues.append(
                    _issue(
                        IssueType.GAP,
                        current,
                        ("event_time_ns",),
                        f"gap of {delta}ns since the previous observation exceeds the "
                        f"{threshold_ns}ns tolerance ({expected_interval_ns}ns expected x{tolerance})",
                    )
                )
    return issues


def _detect_stale_runs(records: Sequence[Mapping[str, Any]], threshold: int) -> list[QualityIssue]:
    issues: list[QualityIssue] = []
    by_contract: dict[str, list[Mapping[str, Any]]] = {}
    for record in records:
        by_contract.setdefault(str(record["contract_symbol"]), []).append(record)

    for series in by_contract.values():
        ordered = sorted(series, key=lambda r: int(r["event_time_ns"]))
        run_length = 0
        previous_shape: tuple[int, int, int, int] | None = None
        for record in ordered:
            shape = (record["open_ticks"], record["high_ticks"], record["low_ticks"], record["close_ticks"])
            is_zero_volume = record.get("volume") == 0
            if is_zero_volume and shape == previous_shape:
                run_length += 1
            else:
                run_length = 1 if is_zero_volume else 0
            previous_shape = shape if is_zero_volume else None
            if run_length >= threshold:
                issues.append(
                    _issue(
                        IssueType.STALE_OBSERVATION,
                        record,
                        ("open_ticks", "high_ticks", "low_ticks", "close_ticks", "volume"),
                        f"identical zero-volume OHLC repeated for {run_length} consecutive "
                        f"observations (threshold {threshold})",
                    )
                )
    return issues


def run_quality_checks(
    records: Sequence[Mapping[str, Any]],
    chains: Mapping[tuple[str, str], ContractChain],
    *,
    gap_expected_interval_ns: int | None = None,
    gap_tolerance: float = DEFAULT_GAP_TOLERANCE,
    stale_threshold: int = DEFAULT_STALE_THRESHOLD,
) -> QualityReport:
    """Run every detector over ``records`` (normalized `futures_bar.v1`).

    ``chains`` maps ``(venue, product_root)`` to the :class:`ContractChain`
    that resolves metadata and lifecycle-window conflicts -- required rather
    than inferred, so a contract this run has no chain for is reported
    (CONTRACT_METADATA_CONFLICT), never silently skipped.

    Gap detection is opt-in: pass ``gap_expected_interval_ns`` (the
    caller-known regular spacing, e.g. ``86_400_000_000_000`` for daily bars)
    to enable it. Without it, no GAP issues are produced -- a guessed cadence
    from irregular seed data is not a claim this module makes.

    Deterministic ordering throughout: never hash order.
    """
    issues: list[QualityIssue] = []
    issues.extend(_detect_duplicates(records))
    issues.extend(_detect_price_issues(records))
    issues.extend(_detect_metadata_and_timestamp_issues(records, chains))
    issues.extend(_detect_missing_fields(records))
    issues.extend(_detect_stale_runs(records, stale_threshold))
    if gap_expected_interval_ns is not None:
        issues.extend(_detect_gaps(records, gap_expected_interval_ns, gap_tolerance))

    issues.sort(key=_sort_key)

    counts: dict[IssueType, int] = dict.fromkeys(IssueType, 0)
    for issue in issues:
        counts[issue.issue_type] += 1

    return QualityReport(issues=tuple(issues), counts_by_type=counts)
