"""M2 slice 4 -- AEGIS-026 / AEGIS-014 (ingestion half): normalized
multi-market ingestion, malformed/duplicate policy, `record_index`.

The acceptance is normalization across product families plus the M2 plan of
record's exact `record_index` definition (section 7), so this file drives
both: the ingest/reject/policy behaviour, and the ordering/determinism
invariants `record_index` exists to guarantee.
"""

from __future__ import annotations

import json
import subprocess
import sys
from decimal import Decimal
from pathlib import Path

import pytest
from futures.identifiers import ContractId
from futures.ingest import IngestError, IngestPolicy, ingest
from futures.instruments import Product, ProductCatalog

pytestmark = pytest.mark.unit

ROOT = Path(__file__).resolve().parents[2]


@pytest.fixture
def catalog() -> ProductCatalog:
    product = Product(
        venue="SYNX",
        product_root="EQX",
        description="test product",
        tick_size=Decimal("0.25"),
        lot_size=1,
        multiplier=Decimal("50"),
        currency="USD",
        timezone="America/Chicago",
        session_template="synx_equity_index_rth",
    )
    return ProductCatalog([product])


CONTRACT = ContractId(venue="SYNX", product_root="EQX", year=2026, month=3).canonical


def _row(**overrides: object) -> dict[str, object]:
    row: dict[str, object] = {
        "contract_symbol": CONTRACT,
        "event_time_ns": 1_773_144_000_000_000_000,
        "open": "5000.00",
        "high": "5001.00",
        "low": "4999.50",
        "close": "5000.25",
        "settlement_price": "5000.25",
        "volume": 1000,
        "open_interest": 5000,
        "source_sequence": 0,
    }
    row.update(overrides)
    return row


def write_csv(path: Path, rows: list[dict[str, object]]) -> None:
    import csv

    columns = list(_row().keys())
    with path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=columns, lineterminator="\n")
        writer.writeheader()
        for row in rows:
            writer.writerow({k: ("" if v is None else v) for k, v in row.items()})


def write_jsonl(path: Path, rows: list[dict[str, object] | str]) -> None:
    lines = [row if isinstance(row, str) else json.dumps(row) for row in rows]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


# --------------------------------------------------------------------- basic


def test_csv_row_normalizes_correctly(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row()])
    result = ingest(ROOT, [path], catalog)
    assert len(result.records) == 1
    assert not result.rejections
    record = result.records[0]
    assert record["contract_symbol"] == CONTRACT
    assert record["open_ticks"] == 20000
    assert record["high_ticks"] == 20004
    assert record["low_ticks"] == 19998
    assert record["close_ticks"] == 20001
    assert record["settlement_price_ticks"] == 20001
    assert record["volume"] == 1000
    assert record["open_interest"] == 5000
    assert record["record_index"] == 0
    assert record["schema_version"] == 1


def test_jsonl_row_normalizes_correctly(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.jsonl"
    write_jsonl(path, [_row()])
    result = ingest(ROOT, [path], catalog)
    assert len(result.records) == 1
    assert result.records[0]["open_ticks"] == 20000


def test_unsupported_extension_raises(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.txt"
    path.write_text("nothing", encoding="utf-8")
    with pytest.raises(IngestError):
        ingest(ROOT, [path], catalog)


def test_nullable_fields_absent_become_null(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(volume=None, open_interest=None, settlement_price=None)])
    result = ingest(ROOT, [path], catalog)
    record = result.records[0]
    assert record["volume"] is None
    assert record["open_interest"] is None
    assert record["settlement_price_ticks"] is None


# ------------------------------------------------------------------- rejects


def test_unparseable_contract_symbol_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(contract_symbol="not-a-contract")])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert not result.records
    assert len(result.rejections) == 1
    assert result.rejections[0].kind == "malformed"
    assert result.rejections[0].field == "contract_symbol"


