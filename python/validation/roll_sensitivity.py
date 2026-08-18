"""Roll-method sensitivity, the M5 validation-level consumption of M4's own
module (AEGIS-154).

Reuses `research.roll_method_sensitivity.compute_roll_method_strategy_
sensitivity` unmodified -- M4 already built exactly this comparison (the
residual on AEGIS-024) and M5 must not rebuild roll logic. This module adds
nothing but an explicit "report the differences, honestly" framing: if two
roll policies produce the identical metric, that is reported as a real
zero, never nudged apart to manufacture a difference (the M4 closure
lesson -- ADR-0025, `experiments/milestone-reports/M4.md` section 10).
"""

from __future__ import annotations

from dataclasses import dataclass
from decimal import Decimal

from research.roll_method_sensitivity import PolicyStrategyMetrics, RollMethodStrategySensitivityResult

__all__ = ["PolicyPnlDifference", "summarize_roll_method_differences"]


@dataclass(frozen=True, slots=True)
class PolicyPnlDifference:
    policy_a: str
    policy_b: str
    total_pnl_difference: Decimal  # policy_a - policy_b; exactly Decimal(0) when truly equal.


def summarize_roll_method_differences(
    result: RollMethodStrategySensitivityResult,
) -> tuple[PolicyPnlDifference, ...]:
    """Every distinct pair of policies in ``result``, reporting the actual
    signed P&L difference -- ``Decimal(0)`` where the policies genuinely
    produced the same total, not a value perturbed to look meaningful."""
    metrics: list[PolicyStrategyMetrics] = list(result.strategy_metrics_by_policy)
    differences = []
    for i, policy_a in enumerate(metrics):
        for policy_b in metrics[i + 1 :]:
            differences.append(
                PolicyPnlDifference(
                    policy_a=policy_a.policy_name,
                    policy_b=policy_b.policy_name,
                    total_pnl_difference=policy_a.total_pnl - policy_b.total_pnl,
                )
            )
    return tuple(differences)
