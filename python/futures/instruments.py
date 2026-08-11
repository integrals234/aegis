"""Exchange, product and instrument metadata (AEGIS-011).

One schema, ``configs/schemas/futures_product.v1.json``, is the only statement
of what a valid product is — the same discipline ``python/common/config.py``
uses for AEGIS-231, so a document written against a future schema version is
rejected rather than silently reinterpreted.

Scope is metadata only: venue, product root, tick size, lot size, multiplier,
currency, timezone and a session-template *reference*. Trading-session
calendars are AEGIS-013 (M2 slice 3) and do not exist yet, so
``session_template`` is opaque here — nothing in this module may interpret it.
Individual contracts, expiry and lifecycle are AEGIS-012, in
:mod:`futures.contracts`.

``tick_size`` and ``multiplier`` are :class:`~decimal.Decimal`, parsed from the
decimal *strings* the schema requires, not from JSON/YAML numbers. A YAML float
loses exactness on values like ``0.1``, and these are the values every price
and every notional calculation on the product is a multiple of — the
imprecision would be inherited by everything downstream, including the eventual
projection onto M1's integer tick/lot grid (``cpp/exchange/order_book/instrument.hpp``).
"""

from __future__ import annotations

import json
import re
import zoneinfo
from collections.abc import Iterable, Iterator, Mapping
from dataclasses import dataclass
from decimal import Decimal
from pathlib import Path
from typing import Any, Final

import jsonschema
import yaml

__all__ = [
    "DEFAULT_CATALOG_PATH",
    "SCHEMA_PATH",
    "SCHEMA_VERSION",
    "InvalidProduct",
    "Product",
    "ProductCatalog",
    "load_catalog",
]

SCHEMA_VERSION: Final[int] = 1
SCHEMA_PATH: Final[str] = "configs/schemas/futures_product.v1.json"
DEFAULT_CATALOG_PATH: Final[str] = "configs/futures/products.yaml"

_TOKEN: Final[re.Pattern[str]] = re.compile(r"^[A-Z0-9]{1,12}$")
_CURRENCY: Final[re.Pattern[str]] = re.compile(r"^[A-Z]{3}$")


class InvalidProduct(ValueError):
    """A product catalog or a single product entry failed validation."""


@dataclass(frozen=True, slots=True)
class Product:
    """One product's metadata — the shared defaults every contract of that
    product inherits (AEGIS-011).

    Frozen and hashable, like :class:`futures.identifiers.ContractId`, so a
    catalog can be built once and shared without defensive copies.
    """

    venue: str
    product_root: str
    description: str
    tick_size: Decimal
    lot_size: int
    multiplier: Decimal
    currency: str
    timezone: str
    session_template: str

    def __post_init__(self) -> None:
        for name, value in (("venue", self.venue), ("product_root", self.product_root)):
            if not isinstance(value, str) or not _TOKEN.match(value):
                raise InvalidProduct(
                    f"{name} must be 1-12 upper-case alphanumerics, got {value!r}"
                )
        if not isinstance(self.description, str) or not self.description.strip():
            raise InvalidProduct("description must be non-empty")
        if not isinstance(self.tick_size, Decimal) or self.tick_size <= 0:
            raise InvalidProduct(f"tick_size must be a positive Decimal, got {self.tick_size!r}")
        if (
            not isinstance(self.lot_size, int)
            or isinstance(self.lot_size, bool)
            or self.lot_size < 1
        ):
            raise InvalidProduct(f"lot_size must be an int >= 1, got {self.lot_size!r}")
        if not isinstance(self.multiplier, Decimal) or self.multiplier <= 0:
            raise InvalidProduct(
                f"multiplier must be a positive Decimal, got {self.multiplier!r}"
            )
        if not isinstance(self.currency, str) or not _CURRENCY.match(self.currency):
            raise InvalidProduct(
                f"currency must be a 3-letter ISO-4217-shaped code, got {self.currency!r}"
            )
        if self.timezone not in zoneinfo.available_timezones():
            raise InvalidProduct(
                f"timezone {self.timezone!r} is not present in the installed tz database"
            )
        if not isinstance(self.session_template, str) or not self.session_template.strip():
            raise InvalidProduct("session_template must be non-empty")

    @property
    def key(self) -> tuple[str, str]:
        """The catalog's lookup key: ``(venue, product_root)``."""
        return (self.venue, self.product_root)


