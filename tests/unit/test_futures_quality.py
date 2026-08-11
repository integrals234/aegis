"""M2 slice 5 -- AEGIS-025 / AEGIS-014 (quality-report half): data-quality
detection over normalized `futures_bar.v1` records.

`python/futures/ingest.py` already rejects a record that cannot be *parsed*
(off-tick prices, negative volume, an unregistered product). Every fixture
here is built directly as a normalized-record dict -- not run through
ingestion -- because the whole point of this file is records that are
well-formed but *bad*: exactly the category ingestion does not judge.
"""

from __future__ import annotations

from datetime import UTC, date, datetime
from typing import Any

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.quality import IssueType, Severity, run_quality_checks

pytestmark = pytest.mark.unit

CONTRACT_ID = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)


def _ns(day: date) -> int:
    delta = datetime.combine(day, datetime.min.time(), tzinfo=UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * 1_000_000_000


def _chains() -> dict[tuple[str, str], ContractChain]:
    contract = Contract(
        contract_id=CONTRACT_ID,
        first_trade_date=date(2025, 3, 20),
        last_trade_date=date(2026, 3, 20),
        expiry=date(2026, 3, 20),
        settlement_type=SettlementType.CASH,
    )
    chain = ContractChain("SYNX", "EQX")
    chain.add(contract)
    return {("SYNX", "EQX"): chain}


def _record(day: date, index: int = 0, **overrides: Any) -> dict[str, Any]:
    record: dict[str, Any] = {
        "schema_version": 1,
        "venue": "SYNX",
        "product_root": "EQX",
        "contract_symbol": CONTRACT_ID.canonical,
        "event_time_ns": _ns(day),
        "open_ticks": 20000,
        "high_ticks": 20010,
        "low_ticks": 19990,
        "close_ticks": 20005,
        "volume": 1000,
        "open_interest": 5000,
        "settlement_price_ticks": 20005,
        "source_sequence": index,
        "record_index": index,
    }
    record.update(overrides)
    return record


def test_clean_records_produce_no_issues() -> None:
    records = [
        _record(date(2026, 3, 1), 0),
        _record(date(2026, 3, 2), 1),
        _record(date(2026, 3, 3), 2),
    ]
    report = run_quality_checks(records, _chains())
    assert report.total == 0
    assert all(count == 0 for count in report.counts_by_type.values())


def test_duplicate_observation_detected() -> None:
    records = [_record(date(2026, 3, 5), 0), _record(date(2026, 3, 5), 1, open_ticks=20100, high_ticks=20110)]
    report = run_quality_checks(records, _chains())
    duplicates = [i for i in report.issues if i.issue_type is IssueType.DUPLICATE_OBSERVATION]
    assert len(duplicates) == 1
    assert duplicates[0].record_identifier[0] == CONTRACT_ID.canonical
    assert duplicates[0].severity is Severity.ERROR


def test_no_duplicate_when_timestamps_differ() -> None:
    records = [_record(date(2026, 3, 5), 0), _record(date(2026, 3, 6), 1)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.DUPLICATE_OBSERVATION] == 0


@pytest.mark.parametrize("field", ["open_ticks", "high_ticks", "low_ticks", "close_ticks", "settlement_price_ticks"])
def test_non_positive_price_detected(field: str) -> None:
    records = [_record(date(2026, 3, 1), 0, **{field: 0})]
    report = run_quality_checks(records, _chains())
    invalid = [i for i in report.issues if i.issue_type is IssueType.INVALID_PRICE]
    assert len(invalid) == 1
    assert field in invalid[0].fields


def test_contradictory_ohlc_high_below_close_detected() -> None:
    records = [_record(date(2026, 3, 1), 0, high_ticks=19995, close_ticks=20005)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.CONTRADICTORY_OHLC] == 1


def test_contradictory_ohlc_low_above_open_detected() -> None:
    records = [_record(date(2026, 3, 1), 0, low_ticks=20050, open_ticks=20000)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.CONTRADICTORY_OHLC] == 1


def test_consistent_ohlc_not_flagged() -> None:
    records = [_record(date(2026, 3, 1), 0, open_ticks=100, high_ticks=110, low_ticks=90, close_ticks=105)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.CONTRADICTORY_OHLC] == 0


def test_impossible_timestamp_before_first_trade_date_detected() -> None:
    records = [_record(date(2025, 1, 1), 0)]  # before first_trade_date 2025-03-20
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.IMPOSSIBLE_TIMESTAMP] == 1


def test_impossible_timestamp_after_expiry_detected() -> None:
    records = [_record(date(2026, 4, 1), 0)]  # after expiry 2026-03-20
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.IMPOSSIBLE_TIMESTAMP] == 1


def test_timestamp_within_window_not_flagged() -> None:
    records = [_record(date(2026, 3, 1), 0)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.IMPOSSIBLE_TIMESTAMP] == 0


