"""M2 slice 2 — AEGIS-011: exchange, product and instrument metadata.

The acceptance is "schema validation and fixtures cover at least three futures
product families", so this file tests exactly those two things: that the
schema actually rejects invalid documents (not just accepts valid ones), and
that the three committed families load through one interface.
"""

from __future__ import annotations

from decimal import Decimal
from pathlib import Path

import pytest
import yaml
from futures.instruments import (
    DEFAULT_CATALOG_PATH,
    InvalidProduct,
    Product,
    ProductCatalog,
    load_catalog,
    load_schema,
)

pytestmark = pytest.mark.unit


@pytest.fixture
def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


@pytest.fixture
def schema(repo_root: Path) -> dict:
    return load_schema(repo_root)


def valid_document() -> dict:
    return {
        "schema_version": 1,
        "products": [
            {
                "venue": "SYNX",
                "product_root": "EQX",
                "description": "test product",
                "tick_size": "0.25",
                "lot_size": 1,
                "multiplier": "50",
                "currency": "USD",
                "timezone": "America/Chicago",
                "session_template": "synx_equity_index_rth",
            }
        ],
    }


# ------------------------------------------------------------- schema itself


def test_schema_accepts_a_well_formed_catalog(schema):
    import jsonschema

    jsonschema.Draft202012Validator(schema).validate(valid_document())


@pytest.mark.parametrize(
    "mutate",
    [
        lambda d: d.pop("schema_version"),
        lambda d: d.update(schema_version=2),
        lambda d: d["products"][0].pop("venue"),
        lambda d: d["products"][0].update(venue="lowercase"),
        lambda d: d["products"][0].update(tick_size=0.25),  # a JSON number, not a string
        lambda d: d["products"][0].update(tick_size="not-a-number"),
        lambda d: d["products"][0].update(lot_size=0),
        lambda d: d["products"][0].update(lot_size="1"),  # string, not integer
        lambda d: d["products"][0].update(currency="US"),
        lambda d: d["products"][0].update(currency="usd"),
        lambda d: d["products"][0].update(extra_field="not allowed"),
        lambda d: d.update(products=[]),  # minItems: 1
    ],
)
def test_schema_rejects_malformed_documents(schema, mutate):
    import jsonschema

    document = valid_document()
    mutate(document)
    with pytest.raises(jsonschema.ValidationError):
        jsonschema.Draft202012Validator(schema).validate(document)


# -------------------------------------------------------------------- Product


def test_product_parses_decimal_strings_exactly():
    """The reason tick_size/multiplier are strings: 0.1 has no exact float
    representation, and this is the value every price is a multiple of."""
    product = Product(
        venue="SYNX",
        product_root="EQX",
        description="x",
        tick_size=Decimal("0.1"),
        lot_size=1,
        multiplier=Decimal("50"),
        currency="USD",
        timezone="America/Chicago",
        session_template="t",
    )
    assert product.tick_size == Decimal("0.1")
    # The hazard this avoids: 0.1 + 0.1 + 0.1 != 0.3 in binary float.
    assert product.tick_size + product.tick_size + product.tick_size == Decimal("0.3")
    assert product.key == ("SYNX", "EQX")


@pytest.mark.parametrize(
    "kwargs",
    [
        {"venue": "synx"},
        {"venue": ""},
        {"product_root": "too long product root"},
        {"description": ""},
        {"description": "   "},
        {"currency": "US"},
        {"currency": "usd"},
        {"timezone": "Not/A/Real/Zone"},
        {"session_template": ""},
    ],
)
def test_product_constructor_rejects_invalid_fields(kwargs):
    base = {
        "venue": "SYNX",
        "product_root": "EQX",
        "description": "x",
        "tick_size": Decimal("0.25"),
        "lot_size": 1,
        "multiplier": Decimal("50"),
        "currency": "USD",
        "timezone": "America/Chicago",
        "session_template": "t",
    }
    with pytest.raises(InvalidProduct):
        Product(**{**base, **kwargs})


