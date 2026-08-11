"""M2 closure -- the whole futures stack wired together.

Product catalog -> ContractChain -> calendar/session lookup -> normalized
ingestion -> quality validation -> columnar round trip -> roll-policy
selection -> continuous series -> roll audit -> canonical replay order ->
HistoricalReplayFeed -> the compiled C++ binding, over all three committed
synthetic families, using only production components (no reimplementation
of any stage inside this test). Each stage already has its own dedicated
unit/property/integration coverage; this file's purpose is narrower and
different: proving the stages actually compose into one working pipeline
end to end, which no single slice's own tests exercise on its own.

Extended at slice 14 (M2 closure) from slices 2-7 to the full M2 chain.
The C++ half of replay -- ReplayEngine, the four pacing modes and
deterministic fault injection -- is exercised over the *same* canonical
order by tests/cpp/unit/test_replay_full_stack_integration.cpp and, across
process boundaries, by tests/replay/test_futures_replay_determinism.py.
The binding assertion at the end of this file is the seam that proves both
halves agree on that order rather than each being separately self-consistent.
"""

from __future__ import annotations

import json
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
from futures.replay import HistoricalReplayFeed, canonical_sort_key
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll_audit import build_roll_audit, render_human_readable, to_machine_readable
from futures.schema import SCHEMA_NAME, build_registry
from futures.series import PriceObservation, build_ratio_adjusted_series, build_unadjusted_series

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BAR_PATHS = (
    "data_samples/futures/bars/eqx.csv",
    "data_samples/futures/bars/clx.jsonl",
    "data_samples/futures/bars/srx.csv",
)


def _load_bindings():
    """The compiled extension, failing rather than skipping if absent.

    Same rule as tests/integration/test_bindings_roundtrip.py: a skipped
    binding check is not evidence (AEGIS-003).
    """
    import sys

    for preset in ("debug", "release"):
        directory = ROOT / f"build/{preset}/cpp/bindings"
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    try:
        import aegis_bindings
    except ImportError as exc:  # pragma: no cover - exercised when the build is absent
        pytest.fail(
            "the compiled extension was not found; build it with "
            f"'cmake --build --preset debug'. Underlying import error: {exc}"
        )
    return aegis_bindings


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

        # 9. Roll audit (M2 slice 8) over the same chain/policy/prices. The
        # committed windows are pre-roll by construction (see the
        # days_before_expiry=0 note above), so the audit's own correctness is
        # that it reports exactly the transitions the series contains --
        # asserted against build_unadjusted_series' own roll points rather
        # than against a number typed in here.
        audit = build_roll_audit(chain, policy, [], prices, dates)  # type: ignore[arg-type]
        assert len(audit) == sum(1 for o in unadjusted[1:] if o.is_roll_point)
        # AEGIS-023's two views must come from one data source: the
        # machine-readable form must round-trip through JSON, and the
        # human-readable form must describe the same number of rolls.
        machine = to_machine_readable(audit)
        assert json.loads(json.dumps(machine)) == machine
        human = render_human_readable(audit)
        assert human.startswith("as_of")
        assert len(human.strip().splitlines()) == len(machine) + 1  # header + one row per roll

    # 10. Canonical replay order over the pipeline's own output (M2 slices
    # 9/13).
    #
    # For these particular fixtures ingestion order already *is* canonical
    # order -- the three families' event times do not interleave -- so feeding
    # the records in pipeline order would let a broken sort (or no sort at
    # all) pass. The feed is therefore given a deterministically scrambled
    # copy, and must reproduce the pipeline's own order from it. The scramble
    # is a fixed rotation-and-reverse, not random: a test that fails only on
    # some seeds is worse than no test.
    canonical_order = [r["record_index"] for r in round_tripped]
    scrambled = list(reversed(round_tripped[3:] + round_tripped[:3]))
    assert [r["record_index"] for r in scrambled] != canonical_order  # the scramble is real

    feed = HistoricalReplayFeed(scrambled)
    replayed = list(feed)
    assert len(replayed) == len(ingest_result.records)
    assert [r["record_index"] for r in replayed] == canonical_order
    assert [canonical_sort_key(r) for r in replayed] == sorted(
        canonical_sort_key(r) for r in round_tripped
    )

    # 11. Cursor/resume in Python terms mirrors ReplayEngine's (ADR-0018).
    resumable = HistoricalReplayFeed(scrambled)
    assert resumable.cursor() is None
    midpoint_position = len(replayed) // 2
    resumable.resume_from(replayed[midpoint_position]["record_index"])
    assert [r["record_index"] for r in resumable] == canonical_order[midpoint_position + 1 :]

    # 12. The compiled C++ binding sorts the identical scrambled records into
    # the identical order via the real canonical_less (AEGIS-229). This is the
    # seam between the Python pipeline above and the C++ replay core: if the
    # two ever disagreed about what "canonical order" means, replaying this
    # pipeline's output in C++ would silently reorder it.
    bindings = _load_bindings()
    canonical_fields = ("event_time_ns", "source_sequence", "contract_symbol", "record_index")
    binding_input = [{field: record[field] for field in canonical_fields} for record in scrambled]
    assert bindings.sort_canonical(binding_input) == [
        {field: record[field] for field in canonical_fields} for record in replayed
    ]
