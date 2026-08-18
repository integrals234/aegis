"""Look-ahead-bias detection and feature/data leakage audit (AEGIS-152,
AEGIS-153).

Both consume RECORDED relationships -- a feature's own index/time, and the
index range of the data actually used to compute it -- never the
estimator's arithmetic itself. This is deliberate (the AEGIS-107 lesson,
ADR-0029): a detector built by re-executing or transliterating the
implementation under test would agree with it by construction and catch
nothing. :func:`honest_rolling_zscore_timing_records` derives its records
from the DOCUMENTED windowing convention
(:mod:`research.signal_reference`'s "scored against the prior window
only") -- structural metadata about what the convention promises, not a
recomputation of any z-score value.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

__all__ = [
    "FeatureTimingRecord",
    "LeakageAuditResult",
    "LeakageViolation",
    "audit_feature_timing",
    "audit_partition_boundary_consistency",
    "honest_rolling_zscore_timing_records",
    "seeded_leaky_timing_records",
]


@dataclass(frozen=True, slots=True)
class FeatureTimingRecord:
    """One feature value's timing metadata: the index/time it is USED at
    (``feature_index``), and the inclusive index range of data that went
    into computing it (``fitting_window_start_index``,
    ``fitting_window_end_index``)."""

    feature_index: int
    fitting_window_start_index: int
    fitting_window_end_index: int


@dataclass(frozen=True, slots=True)
class LeakageViolation:
    feature_index: int
    fitting_window_end_index: int
    reason: str


@dataclass(frozen=True, slots=True)
class LeakageAuditResult:
    record_count: int
    violations: tuple[LeakageViolation, ...]

    @property
    def passed(self) -> bool:
        return not self.violations

    def as_records(self) -> list[dict[str, object]]:
        return [
            {"feature_index": v.feature_index, "fitting_window_end_index": v.fitting_window_end_index,
             "reason": v.reason}
            for v in self.violations
        ]


def audit_feature_timing(records: Sequence[FeatureTimingRecord]) -> LeakageAuditResult:
    """AEGIS-152: a record is a violation iff its fitting window reaches AT
    OR PAST the feature's own index -- using the value being predicted (or
    anything from its own or a later time) to compute the feature that
    predicts it. Also flags a structurally inverted window
    (``start > end``) as a distinct, always-worth-surfacing defect."""
    violations = []
    for record in records:
        if record.fitting_window_end_index >= record.feature_index:
            violations.append(
                LeakageViolation(
                    feature_index=record.feature_index,
                    fitting_window_end_index=record.fitting_window_end_index,
                    reason=(
                        f"fitting window ends at index {record.fitting_window_end_index}, at or after "
                        f"the feature's own index {record.feature_index} (look-ahead)"
                    ),
                )
            )
        elif record.fitting_window_start_index > record.fitting_window_end_index + 1:
            violations.append(
                LeakageViolation(
                    feature_index=record.feature_index,
                    fitting_window_end_index=record.fitting_window_end_index,
                    reason="fitting window is structurally inverted (start beyond end + 1)",
                )
            )
    return LeakageAuditResult(record_count=len(records), violations=tuple(violations))


def audit_partition_boundary_consistency(
    records: Sequence[FeatureTimingRecord], train_end_index: int
) -> LeakageAuditResult:
    """AEGIS-153's partition-boundary half: a feature used at an index
    inside the training partition (``feature_index <= train_end_index``)
    whose fitting window reaches past ``train_end_index`` has used
    out-of-partition data to inform an in-partition decision -- a distinct
    failure mode from plain look-ahead, since each individual observation
    could still be chronologically prior to its own feature_index while
    still crossing a partition the experiment declared closed."""
    violations = []
    for record in records:
        if record.feature_index <= train_end_index and record.fitting_window_end_index > train_end_index:
            violations.append(
                LeakageViolation(
                    feature_index=record.feature_index,
                    fitting_window_end_index=record.fitting_window_end_index,
                    reason=(
                        f"training-partition feature at index {record.feature_index} was fit using data "
                        f"up to index {record.fitting_window_end_index}, past train_end_index "
                        f"{train_end_index}"
                    ),
                )
            )
    return LeakageAuditResult(record_count=len(records), violations=tuple(violations))


def honest_rolling_zscore_timing_records(num_observations: int, window: int) -> tuple[FeatureTimingRecord, ...]:
    """The documented convention `research.signal_reference.
    rolling_zscore_reference` and `cpp::RollingZScore` both commit to:
    index ``i``'s score is computed from the PRIOR window
    ``[max(0, i - window), i - 1]``, never including ``i`` itself. This
    function encodes that documented promise as timing metadata; it does
    not call the z-score function or reproduce its arithmetic."""
    records = []
    for i in range(num_observations):
        start = max(0, i - window)
        end = i - 1  # Strictly prior; a fresh series (i == 0) yields end < start, which passes trivially.
        records.append(
            FeatureTimingRecord(feature_index=i, fitting_window_start_index=start, fitting_window_end_index=end)
        )
    return tuple(records)


def seeded_leaky_timing_records(num_observations: int, window: int) -> tuple[FeatureTimingRecord, ...]:
    """A deliberately leaky variant, for exercising the detector itself
    (never used to score anything real): the fitting window incorrectly
    includes the CURRENT index -- the exact shape of bug a rolling
    computation that forgot to exclude "now" would produce."""
    return tuple(
        FeatureTimingRecord(feature_index=i, fitting_window_start_index=max(0, i - window), fitting_window_end_index=i)
        for i in range(num_observations)
    )