def test_unlisted_contract_month_flagged_as_metadata_conflict() -> None:
    unlisted = ContractId(venue="SYNX", product_root="EQX", year=2030, month=12)
    records = [_record(date(2026, 3, 1), 0, contract_symbol=unlisted.canonical)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.CONTRACT_METADATA_CONFLICT] == 1
    # A contract the chain has never heard of cannot also be timestamp-checked.
    assert report.counts_by_type[IssueType.IMPOSSIBLE_TIMESTAMP] == 0


def test_unregistered_product_flagged_as_metadata_conflict() -> None:
    unregistered = ContractId(venue="SYNX", product_root="ZZZ", year=2026, month=3)
    records = [_record(date(2026, 3, 1), 0, contract_symbol=unregistered.canonical, product_root="ZZZ")]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.CONTRACT_METADATA_CONFLICT] == 1


def test_missing_volume_detected() -> None:
    records = [_record(date(2026, 3, 1), 0, volume=None)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.MISSING_VOLUME] == 1
    assert report.counts_by_type[IssueType.MISSING_OPEN_INTEREST] == 0


def test_missing_open_interest_detected() -> None:
    records = [_record(date(2026, 3, 1), 0, open_interest=None)]
    report = run_quality_checks(records, _chains())
    assert report.counts_by_type[IssueType.MISSING_OPEN_INTEREST] == 1


def test_stale_run_detected_at_threshold() -> None:
    records = [
        _record(day, i, open_ticks=21000, high_ticks=21000, low_ticks=21000, close_ticks=21000, volume=0)
        for i, day in enumerate((date(2026, 3, 10), date(2026, 3, 11), date(2026, 3, 12)))
    ]
    report = run_quality_checks(records, _chains(), stale_threshold=3)
    stale = [i for i in report.issues if i.issue_type is IssueType.STALE_OBSERVATION]
    assert len(stale) == 1
    assert stale[0].record_identifier[1] == _ns(date(2026, 3, 12))


def test_stale_run_below_threshold_not_flagged() -> None:
    records = [
        _record(day, i, open_ticks=21000, high_ticks=21000, low_ticks=21000, close_ticks=21000, volume=0)
        for i, day in enumerate((date(2026, 3, 10), date(2026, 3, 11)))
    ]
    report = run_quality_checks(records, _chains(), stale_threshold=3)
    assert report.counts_by_type[IssueType.STALE_OBSERVATION] == 0


def test_nonzero_volume_run_is_not_stale() -> None:
    records = [
        _record(day, i, open_ticks=21000, high_ticks=21000, low_ticks=21000, close_ticks=21000, volume=100)
        for i, day in enumerate((date(2026, 3, 10), date(2026, 3, 11), date(2026, 3, 12)))
    ]
    report = run_quality_checks(records, _chains(), stale_threshold=3)
    assert report.counts_by_type[IssueType.STALE_OBSERVATION] == 0


def test_gap_detected_when_enabled() -> None:
    records = [_record(date(2026, 3, 1), 0), _record(date(2026, 3, 20), 1)]
    report = run_quality_checks(
        records, _chains(), gap_expected_interval_ns=86_400_000_000_000, gap_tolerance=1.5
    )
    assert report.counts_by_type[IssueType.GAP] == 1


def test_gap_not_reported_when_disabled() -> None:
    records = [_record(date(2026, 3, 1), 0), _record(date(2026, 3, 20), 1)]
    report = run_quality_checks(records, _chains())  # gap_expected_interval_ns=None
    assert report.counts_by_type[IssueType.GAP] == 0


def test_regular_daily_cadence_no_gap() -> None:
    records = [_record(date(2026, 3, 1 + i), i) for i in range(5)]
    report = run_quality_checks(records, _chains(), gap_expected_interval_ns=86_400_000_000_000)
    assert report.counts_by_type[IssueType.GAP] == 0


def test_issues_are_deterministically_ordered() -> None:
    records = [
        _record(date(2026, 3, 3), 0, volume=None),
        _record(date(2026, 3, 1), 1, open_interest=None),
        _record(date(2026, 3, 2), 2, high_ticks=1),
    ]
    report = run_quality_checks(records, _chains())
    identifiers = [i.record_identifier for i in report.issues]
    assert identifiers == sorted(identifiers)


def test_seeded_corruption_suite_catches_every_issue_type() -> None:
    """The AEGIS-025 acceptance proof: the production quality code, not a
    hardcoded claim, catches every seeded corruption type."""
    import sys
    from pathlib import Path

    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    from seeded_quality_corruptions import GAP_EXPECTED_INTERVAL_NS, build_seeded_corruptions

    records, chains = build_seeded_corruptions()
    report = run_quality_checks(records, chains, gap_expected_interval_ns=GAP_EXPECTED_INTERVAL_NS)
    for issue_type in IssueType:
        assert report.counts_by_type[issue_type] >= 1, f"{issue_type} was never caught"