class ProductCatalog:
    """An immutable collection of :class:`Product`, keyed by ``(venue, product_root)``.

    Construction validates every product and rejects a duplicate key outright
    — two entries claiming the same identity is a catalog authoring error, not
    a "last one wins" situation a loader should paper over.
    """

    def __init__(self, products: Iterable[Product]) -> None:
        by_key: dict[tuple[str, str], Product] = {}
        for product in products:
            if product.key in by_key:
                raise InvalidProduct(
                    f"duplicate product {product.key[0]}:{product.key[1]} in catalog"
                )
            by_key[product.key] = product
        self._by_key = by_key

    def __len__(self) -> int:
        return len(self._by_key)

    def __iter__(self) -> Iterator[Product]:
        # Sorted, not insertion order: a catalog built from a dict comprehension
        # or re-ordered YAML must still iterate identically.
        return iter(sorted(self._by_key.values(), key=lambda p: p.key))

    def __contains__(self, key: tuple[str, str]) -> bool:
        return key in self._by_key

    def get(self, venue: str, product_root: str) -> Product:
        try:
            return self._by_key[(venue, product_root)]
        except KeyError:
            raise InvalidProduct(f"no product registered for {venue}:{product_root}") from None


def load_schema(root: Path) -> dict[str, Any]:
    path = root / SCHEMA_PATH
    if not path.exists():
        raise InvalidProduct(f"product schema not found at {SCHEMA_PATH}")
    schema: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return schema


def _describe(error: jsonschema.ValidationError) -> str:
    location = ".".join(str(part) for part in error.absolute_path) or "(root)"
    return f"{location}: {error.message}"


def _validate(document: Mapping[str, Any], schema: Mapping[str, Any]) -> None:
    """Validate against the schema, reporting every problem rather than the first.

    Mirrors ``python/common/config.py``'s ``validate()``: a loader that stops
    at the first error turns fixing a catalog into a sequence of guesses.
    """
    declared = document.get("schema_version")
    if declared is None:
        raise InvalidProduct(
            "schema_version is required. A catalog with no declared version is "
            "not something this loader will guess at."
        )
    if declared != SCHEMA_VERSION:
        raise InvalidProduct(
            f"schema_version {declared!r} is not supported by this build (this build "
            f"understands version {SCHEMA_VERSION}). A document written for a different "
            "schema is rejected rather than reinterpreted."
        )

    validator = jsonschema.Draft202012Validator(schema)
    problems = sorted(validator.iter_errors(document), key=lambda e: list(e.absolute_path))
    if problems:
        detail = "\n".join(f"  - {_describe(problem)}" for problem in problems)
        raise InvalidProduct(f"product catalog is invalid ({len(problems)} problem(s)):\n{detail}")


def load_catalog(root: Path, path: str = DEFAULT_CATALOG_PATH) -> ProductCatalog:
    """Load and schema-validate the product catalog at ``root / path``."""
    catalog_path = root / path
    if not catalog_path.exists():
        raise InvalidProduct(f"product catalog not found at {path}")

    document = yaml.safe_load(catalog_path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise InvalidProduct(f"{path} must contain a mapping at the top level")

    schema = load_schema(root)
    _validate(document, schema)

    products = [
        Product(
            venue=entry["venue"],
            product_root=entry["product_root"],
            description=entry["description"],
            tick_size=Decimal(entry["tick_size"]),
            lot_size=entry["lot_size"],
            multiplier=Decimal(entry["multiplier"]),
            currency=entry["currency"],
            timezone=entry["timezone"],
            session_template=entry["session_template"],
        )
        for entry in document["products"]
    ]
    return ProductCatalog(products)
