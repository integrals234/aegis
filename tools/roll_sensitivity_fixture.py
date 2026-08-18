"""The shared synthetic multi-contract scenario for AEGIS-024 and AEGIS-081
(Batch 2). One builder, reused by both the pytest suite and the evidence
generators, so the fixture cannot silently diverge between the two.

Three contracts (NEAR/MID/FAR, all listed throughout the window) rather than
the two `tests/unit/test_roll_sensitivity.py` uses for the pure M2 policy
comparison: AEGIS-024/081 need a calendar-spread *far leg* available whichever
of NEAR/MID a policy selects as front, which two contracts cannot supply on
their own. Product root `CSX` (not `EQX`) deliberately keeps this fixture
visually distinct from the real EQX chain `data_samples/futures/` and
Batch 1's demo use -- this dataset is synthetic throughout, disclosed as
such, and used only for the two policies' *relative* comparison, never as a
claim about real markets (ADR-0025).
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from datetime import date, timedelta
from decimal import Decimal
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))

from futures.chain import ContractChain
from futures.contracts import Contract, SettlementType
from futures.identifiers import ContractId
from futures.roll.fixed_days import FixedDaysPolicy
from futures.roll.policy import RollObservation, RollPolicy
from futures.roll.volume_crossover import VolumeCrossoverPolicy
from futures.series import PriceObservation
from research.calendar_spread import ConstructedBasisRule
from research.strategy_replay import ReplayConfig

VENUE = "SYNX"
PRODUCT_ROOT = "CSX"
NEAR = ContractId(venue=VENUE, product_root=PRODUCT_ROOT, year=2026, month=3)
MID = ContractId(venue=VENUE, product_root=PRODUCT_ROOT, year=2026, month=6)
FAR = ContractId(venue=VENUE, product_root=PRODUCT_ROOT, year=2026, month=9)

BASE_DAY = date(2026, 1, 1)
NUM_DATES = 10

# The constructed-basis FALLBACK for this fixture. Since all three contracts
# above are priced, `build_calendar_spread_observations` uses observed far-leg
# prices throughout and this rule is never actually applied here -- it is
# supplied because the function requires one, and kept identical in shape to
# Batch 1's own demo basis sequence for recognisability. All synthetic
# (ADR-0025).
FAR_BASIS_BY_INDEX: tuple[Decimal, ...] = (
    Decimal("0.50"),
    Decimal("0.55"),
    Decimal("0.60"),
    Decimal("0.65"),
    Decimal("2.50"),
    Decimal("0.70"),
    Decimal("0.75"),
    Decimal("3.00"),
    Decimal("0.80"),
    Decimal("0.85"),
)
FAR_BASIS_DESCRIPTION = (
    "constructed: far_price = near_price + basis[index], basis in dollars = "
    "[0.50, 0.55, 0.60, 0.65, 2.50, 0.70, 0.75, 3.00, 0.80, 0.85]; NOT observed "
    "market data (ADR-0025)"
)


@dataclass(frozen=True, slots=True)
class RollSensitivityFixture:
    chain: ContractChain
    roll_observations: tuple[RollObservation, ...]
    near_prices: tuple[PriceObservation, ...]
    dates: tuple[date, ...]
    policies: dict[str, RollPolicy]
    basis_rule: ConstructedBasisRule
    replay_config: ReplayConfig


def build_roll_sensitivity_fixture() -> RollSensitivityFixture:
    chain = ContractChain(VENUE, PRODUCT_ROOT)
    for contract_id, expiry in (
        (NEAR, date(2026, 3, 20)),
        (MID, date(2026, 6, 20)),
        (FAR, date(2026, 9, 20)),
    ):
        chain.add(
            Contract(
                contract_id=contract_id,
                first_trade_date=date(2025, 1, 1),
                last_trade_date=expiry,
                expiry=expiry,
                settlement_type=SettlementType.CASH,
            )
        )

    dates = tuple(BASE_DAY + timedelta(days=i) for i in range(NUM_DATES))

    roll_observations: list[RollObservation] = []
    near_prices: list[PriceObservation] = []
    for i, day in enumerate(dates):
        # NEAR's volume declines, MID's rises -- VolumeCrossoverPolicy(2)
        # confirms the crossover and rolls on day index 5, matching the
        # identical numbers tests/unit/test_roll_sensitivity.py already
        # proved roll on day 6 (0-based index 5) for the same policy.
        roll_observations.append(RollObservation(NEAR, day, 1000 - i * 80, None))
        roll_observations.append(RollObservation(MID, day, 200 + i * 90, None))
        # All THREE contracts are priced, deliberately: whichever contract a
        # policy selects as front, the next one along has an observed price,
        # so `build_calendar_spread_observations` uses observed prices for
        # both legs under every policy compared. That is what keeps the roll
        # policy the only variable that differs -- if one policy's far leg
        # were observed and another's constructed, the comparison would be
        # confounded by provenance rather than controlled. The three shapes
        # are deliberately different (linear / quadratic / faster-linear) so
        # the resulting spread series genuinely differ by contract pair.
        near_prices.append(PriceObservation(NEAR, day, Decimal(100 + i)))
        near_prices.append(PriceObservation(MID, day, Decimal(150 + i * i)))
        near_prices.append(PriceObservation(FAR, day, Decimal(200 + 3 * i)))

    policies: dict[str, RollPolicy] = {
        "volume_crossover": VolumeCrossoverPolicy(persistence_days=2),
        # 100 days clears NEAR's ~78-day distance to its own expiry but not
        # MID's ~170-day distance to its own -- MID is the first contract
        # that qualifies, on every date, so this policy rolls to MID
        # immediately rather than following NEAR's own volume signal.
        "fixed_100_days": FixedDaysPolicy(days_before_expiry=100),
    }

    basis_rule = ConstructedBasisRule(
        basis_units_by_index=FAR_BASIS_BY_INDEX, description=FAR_BASIS_DESCRIPTION
    )
    replay_config = ReplayConfig(
        zscore_window=20, entry_threshold=2.0, exit_threshold=0.5, quantity_units=Decimal(1)
    )

    return RollSensitivityFixture(
        chain=chain,
        roll_observations=tuple(roll_observations),
        near_prices=tuple(near_prices),
        dates=dates,
        policies=policies,
        basis_rule=basis_rule,
        replay_config=replay_config,
    )
