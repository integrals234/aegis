"""M2 slice 5 -- invariants AEGIS-025's quality report must hold for
arbitrary input, not just the handful of examples in test_futures_quality.py.

* No false positives on well-formed data (a generator that only ever
  produces valid, in-window, tick-consistent, non-duplicate records must
  yield an empty report).
* Determinism: identical input produces identical output, every time.
* Order-independence: the *set* of issues does not depend on the order
  records were supplied in (only their deterministic sort position does).
* Every reported record_identifier names a record that was actually supplied
  -- an issue is never fabricated against nothing.
"""

from __future__ import annotations

from datetime import UTC, date, datetime, timedelta
from typing import Any

import pytest
from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.quality import run_quality_checks
from hypothesis import given, settings
from hypothesis import strategies as st

pytestmark = pytest.mark.property

CONTRACT_ID = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FIRST_TRADE = date(2025, 3, 20)
EXPIRY = date(2026, 3, 20)


def _ns(day: date) -> int:
    delta = datetime.combine(day, datetime.min.time(), tzinfo=UTC) - datetime(1970, 1, 1, tzinfo=UTC)
    return (delta.days * 86400 + delta.seconds) * 1_000_000_000


def _chains() -> dict[tuple[str, str], ContractChain]:
    contract = Contract(
        contract_id=CONTRACT_ID,
        first_trade_date=FIRST_TRADE,
        last_trade_date=EXPIRY,
        expiry=EXPIRY,
        settlement_type=SettlementType.CASH,
    )
    chain = ContractChain("SYNX", "EQX")
    chain.add(contract)
    return {("SYNX", "EQX"): chain}


window_days = (EXPIRY - FIRST_TRADE).days
day_offsets = st.integers(min_value=0, max_value=window_days - 1)
price_bases = st.integers(min_value=1_000, max_value=90_000)
volumes = st.integers(min_value=1, max_value=1_000_000)


def _valid_record(offset: int, base: int, volume: int, index: int) -> dict[str, Any]:
    day = FIRST_TRADE + timedelta(days=offset)
    return {
        "schema_version": 1,
        "venue": "SYNX",
        "product_root": "EQX",
        "contract_symbol": CONTRACT_ID.canonical,
        "event_time_ns": _ns(day),
        "open_ticks": base,
        "high_ticks": base + 20,
        "low_ticks": base - 10,
        "close_ticks": base + 5,
        "volume": volume,
        "open_interest": volume,
        "settlement_price_ticks": base + 5,
        "source_sequence": index,
        "record_index": index,
    }


@given(
    entries=st.lists(
        st.tuples(day_offsets, price_bases, volumes), min_size=1, max_size=15, unique_by=lambda e: e[0]
    )
)
@settings(max_examples=100)
def test_well_formed_unique_records_never_flagged(entries: list[tuple[int, int, int]]) -> None:
    records = [_valid_record(offset, base, volume, i) for i, (offset, base, volume) in enumerate(entries)]
    report = run_quality_checks(records, _chains())
    assert report.total == 0


@given(
    entries=st.lists(
        st.tuples(day_offsets, price_bases, volumes), min_size=1, max_size=15, unique_by=lambda e: e[0]
    )
)
@settings(max_examples=50)
def test_deterministic_across_repeated_calls(entries: list[tuple[int, int, int]]) -> None:
    records = [_valid_record(offset, base, volume, i) for i, (offset, base, volume) in enumerate(entries)]
    first = run_quality_checks(records, _chains())
    second = run_quality_checks(records, _chains())
    assert first.issues == second.issues
    assert first.counts_by_type == second.counts_by_type


@given(
    entries=st.lists(
        st.tuples(day_offsets, price_bases, volumes), min_size=2, max_size=15, unique_by=lambda e: e[0]
    )
)
@settings(max_examples=50)
def test_result_independent_of_input_order(entries: list[tuple[int, int, int]]) -> None:
    records = [_valid_record(offset, base, volume, i) for i, (offset, base, volume) in enumerate(entries)]
    forward = run_quality_checks(records, _chains())
    backward = run_quality_checks(list(reversed(records)), _chains())
    assert forward.issues == backward.issues


@given(
    open_ticks=st.integers(min_value=-100, max_value=200),
    high_ticks=st.integers(min_value=-100, max_value=200),
    low_ticks=st.integers(min_value=-100, max_value=200),
    close_ticks=st.integers(min_value=-100, max_value=200),
)
@settings(max_examples=200)
def test_every_issue_identifier_matches_a_supplied_record(
    open_ticks: int, high_ticks: int, low_ticks: int, close_ticks: int
) -> None:
    day = FIRST_TRADE + timedelta(days=10)
    record = {
        "schema_version": 1,
        "venue": "SYNX",
        "product_root": "EQX",
        "contract_symbol": CONTRACT_ID.canonical,
        "event_time_ns": _ns(day),
        "open_ticks": open_ticks,
        "high_ticks": high_ticks,
        "low_ticks": low_ticks,
        "close_ticks": close_ticks,
        "volume": 1,
        "open_interest": 1,
        "settlement_price_ticks": close_ticks,
        "source_sequence": 0,
        "record_index": 0,
    }
    report = run_quality_checks([record], _chains())
    known_identifiers = {(record["contract_symbol"], record["event_time_ns"], record["record_index"])}
    for issue in report.issues:
        assert issue.record_identifier in known_identifiers
