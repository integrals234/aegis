"""M5 portfolio-risk report (AEGIS-138): independently RECOMPUTES gross and
net exposure from the same position/price accounting values the C++
`portfolio::compute_portfolio_risk` analytics used, and reconciles the
result against what that analytics run reported -- rather than treating a
serialized copy of the C++ object as its own reconciliation, which would
prove nothing.

Margin, volatility and drawdown contribution, and the scripted stress
results are passed through as reported: recomputing the margin/volatility
model in Python would be a *second implementation* of the same simplified
formula (ADR-0028), which risks the two silently diverging without adding
any real independence -- stating that plainly is more honest than
fabricating a second recompute that is not actually independent.
"""

from __future__ import annotations

from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from reports.report_model import build_report_provenance, render_report

__all__ = ["PositionAccountingRecord", "ReconciliationResult", "build_portfolio_risk_report", "reconcile_exposure"]


@dataclass(frozen=True, slots=True)
class PositionAccountingRecord:
    """One instrument's raw accounting values -- the source of truth this
    report recomputes exposure from, independently of the C++ analytics'
    own arithmetic."""

    instrument_id: int
    quantity_units: int
    mark_price_units: int
    multiplier_units: int
    market: str = ""
    sector: str = ""


@dataclass(frozen=True, slots=True)
class ReconciliationResult:
    recomputed_gross_exposure_units: int
    recomputed_net_exposure_units: int
    reported_gross_exposure_units: int
    reported_net_exposure_units: int
    gross_matches: bool
    net_matches: bool

    @property
    def reconciles(self) -> bool:
        return self.gross_matches and self.net_matches


def reconcile_exposure(
    positions: Sequence[PositionAccountingRecord],
    reported_gross_exposure_units: int,
    reported_net_exposure_units: int,
) -> ReconciliationResult:
    """Recomputes gross/net exposure directly from ``positions`` --
    ``sum(|quantity * mark_price * multiplier|)`` for gross, the signed sum
    for net -- the identical formula `portfolio::compute_portfolio_risk`
    documents, applied here to the same accounting inputs rather than to
    its output."""
    gross = 0
    net = 0
    for position in positions:
        signed = position.quantity_units * position.mark_price_units * position.multiplier_units
        gross += abs(signed)
        net += signed
    return ReconciliationResult(
        recomputed_gross_exposure_units=gross,
        recomputed_net_exposure_units=net,
        reported_gross_exposure_units=reported_gross_exposure_units,
        reported_net_exposure_units=reported_net_exposure_units,
        gross_matches=gross == reported_gross_exposure_units,
        net_matches=net == reported_net_exposure_units,
    )


def build_portfolio_risk_report(
    root: Path,
    input_paths: Sequence[str],
    dataset_id: str,
    *,
    strategy_config: Mapping[str, Any],
    positions: Sequence[PositionAccountingRecord],
    reported_risk_analytics: Mapping[str, Any],
) -> str:
    """``reported_risk_analytics`` is the C++ `portfolio::PortfolioRiskReport`
    decoded from its evidence JSON -- must carry at least
    ``gross_exposure_units``/``net_exposure_units`` for the reconciliation,
    and may carry ``margin_used_units``/``margin_available_units``/
    ``market_exposure_units``/``sector_exposure_units``/
    ``volatility_contribution``/``current_drawdown``/``max_drawdown``/
    ``stress_results``, all passed through unmodified (see module
    docstring)."""
    provenance = build_report_provenance(
        report_id="AEGIS-138-portfolio-risk",
        root=root,
        input_paths=input_paths,
        strategy_config=dict(strategy_config),
        dataset_id=dataset_id,
        roll_policy_name="n/a",
    )
    reconciliation = reconcile_exposure(
        positions,
        int(reported_risk_analytics["gross_exposure_units"]),
        int(reported_risk_analytics["net_exposure_units"]),
    )
    findings: dict[str, Any] = {
        "reconciliation": {
            "recomputed_gross_exposure_units": reconciliation.recomputed_gross_exposure_units,
            "recomputed_net_exposure_units": reconciliation.recomputed_net_exposure_units,
            "reported_gross_exposure_units": reconciliation.reported_gross_exposure_units,
            "reported_net_exposure_units": reconciliation.reported_net_exposure_units,
            "reconciles": reconciliation.reconciles,
        },
        "margin_used_units": reported_risk_analytics.get("margin_used_units"),
        "margin_available_units": reported_risk_analytics.get("margin_available_units"),
        "market_exposure_units": reported_risk_analytics.get("market_exposure_units"),
        "sector_exposure_units": reported_risk_analytics.get("sector_exposure_units"),
        "volatility_contribution": reported_risk_analytics.get("volatility_contribution"),
        "current_drawdown": reported_risk_analytics.get("current_drawdown"),
        "max_drawdown": reported_risk_analytics.get("max_drawdown"),
        "stress_results": reported_risk_analytics.get("stress_results"),
        "margin_model_disclosure": (
            "margin_used_units/margin_available_units use the M5 Model A margin "
            "(margin_per_contract_units * abs(quantity)), NOT SPAN and not an exchange "
            "clearing model (ADR-0028); passed through from the C++ analytics, not "
            "recomputed here."
        ),
    }
    return render_report(provenance, findings)
