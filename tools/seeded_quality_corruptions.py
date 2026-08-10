"""One deterministic, seeded corruption fixture exercising every
`python/futures/quality.py` detector (AEGIS-025, AEGIS-014).

Shared by `tests/unit/test_futures_quality.py` and
`tools/generate_quality_evidence.py` so there is exactly one fixture, not a
test copy and a separate evidence claim that could silently drift apart --
the acceptance is "every seeded corruption is actually caught by the
production quality code", which only means something if both consumers run
the identical fixture through the identical production function.

Built on the real, committed `SYNX:EQX:2026H` contract (M2 slice 2) so this
is not synthetic-on-synthetic: the corruptions are the only invented part,
the contract identity and lifecycle window are the genuine committed fixture.
"""

from __future__ import annotations

import sys
from datetime import UTC, date, datetime
from pathlib import Path
from typing import Any, Final

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId

CONTRACT_ID: Final[ContractId] = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3)
GAP_EXPECTED_INTERVAL_NS: Final[int] = 86_400_000_000_000  # one day


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


def _clean_record(day: date, index: int, **overrides: Any) -> dict[str, Any]:
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


def build_seeded_corruptions() -> tuple[list[dict[str, Any]], dict[tuple[str, str], ContractChain]]:
    """One record (or run of records) per detector, plus a clean baseline.

    Returns ``(records, chains)`` ready for
    ``futures.quality.run_quality_checks(records, chains,
    gap_expected_interval_ns=GAP_EXPECTED_INTERVAL_NS)``.
    """
    records: list[dict[str, Any]] = []
    index = 0

    def add(record: dict[str, Any]) -> None:
        nonlocal index
        record["record_index"] = index
        record["source_sequence"] = index
        records.append(record)
        index += 1

    # Clean baseline -- proves the suite does not false-positive on good data.
    for day in (date(2026, 3, 1), date(2026, 3, 2), date(2026, 3, 3)):
        add(_clean_record(day, index, open_ticks=20000 + index, high_ticks=20010 + index,
                           low_ticks=19990 + index, close_ticks=20005 + index))

    # duplicate_observation: two records, same (contract_symbol, event_time_ns).
    # The second keeps internally-consistent OHLC (high/low widened to match)
    # so this record is a duplicate_observation and nothing else.
    add(_clean_record(date(2026, 3, 5), index))
    add(_clean_record(date(2026, 3, 5), index, open_ticks=20100, high_ticks=20110))

    # contradictory_ohlc: high below close, low above open -- internally
    # inconsistent even though every individual value is a positive integer.
    add(_clean_record(date(2026, 3, 6), index, open_ticks=20000, high_ticks=19995,
                       low_ticks=20050, close_ticks=20010))

    # invalid_price: non-positive, but internally consistent (all equal) so
    # this record does not also trip contradictory_ohlc.
    add(_clean_record(date(2026, 3, 7), index, open_ticks=-5, high_ticks=-5,
                       low_ticks=-5, close_ticks=-5, settlement_price_ticks=-5))

    # missing_volume.
    add(_clean_record(date(2026, 3, 8), index, volume=None))

    # missing_open_interest.
    add(_clean_record(date(2026, 3, 9), index, open_interest=None))

    # stale_observation: three consecutive identical zero-volume bars.
    for day in (date(2026, 3, 10), date(2026, 3, 11), date(2026, 3, 12)):
        add(_clean_record(day, index, open_ticks=21000, high_ticks=21000,
                           low_ticks=21000, close_ticks=21000, volume=0))

    # gap: an 8-day jump against a 1-day expected interval.
    add(_clean_record(date(2026, 3, 20), index))

    # impossible_timestamp: before the contract's first_trade_date.
    add(_clean_record(date(2025, 1, 1), index))

    # contract_metadata_conflict: a contract month no chain has ever listed.
    unlisted = ContractId(venue="SYNX", product_root="EQX", year=2030, month=12)
    add(_clean_record(date(2026, 3, 15), index, contract_symbol=unlisted.canonical))

    return records, _chains()
