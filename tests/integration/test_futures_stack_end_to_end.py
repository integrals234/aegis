"""Checkpoint 1 (M2 slices 2-7) -- the whole futures stack wired together.

Product catalog -> ContractChain -> calendar/session lookup -> normalized
ingestion -> quality validation -> columnar round trip -> roll-policy
selection -> continuous series, over all three committed synthetic
families, using only production components (no reimplementation of any
stage inside this test). Each stage already has its own dedicated unit/
property/integration coverage; this file's purpose is narrower and
different: proving the stages actually compose into one working pipeline
end to end, which no single slice's own tests exercise on its own.
"""

from __future__ import annotations

import tempfile
from datetime import UTC, date, datetime
from pathlib import Path
from typing import Any

import pytest
from futures.calendars import load_calendar_registry
from futures.chain import ContractChain
from futures.columnar import query_duckdb, read_parquet, table_to_records, to_arrow_table, write_parquet
from futures.identifiers import ContractId
from futures.ingest import ingest
from futures.instruments import DEFAULT_CATALOG_PATH, load_catalog
from futures.quality import run_quality_checks
from futures.roll.fixed_days import FixedDaysPolicy
from futures.schema import SCHEMA_NAME, build_registry
from futures.series import PriceObservation, build_ratio_adjusted_series, build_unadjusted_series

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)


def _ns_to_date(event_time_ns: int) -> date:
    seconds, _ = divmod(event_time_ns, 1_000_000_000)
    return datetime.fromtimestamp(seconds, tz=UTC).date()


def _committed_chains() -> dict[tuple[str, str], ContractChain]:
    import sys

    sys.path.insert(0, str(ROOT / "tools"))
    from make_futures_fixtures import FAMILIES, load_family

    chains: dict[tuple[str, str], ContractChain] = {}
    for spec in FAMILIES:
        venue, product_root, contracts = load_family(
            ROOT / f"data_samples/futures/{spec.product_root.lower()}.json"
        )
        chain = ContractChain(venue, product_root)
        for contract in contracts:
            chain.add(contract)
        chains[(venue, product_root)] = chain
    return chains


def test_full_stack_end_to_end_across_all_three_families() -> None:
    # 1. Product catalog.
    catalog = load_catalog(ROOT, DEFAULT_CATALOG_PATH)
    assert len(catalog) == 3

    # 2. Contract chains (identity + lifecycle, M2 slice 2).
    chains = _committed_chains()
    assert set(chains) == {("SYNX", "EQX"), ("SYNX", "CLX"), ("SYNX", "SRX")}

    # 3. Calendars: every product's session_template resolves (M2 slice 3).
    calendar_registry = load_calendar_registry(ROOT)
    for product in catalog:
        assert product.session_template in calendar_registry

    # 4. Normalized ingestion (M2 slice 4).
    ingest_result = ingest(ROOT, BAR_PATHS, catalog)
    assert not ingest_result.rejections
    assert not ingest_result.out_of_order
    assert len(ingest_result.records) == 18

    # 5. Data quality (M2 slice 5) -- production ContractChain map, real detectors.
    quality_report = run_quality_checks(ingest_result.records, chains)
    assert quality_report.total == 0  # the committed fixtures are clean

    # 6. Columnar round trip (M2 slice 5 / AEGIS-230), same schema throughout.
    schema_registry = build_registry(ROOT)
    for record in ingest_result.records:
        schema_registry.validate(SCHEMA_NAME, record)
    table = to_arrow_table(ingest_result.records)
    with tempfile.TemporaryDirectory() as tmp:
        parquet_path = Path(tmp) / "bars.parquet"
        write_parquet(table, parquet_path)
        round_tripped = table_to_records(read_parquet(parquet_path))
        family_counts = query_duckdb(
            parquet_path,
            "SELECT product_root, count(*) FROM bars GROUP BY product_root ORDER BY product_root",
        )
    assert round_tripped == [dict(r) for r in ingest_result.records]
    assert family_counts == [("CLX", 6), ("EQX", 6), ("SRX", 6)]

    # 7-8. Roll-policy selection (M2 slice 6) over each family's chain, then
    # a continuous series and ratio adjustment (M2 slice 7) built from those
    # explicit selections plus prices derived from the round-tripped
    # records -- the full pipeline's own output feeding the next stage, not
    # a fresh synthetic fixture.
    # days_before_expiry=0: the committed bar fixtures carry price data for
    # only each family's *first* FAMILIES contract (M2 slice 4), so a
    # parameter that rolled the policy into a second contract within the
    # fixtures' narrow ~6-day windows would correctly raise MissingPrice --
    # this pipeline refuses to fabricate a price for a contract the data
    # never covers. 0 keeps every family's window pre-roll; the roll
    # mechanism itself is exhaustively covered by M2 slice 6's own tests.
    policy = FixedDaysPolicy(days_before_expiry=0)
    records_by_family: dict[tuple[str, str], list[dict[str, Any]]] = {}
    for record in round_tripped:
        key = (str(record["venue"]), str(record["product_root"]))
        records_by_family.setdefault(key, []).append(record)

    for key, chain in chains.items():
        family_records = records_by_family[key]
        product = catalog.get(*key)
        tick_size = product.tick_size

        dates = sorted({_ns_to_date(int(r["event_time_ns"])) for r in family_records})
        front_by_date = {d: policy.front_contract(chain, [], d) for d in dates}
        assert all(v is not None for v in front_by_date.values())

        prices = [
            PriceObservation(
                contract_id=ContractId.parse(str(r["contract_symbol"])),
                session_date=_ns_to_date(int(r["event_time_ns"])),
                price=int(r["close_ticks"]) * tick_size,
            )
            for r in family_records
        ]

        unadjusted = build_unadjusted_series(front_by_date, prices)  # type: ignore[arg-type]
        assert len(unadjusted) == len(dates)
        assert all(o.contract_id is not None for o in unadjusted)

        ratio_adjusted = build_ratio_adjusted_series(unadjusted, prices)  # type: ignore[arg-type]
        assert len(ratio_adjusted) == len(unadjusted)
        assert ratio_adjusted[-1].adjustment_factor > 0
