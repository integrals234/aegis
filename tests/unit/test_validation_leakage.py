"""AEGIS-152, AEGIS-153 -- the look-ahead detector must be falsifiable: it
catches a seeded leaky fixture and passes the honest path."""

from __future__ import annotations

import pytest
from validation.leakage import (
    audit_feature_timing,
    audit_partition_boundary_consistency,
    honest_rolling_zscore_timing_records,
    seeded_leaky_timing_records,
)

pytestmark = pytest.mark.unit


def test_the_honest_rolling_convention_passes_with_zero_violations() -> None:
    records = honest_rolling_zscore_timing_records(num_observations=50, window=20)
    result = audit_feature_timing(records)
    assert result.passed
    assert result.violations == ()
    assert result.record_count == 50


def test_a_seeded_leaky_implementation_is_caught() -> None:
    # If the detector passed this fixture, the test itself would be wrong to
    # trust it -- this is the negative-gate proof AEGIS-152 requires.
    records = seeded_leaky_timing_records(num_observations=50, window=20)
    result = audit_feature_timing(records)
    assert not result.passed
    # Every index >= 1 leaks its own current value into its own fitting
    # window (index 0 has an empty/degenerate window and is not itself a
    # look-ahead case).
    assert len(result.violations) >= 40
    assert all("look-ahead" in v.reason for v in result.violations)


def test_violation_records_are_deterministic_and_reference_the_offending_index() -> None:
    records = seeded_leaky_timing_records(num_observations=10, window=5)
    result = audit_feature_timing(records)
    first_violation = result.violations[0]
    assert first_violation.feature_index == first_violation.fitting_window_end_index


def test_partition_boundary_consistency_flags_training_features_fit_past_the_boundary() -> None:
    # A (hypothetically buggy) record: feature at index 5 (inside train,
    # train_end_index=10) fit using data up to index 15 (past the boundary).
    from validation.leakage import FeatureTimingRecord

    records = (
        FeatureTimingRecord(feature_index=5, fitting_window_start_index=0, fitting_window_end_index=4),
        FeatureTimingRecord(feature_index=8, fitting_window_start_index=0, fitting_window_end_index=15),
    )
    result = audit_partition_boundary_consistency(records, train_end_index=10)
    assert len(result.violations) == 1
    assert result.violations[0].feature_index == 8


def test_partition_boundary_consistency_passes_when_nothing_crosses_the_boundary() -> None:
    records = honest_rolling_zscore_timing_records(num_observations=30, window=10)
    result = audit_partition_boundary_consistency(records, train_end_index=20)
    assert result.passed