def test_unregistered_product_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    unknown = ContractId(venue="SYNX", product_root="ZZZ", year=2026, month=3).canonical
    path = tmp_path / "a.csv"
    write_csv(path, [_row(contract_symbol=unknown)])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert not result.records
    assert result.rejections[0].field == "contract_symbol"


def test_missing_event_time_ns_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(event_time_ns=None)])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "event_time_ns"


def test_non_integer_event_time_ns_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    """The naive/ambiguous timestamp case: a float-shaped value is rejected,
    never coerced."""
    path = tmp_path / "a.jsonl"
    write_jsonl(path, [json.dumps({**_row(), "event_time_ns": 1773144000.5})])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "event_time_ns"


def test_iso_timestamp_string_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(event_time_ns="2026-03-10T14:00:00Z")])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "event_time_ns"


def test_non_exact_tick_price_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(open="5000.10")])  # tick_size 0.25, not an exact multiple
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "open"


def test_negative_volume_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(volume=-5)])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "volume"


def test_negative_source_sequence_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=-1)])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].field == "source_sequence"


def test_malformed_json_line_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.jsonl"
    write_jsonl(path, ["{not valid json"])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].kind == "malformed"
    assert result.rejections[0].field == "__line__"


def test_json_array_line_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.jsonl"
    write_jsonl(path, ["[1, 2, 3]"])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert result.rejections[0].kind == "malformed"


def test_blank_jsonl_lines_are_skipped_not_rejected(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.jsonl"
    path.write_text(f"\n{json.dumps(_row())}\n\n", encoding="utf-8")
    result = ingest(ROOT, [path], catalog)
    assert len(result.records) == 1
    assert not result.rejections


# ---------------------------------------------------------------- duplicates


def test_duplicate_identity_strict_raises(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=0), _row(source_sequence=0)])
    with pytest.raises(IngestError):
        ingest(ROOT, [path], catalog, policy=IngestPolicy.STRICT)


def test_duplicate_identity_report_collects(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=0), _row(source_sequence=0)])
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert len(result.records) == 1  # first occurrence survives
    assert len(result.rejections) == 1
    assert result.rejections[0].kind == "duplicate"


def test_different_source_sequence_is_not_a_duplicate(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=0), _row(source_sequence=1)])
    result = ingest(ROOT, [path], catalog)
    assert len(result.records) == 2
    assert not result.rejections


# ------------------------------------------------------------------ ordering


def test_out_of_order_is_detected_and_observable(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(
        path,
        [
            _row(source_sequence=0, event_time_ns=200),
            _row(source_sequence=1, event_time_ns=100),  # decreases -- out of order
            _row(source_sequence=2, event_time_ns=300),
        ],
    )
    result = ingest(ROOT, [path], catalog)
    assert len(result.records) == 3  # normalization does not drop or resort
    assert len(result.out_of_order) == 1
    assert result.out_of_order[0].physical_position == 3  # header is line 1


def test_in_order_input_has_no_out_of_order_entries(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(
        path,
        [_row(source_sequence=0, event_time_ns=100), _row(source_sequence=1, event_time_ns=200)],
    )
    result = ingest(ROOT, [path], catalog)
    assert not result.out_of_order


# --------------------------------------------------------------- record_index


def test_record_index_is_contiguous_from_zero(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=i) for i in range(5)])
    result = ingest(ROOT, [path], catalog)
    assert [r["record_index"] for r in result.records] == [0, 1, 2, 3, 4]


def test_record_index_has_no_duplicates(tmp_path: Path, catalog: ProductCatalog) -> None:
    a = tmp_path / "a.csv"
    b = tmp_path / "b.csv"
    write_csv(a, [_row(source_sequence=i) for i in range(3)])
    write_csv(b, [_row(source_sequence=i) for i in range(3, 6)])
    result = ingest(ROOT, [a, b], catalog)
    indexes = [r["record_index"] for r in result.records]
    assert indexes == sorted(indexes)
    assert len(set(indexes)) == len(indexes)