@pytest.mark.parametrize(
    "field, value",
    [("tick_size", Decimal("0")), ("tick_size", Decimal("-1")), ("multiplier", Decimal("0")),
     ("multiplier", Decimal("-5")), ("lot_size", 0), ("lot_size", -1)],
)
def test_product_rejects_nonpositive_numeric_fields(field, value):
    base = {
        "venue": "SYNX",
        "product_root": "EQX",
        "description": "x",
        "tick_size": Decimal("0.25"),
        "lot_size": 1,
        "multiplier": Decimal("50"),
        "currency": "USD",
        "timezone": "America/Chicago",
        "session_template": "t",
    }
    with pytest.raises(InvalidProduct):
        Product(**{**base, field: value})


def test_boolean_lot_size_is_not_accepted():
    """`True == 1` in Python; an unguarded int check would silently accept it."""
    base = {
        "venue": "SYNX",
        "product_root": "EQX",
        "description": "x",
        "tick_size": Decimal("0.25"),
        "lot_size": True,
        "multiplier": Decimal("50"),
        "currency": "USD",
        "timezone": "America/Chicago",
        "session_template": "t",
    }
    with pytest.raises(InvalidProduct):
        Product(**base)


# --------------------------------------------------------------- catalog


def test_catalog_rejects_duplicate_keys():
    product = Product(
        venue="SYNX",
        product_root="EQX",
        description="x",
        tick_size=Decimal("0.25"),
        lot_size=1,
        multiplier=Decimal("50"),
        currency="USD",
        timezone="America/Chicago",
        session_template="t",
    )
    with pytest.raises(InvalidProduct, match="duplicate"):
        ProductCatalog([product, product])


def test_catalog_get_and_lookup_miss():
    product = Product(
        venue="SYNX",
        product_root="EQX",
        description="x",
        tick_size=Decimal("0.25"),
        lot_size=1,
        multiplier=Decimal("50"),
        currency="USD",
        timezone="America/Chicago",
        session_template="t",
    )
    catalog = ProductCatalog([product])
    assert catalog.get("SYNX", "EQX") is product
    with pytest.raises(InvalidProduct):
        catalog.get("SYNX", "NOPE")


def test_catalog_iteration_is_sorted_not_insertion_order():
    """A catalog built in Z, A order must still iterate A before Z: the order
    is a property of product identity, not of construction order."""
    a = Product(
        venue="SYNX", product_root="AAA", description="x", tick_size=Decimal("1"),
        lot_size=1, multiplier=Decimal("1"), currency="USD", timezone="UTC",
        session_template="t",
    )
    z = Product(
        venue="SYNX", product_root="ZZZ", description="x", tick_size=Decimal("1"),
        lot_size=1, multiplier=Decimal("1"), currency="USD", timezone="UTC",
        session_template="t",
    )
    catalog = ProductCatalog([z, a])
    assert [p.key for p in catalog] == [("SYNX", "AAA"), ("SYNX", "ZZZ")]


# --------------------------------------------- the committed catalog itself


def test_committed_catalog_loads_and_covers_three_families(repo_root):
    """AEGIS-011's acceptance, literally: fixtures cover >= 3 product families."""
    catalog = load_catalog(repo_root, DEFAULT_CATALOG_PATH)
    assert len(catalog) >= 3
    roots = {product.product_root for product in catalog}
    assert roots == {"EQX", "CLX", "SRX"}
    for product in catalog:
        assert product.venue == "SYNX"  # synthetic venue, never a real one


def test_committed_catalog_yaml_is_schema_valid(repo_root, schema):
    import jsonschema

    document = yaml.safe_load((repo_root / DEFAULT_CATALOG_PATH).read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator(schema).validate(document)


def test_load_catalog_rejects_unsupported_schema_version(repo_root, tmp_path):
    bad = tmp_path / "products.yaml"
    document = valid_document()
    document["schema_version"] = 99
    bad.write_text(yaml.safe_dump(document), encoding="utf-8")
    # An absolute path overrides root entirely under pathlib's `/` operator,
    # so load_catalog(repo_root, <absolute path>) reads exactly `bad` while
    # still resolving the schema from repo_root.
    assert bad.is_absolute()
    with pytest.raises(InvalidProduct, match="schema_version"):
        load_catalog(repo_root, str(bad))


def test_load_catalog_missing_file(repo_root):
    with pytest.raises(InvalidProduct, match="not found"):
        load_catalog(repo_root, "configs/futures/does_not_exist.yaml")
