"""Futures contract identity and its canonical string form (M2 slice 1).

Layer `python-futures` may depend on `python-common` and `python-data` only
(`configs/architecture_rules.yaml`). This module deliberately depends on
neither yet: identity is the one thing every later module needs, so it is
written first and written flat.

Scope is identity **only**. Tick size, lot size, multiplier, currency, expiry
dates, sessions, settlement and delivery belong to the instrument and contract
models in slice 2, and to calendars in slice 3. Nothing here asserts any fact
about any real product — a `ContractId` is a name, not a specification, and
this module knows nothing about what any particular contract is worth.

# Why identity gets its own module, and a canonical form

Every downstream artifact is keyed by contract: the normalized schema
(AEGIS-026), roll audit records (AEGIS-023), per-observation provenance
(AEGIS-019) and the `contract_symbol` component of the canonical replay order.
If two spellings of the same contract can coexist — `ESZ26` and `ES Z6`, say —
then provenance silently splits and a roll audit under-reports. So there is
exactly one canonical form, parsing is total and round-tripping, and
comparison is byte-wise over that form.

The month codes are the standard futures convention (F G H J K M N Q U V X Z
for January through December). They are a naming convention, not market data.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Final

__all__ = [
    "MONTH_CODES",
    "ContractId",
    "InvalidContractId",
    "month_code",
    "month_number",
]

# The industry-standard futures month codes, January (1) through December (12).
# I and L are absent by convention -- they read as 1 and as each other in the
# fonts trading screens historically used.
MONTH_CODES: Final[str] = "FGHJKMNQUVXZ"

# A venue or product root is upper-case alphanumerics. Deliberately strict:
# accepting free text here is how `ES `, `es` and `E S` become three contracts.
_TOKEN: Final[re.Pattern[str]] = re.compile(r"^[A-Z0-9]{1,12}$")

# Canonical form: VENUE:ROOT:YYYYM  e.g. XCME:ES:2026Z
_CANONICAL: Final[re.Pattern[str]] = re.compile(
    r"^(?P<venue>[A-Z0-9]{1,12}):(?P<root>[A-Z0-9]{1,12}):(?P<year>\d{4})(?P<month>[A-Z])$"
)

_MIN_YEAR: Final[int] = 1970
_MAX_YEAR: Final[int] = 2999


class InvalidContractId(ValueError):
    """A contract identifier could not be formed or parsed.

    Raised rather than returning a sentinel: an unparseable identifier that
    flows on as `None` becomes a silently dropped observation, and a dropped
    observation is invisible in every downstream report.
    """


def month_code(month: int) -> str:
    """Return the futures month code for a calendar month (1-12)."""
    if not 1 <= month <= 12:
        raise InvalidContractId(f"month must be 1-12, got {month!r}")
    return MONTH_CODES[month - 1]


def month_number(code: str) -> int:
    """Return the calendar month (1-12) for a futures month code."""
    index = MONTH_CODES.find(code)
    if len(code) != 1 or index < 0:
        raise InvalidContractId(
            f"{code!r} is not a futures month code; expected one of {MONTH_CODES}"
        )
    return index + 1


@dataclass(frozen=True, order=True, slots=True)
class ContractId:
    """A single futures contract's identity: `(venue, product_root, year, month)`.

    Frozen and hashable so it can key a mapping without a defensive copy, and
    ordered so a sort of contract-keyed rows is reproducible.

    The four-digit year is stored rather than the one- or two-digit form that
    appears on trading screens. `Z6` is ambiguous by construction -- 2016, 2026
    and 2036 all spell it -- and a research platform that resolves that
    ambiguity by guessing produces a continuous series with a silent decade-long
    hole in it. Callers that receive a short year must resolve it explicitly.
    """

    venue: str
    product_root: str
    year: int
    month: int

    def __post_init__(self) -> None:
        for name, value in (("venue", self.venue), ("product_root", self.product_root)):
            if not isinstance(value, str) or not _TOKEN.match(value):
                raise InvalidContractId(
                    f"{name} must be 1-12 upper-case alphanumerics, got {value!r}"
                )
        if not isinstance(self.year, int) or isinstance(self.year, bool):
            raise InvalidContractId(f"year must be an int, got {self.year!r}")
        if not _MIN_YEAR <= self.year <= _MAX_YEAR:
            raise InvalidContractId(
                f"year must be {_MIN_YEAR}-{_MAX_YEAR}, got {self.year!r}"
            )
        if not isinstance(self.month, int) or isinstance(self.month, bool):
            raise InvalidContractId(f"month must be an int, got {self.month!r}")
        if not 1 <= self.month <= 12:
            raise InvalidContractId(f"month must be 1-12, got {self.month!r}")

    @property
    def month_code(self) -> str:
        """The futures month code for this contract's delivery month."""
        return month_code(self.month)

    @property
    def canonical(self) -> str:
        """The one canonical spelling: `VENUE:ROOT:YYYYM`.

        Fixed width in the year and one character in the month, so a
        lexicographic sort of canonical strings within a single venue and root
        agrees with chronological order. Downstream code sorts these; making the
        two orders disagree would be a trap.
        """
        return f"{self.venue}:{self.product_root}:{self.year:04d}{self.month_code}"

    def __str__(self) -> str:
        return self.canonical

    @classmethod
    def parse(cls, text: str) -> ContractId:
        """Parse the canonical form. Total and exactly inverse to `canonical`.

        Deliberately accepts only the canonical spelling: no whitespace
        trimming, no case folding, no two-digit years. Vendor-specific symbol
        formats are a normalization concern (slice 4), and letting them in here
        would mean two spellings of one contract could both parse.
        """
        if not isinstance(text, str):
            raise InvalidContractId(f"expected a string, got {type(text).__name__}")
        match = _CANONICAL.match(text)
        if match is None:
            raise InvalidContractId(
                f"{text!r} is not a canonical contract id; expected VENUE:ROOT:YYYYM"
            )
        return cls(
            venue=match["venue"],
            product_root=match["root"],
            year=int(match["year"]),
            month=month_number(match["month"]),
        )
