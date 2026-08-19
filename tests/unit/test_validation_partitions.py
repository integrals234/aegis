"""AEGIS-139 -- chronological train/validation/test partitions and the
test-set access lock."""

from __future__ import annotations

from datetime import date, timedelta

import pytest
from validation.partitions import (
    DatasetPartitions,
    LockedTestPartitionError,
    PartitionName,
    RunPurpose,
    guard_test_set_access,
    partition,
)

pytestmark = pytest.mark.unit


def _dates(n: int) -> list[date]:
    start = date(2026, 1, 1)
    return [start + timedelta(days=i) for i in range(n)]


def test_partition_is_chronological_and_every_date_assigned_exactly_once() -> None:
    dates = _dates(10)
    result = partition(dates, train_end=dates[5], validation_end=dates[7])
    assert result.train == tuple(dates[0:6])
    assert result.validation == tuple(dates[6:8])
    assert result.test == tuple(dates[8:10])
    assert len(result.train) + len(result.validation) + len(result.test) == len(dates)


def test_partition_without_validation_split_is_train_then_test() -> None:
    dates = _dates(6)
    result = partition(dates, train_end=dates[3])
    assert result.validation == ()
    assert result.train == tuple(dates[0:4])
    assert result.test == tuple(dates[4:6])


def test_partition_rejects_a_validation_end_at_or_before_train_end() -> None:
    dates = _dates(5)
    with pytest.raises(ValueError, match="validation_end must be strictly after"):
        partition(dates, train_end=dates[3], validation_end=dates[3])


def test_dataset_partitions_rejects_overlapping_construction() -> None:
    dates = _dates(5)
    with pytest.raises(ValueError, match="more than one partition"):
        DatasetPartitions(train=tuple(dates[0:3]), validation=tuple(dates[2:4]), test=tuple(dates[4:5]))


def test_only_final_evaluation_may_read_the_test_partition() -> None:
    # Both non-final purposes are refused. A TRAINING run that peeks at the
    # test split has contaminated it exactly as thoroughly as a TUNING run --
    # an earlier version of this test cemented that hole by asserting
    # TRAINING was allowed through, which the independent M5 quant review
    # correctly flagged.
    with pytest.raises(LockedTestPartitionError):
        guard_test_set_access(RunPurpose.TUNING, PartitionName.TEST)
    with pytest.raises(LockedTestPartitionError):
        guard_test_set_access(RunPurpose.TRAINING, PartitionName.TEST)
    guard_test_set_access(RunPurpose.FINAL_EVALUATION, PartitionName.TEST)  # The only allowed read.
    # Non-test partitions are never gated, for any purpose.
    guard_test_set_access(RunPurpose.TUNING, PartitionName.TRAIN)
    guard_test_set_access(RunPurpose.TRAINING, PartitionName.VALIDATION)


def test_dataset_partitions_get_enforces_the_lock() -> None:
    dates = _dates(9)
    result = partition(dates, train_end=dates[2], validation_end=dates[5])

    assert result.get(PartitionName.TRAIN, purpose=RunPurpose.TUNING) == result.train

    with pytest.raises(LockedTestPartitionError, match="AEGIS-139"):
        result.get(PartitionName.TEST, purpose=RunPurpose.TUNING)

    # A seeded leakage implementation -- a caller ignoring the lock by
    # reading .test directly, bypassing .get() -- is exactly what the lock
    # cannot catch on its own; guard_test_set_access must be called at every
    # access point, which is why it is documented as a separate, directly
    # callable function rather than folded silently into __post_init__.
    assert result.test  # The unguarded attribute is still directly reachable.
