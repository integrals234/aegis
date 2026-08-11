"""M2 slice 1 — futures contract identity and its canonical form.

Identity is the key every downstream artifact is joined on: the normalized
schema, roll audit records, per-observation provenance, and the
`contract_symbol` component of the canonical replay order. So the properties
worth testing are not "does it format a string" but "can the same contract ever
have two spellings, and can a bad identifier pass silently".
"""

from __future__ import annotations

import dataclasses

import pytest
from futures.identifiers import (
    MONTH_CODES,
    ContractId,
    InvalidContractId,
    month_code,
    month_number,
)

pytestmark = pytest.mark.unit


# --------------------------------------------------------------------- codes


def test_month_codes_are_the_standard_twelve():
    assert MONTH_CODES == "FGHJKMNQUVXZ"
    assert len(MONTH_CODES) == 12
    assert len(set(MONTH_CODES)) == 12, "a duplicated code would alias two months"
    # I and L are absent by convention: they read as 1 and as each other.
    assert "I" not in MONTH_CODES
    assert "L" not in MONTH_CODES


@pytest.mark.parametrize("month", range(1, 13))
def test_month_code_round_trips(month):
    assert month_number(month_code(month)) == month


@pytest.mark.parametrize("month", [0, 13, -1, 100])
def test_month_code_rejects_out_of_range(month):
    with pytest.raises(InvalidContractId, match="month must be 1-12"):
        month_code(month)


@pytest.mark.parametrize("code", ["I", "L", "", "ZZ", "z", "1"])
def test_month_number_rejects_non_codes(code):
    with pytest.raises(InvalidContractId):
        month_number(code)


# ------------------------------------------------------------------ identity


def test_canonical_form_is_stable_and_documented():
    contract = ContractId(venue="XCME", product_root="ES", year=2026, month=12)
    assert contract.canonical == "XCME:ES:2026Z"
    assert str(contract) == "XCME:ES:2026Z"
    assert contract.month_code == "Z"


def test_parse_is_exactly_inverse_to_canonical():
    for contract in (
        ContractId("XCME", "ES", 2026, 12),
        ContractId("XNYM", "CL", 1999, 1),
        ContractId("XCBT", "ZN", 2030, 6),
    ):
        assert ContractId.parse(contract.canonical) == contract
        assert ContractId.parse(contract.canonical).canonical == contract.canonical


def test_one_contract_has_exactly_one_spelling():
    """The property the whole module exists for.

    If two spellings of one contract could coexist, provenance would silently
    split and a roll audit would under-report.
    """
    contract = ContractId("XCME", "ES", 2026, 12)
    for rejected in ("xcme:ES:2026Z", "XCME:ES:26Z", "XCME:ES:2026Z ", " XCME:ES:2026Z",
                     "XCME-ES-2026Z", "XCME:ES:2026-12", "XCME:ES:2026z"):
        with pytest.raises(InvalidContractId):
            ContractId.parse(rejected)
    assert ContractId.parse(contract.canonical) == contract


def test_two_digit_years_are_refused_rather_than_guessed():
    """`Z6` is 2016, 2026 and 2036. Guessing puts a decade-long hole in a
    continuous series and nothing downstream can detect it."""
    with pytest.raises(InvalidContractId):
        ContractId.parse("XCME:ES:26Z")


def test_lexicographic_order_of_canonical_agrees_with_chronology():
    """Downstream code sorts these strings; disagreeing orders would be a trap."""
    same_product = [
        ContractId("XCME", "ES", 2026, 3),
        ContractId("XCME", "ES", 2026, 6),
        ContractId("XCME", "ES", 2026, 9),
        ContractId("XCME", "ES", 2026, 12),
        ContractId("XCME", "ES", 2027, 3),
    ]
    by_string = sorted(same_product, key=lambda c: c.canonical)
    by_date = sorted(same_product, key=lambda c: (c.year, c.month))
    assert by_string == by_date


def test_is_hashable_frozen_and_ordered():
    a = ContractId("XCME", "ES", 2026, 12)
    b = ContractId("XCME", "ES", 2026, 12)
    assert a == b and hash(a) == hash(b)
    assert len({a, b}) == 1
    # FrozenInstanceError specifically: a blind `Exception` here would also
    # pass if the assignment raised for some unrelated reason.
    with pytest.raises(dataclasses.FrozenInstanceError):
        a.year = 2027  # type: ignore[misc]
    assert ContractId("XCME", "ES", 2026, 3) < ContractId("XCME", "ES", 2026, 12)


# ------------------------------------------------------------------ negative


@pytest.mark.parametrize(
    "kwargs",
    [
        {"venue": "", "product_root": "ES", "year": 2026, "month": 12},
        {"venue": "xcme", "product_root": "ES", "year": 2026, "month": 12},
        {"venue": "XCME TOO LONG", "product_root": "ES", "year": 2026, "month": 12},
        {"venue": "XCME", "product_root": "", "year": 2026, "month": 12},
        {"venue": "XCME", "product_root": "es", "year": 2026, "month": 12},
        {"venue": "XCME", "product_root": "E S", "year": 2026, "month": 12},
        {"venue": "XCME", "product_root": "ES", "year": 1969, "month": 12},
        {"venue": "XCME", "product_root": "ES", "year": 3000, "month": 12},
        {"venue": "XCME", "product_root": "ES", "year": 2026, "month": 0},
        {"venue": "XCME", "product_root": "ES", "year": 2026, "month": 13},
    ],
)
def test_constructor_rejects_malformed_identity(kwargs):
    with pytest.raises(InvalidContractId):
        ContractId(**kwargs)


def test_booleans_are_not_accepted_as_year_or_month():
    """`True == 1` in Python, so an unguarded int check would accept it and
    produce a contract in January of year 1."""
    with pytest.raises(InvalidContractId):
        ContractId("XCME", "ES", 2026, True)  # type: ignore[arg-type]
    with pytest.raises(InvalidContractId):
        ContractId("XCME", "ES", True, 12)  # type: ignore[arg-type]


@pytest.mark.parametrize("text", [None, 42, b"XCME:ES:2026Z", ["XCME:ES:2026Z"]])
def test_parse_rejects_non_strings(text):
    with pytest.raises(InvalidContractId):
        ContractId.parse(text)  # type: ignore[arg-type]