def test_record_index_assigned_in_sorted_file_order(tmp_path: Path, catalog: ProductCatalog) -> None:
    """Files are processed in lexicographic path order, not argument order."""
    z_file = tmp_path / "z.csv"
    a_file = tmp_path / "a.csv"
    write_csv(z_file, [_row(source_sequence=0)])
    write_csv(a_file, [_row(source_sequence=1)])
    result = ingest(ROOT, [z_file, a_file], catalog)  # z before a in the argument list
    assert result.records[0]["source_sequence"] == 1  # a.csv sorts first
    assert result.records[1]["source_sequence"] == 0


def test_shuffled_path_order_produces_identical_result(tmp_path: Path, catalog: ProductCatalog) -> None:
    paths = []
    for i, name in enumerate(("m.csv", "a.csv", "z.csv")):
        path = tmp_path / name
        write_csv(path, [_row(source_sequence=i)])
        paths.append(path)
    forward = ingest(ROOT, paths, catalog)
    backward = ingest(ROOT, list(reversed(paths)), catalog)
    assert forward.records == backward.records


def test_repeated_ingestion_produces_identical_result(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=i) for i in range(4)])
    first = ingest(ROOT, [path], catalog)
    second = ingest(ROOT, [path], catalog)
    assert first.records == second.records


def test_malformed_record_policy_changes_indexes_deterministically(
    tmp_path: Path, catalog: ProductCatalog
) -> None:
    path = tmp_path / "a.csv"
    write_csv(
        path,
        [
            _row(source_sequence=0),
            _row(source_sequence=1, open="5000.10"),  # malformed: off-tick price
            _row(source_sequence=2),
        ],
    )
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert [r["source_sequence"] for r in result.records] == [0, 2]
    assert [r["record_index"] for r in result.records] == [0, 1]
    assert len(result.rejections) == 1


def test_duplicate_rejection_changes_indexes_deterministically(
    tmp_path: Path, catalog: ProductCatalog
) -> None:
    path = tmp_path / "a.csv"
    write_csv(
        path,
        [
            _row(source_sequence=0),
            _row(source_sequence=0),  # duplicate of the row above
            _row(source_sequence=1),
        ],
    )
    result = ingest(ROOT, [path], catalog, policy=IngestPolicy.REPORT)
    assert [r["source_sequence"] for r in result.records] == [0, 1]
    assert [r["record_index"] for r in result.records] == [0, 1]


def test_strict_reports_location_of_first_rejection(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=0), _row(source_sequence=1, volume=-1)])
    with pytest.raises(IngestError, match=r"a\.csv:3"):
        ingest(ROOT, [path], catalog, policy=IngestPolicy.STRICT)


# ------------------------------------------------------ PYTHONHASHSEED proof


def test_record_index_is_independent_of_pythonhashseed(tmp_path: Path, catalog: ProductCatalog) -> None:
    path = tmp_path / "a.csv"
    write_csv(path, [_row(source_sequence=i) for i in range(6)])

    script = f"""
import json, sys
sys.path.insert(0, {str(ROOT / "python")!r})
from decimal import Decimal
from futures.ingest import ingest
from futures.instruments import Product, ProductCatalog
from pathlib import Path

product = Product(
    venue="SYNX", product_root="EQX", description="t", tick_size=Decimal("0.25"),
    lot_size=1, multiplier=Decimal("50"), currency="USD",
    timezone="America/Chicago", session_template="synx_equity_index_rth",
)
catalog = ProductCatalog([product])
result = ingest(Path({str(ROOT)!r}), [Path({str(path)!r})], catalog)
print(json.dumps([r["record_index"] for r in result.records]))
"""
    outputs = []
    for seed in ("0", "12345"):
        proc = subprocess.run(
            [sys.executable, "-c", script],
            env={"PYTHONHASHSEED": seed, "PATH": "/usr/bin:/bin"},
            capture_output=True,
            text=True,
            check=True,
        )
        outputs.append(proc.stdout.strip())
    assert outputs[0] == outputs[1]
    assert json.loads(outputs[0]) == [0, 1, 2, 3, 4, 5]
