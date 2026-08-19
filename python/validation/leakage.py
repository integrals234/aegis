"""Look-ahead-bias detection and feature/data leakage audit (AEGIS-152,
AEGIS-153).

The detector (:func:`audit_feature_timing`) consumes RECORDED
relationships -- a feature's own index/time, and the index range of the
data actually used to compute it -- never the estimator's arithmetic
itself. `FeatureTimingRecord` carries no z-score, mean, variance or window-
length field, so the detector cannot read or reproduce the estimator's math
even in principle. This is deliberate (the AEGIS-107 lesson, ADR-0029): a
detector built by re-executing or transliterating the implementation under
test would agree with it by construction and catch nothing.

# Where the records come from (M5 closure correction, twice)

An earlier version of this module fabricated timing records from the
*documented* windowing convention rather than from what an estimator
actually did. That was fixed once by giving `rolling_zscore_reference` a
`timing_sink`, but the first fix still computed
`fitting_window_end_index = index - 1` as a constant expression rather than
reading it from execution state -- true for a correctly-behaving call, but
never actually checked, so a future leak in the real estimator still would
not have been caught. `research.signal_reference` now tracks
`(index, value)` pairs in its sliding window and reads BOTH boundaries
directly off that structure (see that module's docstring), so
:func:`collect_timing_records_from_real_estimator` here collects a
`WindowProvenance` that is a genuine readout of what execution held, not an
arithmetic restatement of what it should have held. The detector still
never reads the z-score values themselves -- only the window's index
boundaries, which is what makes it independent.

:func:`run_seeded_leaky_estimator_for_falsifiability_check` is the negative
counterpart: it drives
`research.signal_reference.rolling_zscore_reference_with_seeded_leak_for_falsifiability_check`,
which runs the SAME execution engine as the honest path with one
sequencing difference (the current observation joins the window before,
not after, being scored) -- so the leak's effect on the emitted provenance
is a natural consequence of shared arithmetic, never a hand-authored
violation. It is never used to score a real signal;
`research.signal_reference.rolling_zscore_reference` remains the only
estimator any AEGIS strategy or validation module is scored against.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass

from research.signal_reference import (
    WindowProvenance,
    rolling_zscore_reference,
    rolling_zscore_reference_with_seeded_leak_for_falsifiability_check,
)

__all__ = [
    "FeatureTimingRecord",
    "LeakageAuditResult",
    "LeakageViolation",
    "audit_feature_timing",
    "audit_partition_boundary_consistency",
    "collect_timing_records_from_real_estimator",
    "run_seeded_leaky_estimator_for_falsifiability_check",
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


def collect_timing_records_from_real_estimator(
    values: Sequence[float], window: int
) -> tuple[FeatureTimingRecord, ...]:
    """Drives the REAL `research.signal_reference.rolling_zscore_reference`
    over `values` and collects the `WindowProvenance` it emits from its own
    live execution -- never a formula re-deriving what the estimator
    *should* have read (see that module's docstring for how the provenance
    itself is now read from the same `(index, value)` structure the score
    is computed from). If that estimator were ever modified to leak, this
    collector would automatically surface it, because the records it
    returns are the estimator's own account of what it consumed, not a
    restatement of its documented contract.

    The z-score values themselves are not needed here and are discarded;
    only the timing provenance the sink receives is kept.
    """
    records: list[FeatureTimingRecord] = []

    def sink(provenance: WindowProvenance) -> None:
        records.append(
            FeatureTimingRecord(
                feature_index=provenance.feature_index,
                fitting_window_start_index=provenance.fitting_window_start_index,
                fitting_window_end_index=provenance.fitting_window_end_index,
            )
        )

    for _ in rolling_zscore_reference(values, window, timing_sink=sink):
        pass
    return tuple(records)


def run_seeded_leaky_estimator_for_falsifiability_check(
    values: Sequence[float], window: int
) -> tuple[FeatureTimingRecord, ...]:
    """NEGATIVE-TEST-ONLY. Exists solely to prove `audit_feature_timing` can
    catch a genuinely leaking EXECUTION, not merely a hand-authored leaky
    record.

    Drives
    `research.signal_reference.rolling_zscore_reference_with_seeded_leak_for_falsifiability_check`
    -- the SAME execution engine `rolling_zscore_reference` uses, with one
    sequencing difference (the current observation joins the window before
    it is scored, instead of after). The provenance below is collected from
    that real (leaking) execution's own sink callback, identically to the
    honest collector above; nothing here is a formula describing what a
    leak would look like, and this module contains no separate loop that
    could drift from the production estimator's behaviour.

    Never call this to score a real signal. `research.signal_reference`'s
    `rolling_zscore_reference` is the only estimator any AEGIS strategy or
    validation module is judged against.
    """
    records: list[FeatureTimingRecord] = []

    def sink(provenance: WindowProvenance) -> None:
        records.append(
            FeatureTimingRecord(
                feature_index=provenance.feature_index,
                fitting_window_start_index=provenance.fitting_window_start_index,
                fitting_window_end_index=provenance.fitting_window_end_index,
            )
        )

    for _ in rolling_zscore_reference_with_seeded_leak_for_falsifiability_check(values, window, timing_sink=sink):
        pass
    return tuple(records)
