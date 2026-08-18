"""Roll-method strategy sensitivity: the M4 residual on AEGIS-024.

M2's `python/futures/roll_sensitivity.compare_roll_methods` already
quantifies how roll *policies* differ -- the roll dates they choose, and the
resulting additive-adjusted price-path deviation. That module's own
docstring names what it could not do before M4 existed: attribute those
differences to strategy P&L, because no strategy existed. This module pays
that residual.

For each named roll policy, this module builds the same
:class:`~research.calendar_spread.CalendarSpreadObservation` sequence
(AEGIS-076) under that policy and replays the real strategy decision
semantics (`research.strategy_replay`, the same state machine
`cpp/participant/strategy` implements) over it -- never a separately
invented "strategy" built to manufacture different numbers. Results are
reported alongside `compare_roll_methods`' own price-path comparison, not
duplicating or re-deriving it.

**Sensitivity, not optimization.** This module reports how the metrics
differ across policies. It does not, and must not be read to, claim any
policy is universally better -- the comparison is over one short, disclosed-
synthetic dataset (ADR-0025), not a claim about real markets.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from datetime import date
from decimal import Decimal

from futures.chain import ContractChain
from futures.roll.policy import RollObservation, RollPolicy
from futures.roll_sensitivity import PolicyComparison, compare_roll_methods
from futures.series import PriceObservation

from research.calendar_spread import ConstructedBasisRule, build_calendar_spread_observations
from research.strategy_replay import PositionState, ReplayConfig, replay_strategy

__all__ = [
    "PolicyStrategyMetrics",
    "RollMethodStrategySensitivityResult",
    "compute_roll_method_strategy_sensitivity",
]


@dataclass(frozen=True, slots=True)
class PolicyStrategyMetrics:
    """One roll policy's strategy-level outcome, under a common, controlled
    experiment configuration shared across every policy compared."""

    policy_name: str
    signal_count: int
    entry_count: int
    exit_count: int
    round_trip_count: int
    total_realized_pnl: Decimal
    open_position_unrealized_pnl: Decimal
    total_pnl: Decimal
    final_position: PositionState
    contract_pairs: tuple[str, ...]  # Which (near, far) pairs this policy actually traded.


@dataclass(frozen=True, slots=True)
class RollMethodStrategySensitivityResult:
    price_path_comparisons: tuple[PolicyComparison, ...]  # From M2's compare_roll_methods, unmodified.
    strategy_metrics_by_policy: tuple[PolicyStrategyMetrics, ...]


def compute_roll_method_strategy_sensitivity(
    chain: ContractChain,
    policies: Mapping[str, RollPolicy],
    roll_observations: Sequence[RollObservation],
    near_prices: Sequence[PriceObservation],
    dates: Sequence[date],
    basis_rule: ConstructedBasisRule,
    replay_config: ReplayConfig,
) -> RollMethodStrategySensitivityResult:
    """AEGIS-024. Every policy is run against the identical ``chain``,
    ``near_prices``, ``dates``, ``basis_rule`` and ``replay_config`` -- the
    roll policy is the only controlled variable that differs between them.
    """
    price_path_comparisons = compare_roll_methods(chain, policies, roll_observations, near_prices, dates)

    metrics: list[PolicyStrategyMetrics] = []
    for name in sorted(policies):
        policy = policies[name]
        observations = build_calendar_spread_observations(
            chain=chain,
            policy=policy,
            roll_observations=roll_observations,
            near_prices=near_prices,
            as_of_dates=dates,
            basis_rule=basis_rule,
        )
        replay = replay_strategy(observations, replay_config)
        pairs = sorted(
            {f"{o.near_contract_id.canonical}/{o.far_contract_id.canonical}" for o in observations}
        )
        metrics.append(
            PolicyStrategyMetrics(
                policy_name=name,
                signal_count=replay.signal_count,
                entry_count=replay.entry_count,
                exit_count=replay.exit_count,
                round_trip_count=len(replay.round_trips),
                total_realized_pnl=replay.total_realized_pnl,
                open_position_unrealized_pnl=replay.open_position_unrealized_pnl,
                total_pnl=replay.total_pnl,
                final_position=replay.final_position,
                contract_pairs=tuple(pairs),
            )
        )

    return RollMethodStrategySensitivityResult(
        price_path_comparisons=price_path_comparisons, strategy_metrics_by_policy=tuple(metrics)
    )
