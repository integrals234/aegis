"""ADR-0025 -- the deterministic two-leg market-data stream builder used by
the C++ calendar-spread demo (`tools/generate_calendar_spread_stream.py`)."""

from __future__ import annotations

from datetime import date, timedelta
from decimal import Decimal

import pytest
from futures.identifiers import ContractId
from research.calendar_spread import CalendarSpreadObservation
from research.stream_builder import StreamBuildConfig, build_two_leg_stream_records, price_to_units

pytestmark = pytest.mark.unit

NEAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
FAR = ContractId(venue="SYNX", product_root="EQX", year=2026, month=6)


def _observation(as_of: date, near_price: Decimal, far_price: Decimal) -> CalendarSpreadObservation:
    return CalendarSpreadObservation(
        as_of=as_of,
        near_contract_id=NEAR,
        far_contract_id=FAR,
        near_price=near_price,
        far_price=far_price,
        roll_policy_name="FixedDaysPolicy",
        far_price_provenance="test fixture",
        contract_steps=1,
    )


def test_price_to_units_is_exact_for_two_decimal_prices() -> None:
    assert price_to_units(Decimal("5000.25")) == 500025
    assert price_to_units(Decimal("0.50")) == 50


def test_emits_one_near_and_one_far_record_per_observation_in_order() -> None:
    day = date(2026, 3, 1)
    observations = [
        _observation(day, Decimal("100.00"), Decimal("100.50")),
        _observation(day + timedelta(days=1), Decimal("101.00"), Decimal("101.55")),
    ]
    config = StreamBuildConfig(
        near_instrument_id=2001, far_instrument_id=2002, tick_spread_units=10, quote_quantity_units=100
    )

    records = build_two_leg_stream_records(observations, config)

    assert [r["leg"] for r in records] == ["near", "far", "near", "far"]
    assert [r["md_sequence"] for r in records] == [1, 1, 2, 2]
    assert records[0]["instrument_id"] == 2001
    assert records[1]["instrument_id"] == 2002


def test_bid_ask_are_symmetric_around_the_observation_price() -> None:
    """The reconstructed book's mid must reproduce the observation's own
    price exactly -- this is what lets a C++ test hand-verify the strategy's
    z-score arithmetic against the same numbers this module emits."""
    observation = _observation(date(2026, 3, 1), Decimal("100.00"), Decimal("100.60"))
    config = StreamBuildConfig(
        near_instrument_id=1, far_instrument_id=2, tick_spread_units=10, quote_quantity_units=50
    )

    near_record, far_record = build_two_leg_stream_records([observation], config)

    near_mid = (near_record["bid_price_units"] + near_record["ask_price_units"]) / 2
    far_mid = (far_record["bid_price_units"] + far_record["ask_price_units"]) / 2
    assert near_mid == price_to_units(observation.near_price)
    assert far_mid == price_to_units(observation.far_price)


def test_rejects_non_positive_tick_spread_or_quantity() -> None:
    with pytest.raises(ValueError, match="tick_spread_units"):
        StreamBuildConfig(near_instrument_id=1, far_instrument_id=2, tick_spread_units=0,
                         quote_quantity_units=1)
    with pytest.raises(ValueError, match="quote_quantity_units"):
        StreamBuildConfig(near_instrument_id=1, far_instrument_id=2, tick_spread_units=1,
                         quote_quantity_units=0)


def test_committed_demo_fixture_matches_a_fresh_regeneration(repo_root) -> None:
    """The committed `tests/unit/fixtures/participant/calendar_spread_stream.jsonl`
    must be exactly what `tools/generate_calendar_spread_stream.py` produces
    right now -- a regenerated-but-not-recommitted fixture would silently
    diverge from the C++ demo's own documented arithmetic."""
    import generate_calendar_spread_stream as generator

    fixture_path = repo_root / "tests/unit/fixtures/participant/calendar_spread_stream.jsonl"
    committed = fixture_path.read_text(encoding="utf-8")

    assert generator.main() == 0
    regenerated = fixture_path.read_text(encoding="utf-8")

    assert regenerated == committed
