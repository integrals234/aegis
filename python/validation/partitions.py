"""Chronological train/validation/test partitions and the test-set lock
(AEGIS-139).

The frozen acceptance is "the experiment manifest prevents test-set tuning" --
a mechanism, not a naming convention. :func:`guard_test_set_access` is that
mechanism: a run declares its :class:`RunPurpose` once, and any code path
that reaches for the test partition while that purpose is ``TUNING`` raises
:class:`LockedTestPartitionError` rather than silently returning the data. The
partitions themselves are chronological and non-overlapping by construction
(:func:`partition` uses strict date comparisons at both boundaries), so every
observation is assigned to exactly one of the three splits.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from datetime import date
from enum import StrEnum
from pathlib import Path

import yaml

__all__ = [
    "DatasetPartitions",
    "LockedTestPartitionError",
    "PartitionName",
    "RunPurpose",
    "guard_test_set_access",
    "load_partition_boundaries",
    "partition",
]


class RunPurpose(StrEnum):
    """Why this run exists -- the fact :func:`guard_test_set_access` checks."""

    TRAINING = "training"
    TUNING = "tuning"
    FINAL_EVALUATION = "final_evaluation"


class PartitionName(StrEnum):
    TRAIN = "train"
    VALIDATION = "validation"
    TEST = "test"


class LockedTestPartitionError(RuntimeError):
    """A tuning-purpose run tried to read the locked test partition."""


@dataclass(frozen=True, slots=True)
class DatasetPartitions:
    """The three chronological splits, each a sorted, non-overlapping,
    contiguous list of dates whose union is the input series exactly."""

    train: tuple[date, ...]
    validation: tuple[date, ...]
    test: tuple[date, ...]

    def __post_init__(self) -> None:
        all_dates = self.train + self.validation + self.test
        if len(all_dates) != len(set(all_dates)):
            raise ValueError("a date appears in more than one partition")
        if self.train and self.validation and self.train[-1] >= self.validation[0]:
            raise ValueError("train partition overlaps validation")
        if self.validation and self.test and self.validation[-1] >= self.test[0]:
            raise ValueError("validation partition overlaps test")
        if self.train and self.test and not self.validation and self.train[-1] >= self.test[0]:
            raise ValueError("train partition overlaps test")

    def get(self, name: PartitionName, *, purpose: RunPurpose) -> tuple[date, ...]:
        """The named partition's dates, subject to the test-set lock."""
        guard_test_set_access(purpose, name)
        if name is PartitionName.TRAIN:
            return self.train
        if name is PartitionName.VALIDATION:
            return self.validation
        return self.test


def guard_test_set_access(purpose: RunPurpose, partition_name: PartitionName) -> None:
    """Raises :class:`LockedTestPartitionError` unless a
    ``FINAL_EVALUATION``-purpose run is the one reaching for the test
    partition -- ``TRAINING`` and ``TUNING`` are both refused, which is what
    the error message has always said and what "prevents test-set tuning"
    requires: a training run that peeks at the test split has contaminated it
    just as thoroughly as a tuning run.

    Call this at every point a run obtains a partition's data, not only
    inside :meth:`DatasetPartitions.get` -- :func:`partition` itself does not
    call it, so a caller that slices the result directly is not silently
    protected."""
    if partition_name is PartitionName.TEST and purpose is not RunPurpose.FINAL_EVALUATION:
        raise LockedTestPartitionError(
            f"a {purpose.value}-purpose run attempted to read the test partition; "
            "the test set is locked until purpose=final_evaluation (AEGIS-139)"
        )


def partition(
    dates: Sequence[date], train_end: date, validation_end: date | None = None
) -> DatasetPartitions:
    """Splits ``dates`` (assumed sorted ascending, not re-sorted here so a
    caller's own ordering bug is visible rather than silently corrected) at
    ``train_end`` and, if given, ``validation_end``. Every date `` <=
    train_end`` is train; every date in ``(train_end, validation_end]`` is
    validation (empty if ``validation_end`` is ``None``); everything after is
    test. Both boundaries are strict, so no date is ever assigned twice.
    """
    if validation_end is not None and validation_end <= train_end:
        raise ValueError("validation_end must be strictly after train_end")

    train = tuple(d for d in dates if d <= train_end)
    if validation_end is None:
        validation: tuple[date, ...] = ()
        test = tuple(d for d in dates if d > train_end)
    else:
        validation = tuple(d for d in dates if train_end < d <= validation_end)
        test = tuple(d for d in dates if d > validation_end)
    return DatasetPartitions(train=train, validation=validation, test=test)


def load_partition_boundaries(repo_root: Path) -> tuple[date, date | None]:
    """Reads ``configs/validation/partitions.yaml`` and returns
    ``(train_end, validation_end)``.

    This exists because a committed config file that no code reads is worse
    than no config at all: it implies a mechanism that is not there. The M5
    closure quant review found exactly that — ``partitions.yaml`` and
    ``rejection_criteria.yaml`` were both dead files — so both are now read
    by the evidence producer that claims to honour them.
    """
    doc = yaml.safe_load((repo_root / "configs" / "validation" / "partitions.yaml").read_text())
    validation_end = doc.get("validation_end")
    return doc["train_end"], validation_end
