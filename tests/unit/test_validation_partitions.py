"""AEGIS-139 -- chronological train/validation/test partitions and the
test-set access lock."""

from __future__ import annotations

from datetime import date, timedelta
from pathlib import Path

import pytest
import yaml
from validation.partitions import (
    DatasetPartitions,
    LockedTestPartitionError,
    PartitionName,
    RunPurpose,
    guard_test_set_access,
    load_partition_boundaries,
    partition,
    select_non_test_for_tuning,
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


def test_load_partition_boundaries_is_load_bearing_not_decorative(tmp_path: Path) -> None:
    # AEGIS-139 M5 repair: configs/validation/partitions.yaml must be the sole
    # runtime source of the split, not a committed file no code reads. A
    # deliberately unusual config (a fraction load_partition_boundaries()
    # cannot reach without genuinely reading the YAML) proves it here: 20
    # dates, train_end at index 2 (15%), validation_end at index 4 (25%) --
    # nothing close to the old hardcoded `len(dates) * 2 // 3` (which would
    # put train_end at index 13, 65%).
    dates = _dates(20)
    config_dir = tmp_path / "configs" / "validation"
    config_dir.mkdir(parents=True)
    (config_dir / "partitions.yaml").write_text(
        yaml.safe_dump({"schema_version": 1, "train_end": dates[2], "validation_end": dates[4]}),
        encoding="utf-8",
    )

    train_end, validation_end = load_partition_boundaries(tmp_path)
    assert train_end == dates[2]
    assert validation_end == dates[4]

    result = partition(dates, train_end, validation_end)
    assert result.train == tuple(dates[0:3])
    assert result.validation == tuple(dates[3:5])
    assert result.test == tuple(dates[5:20])

    # The regression this guards against: if load_partition_boundaries() were
    # bypassed and the generator reverted to `len(dates) * 2 // 3`, train
    # would be 13 dates wide, not 3 -- this assertion fails under that defect.
    hardcoded_split = len(dates) * 2 // 3
    assert len(result.train) != hardcoded_split
    assert result.train != tuple(dates[0:hardcoded_split])


def test_tuning_evidence_run_never_passes_test_observations_to_stability_search(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    # AEGIS-139 M5 repair, the decisive reproducer: spy on the REAL argument
    # compute_parameter_stability_surface receives when the actual evidence
    # generator runs, not on an isolated call we construct ourselves and not
    # on counts written to an artifact afterward. _write is monkeypatched to
    # a no-op so this test has no filesystem side effect on committed
    # evidence; every other line of generate_validation_evidence.main() -- the
    # real load_partition_boundaries(), the real partition(), the real
    # select_non_test_for_tuning() -- runs unmodified.
    import generate_validation_evidence as gen

    captured: dict[str, object] = {}
    original_stability_call = gen.compute_parameter_stability_surface

    def spy(observations, *args, **kwargs):  # type: ignore[no-untyped-def]
        captured["observations"] = observations
        return original_stability_call(observations, *args, **kwargs)

    real_partitions = partition(
        [o.as_of for o in gen.make_synthetic_spread_series("EQX", seed=gen.SEED)],
        *load_partition_boundaries(gen.ROOT),
    )

    monkeypatch.setattr(gen, "compute_parameter_stability_surface", spy)
    monkeypatch.setattr(gen, "_write", lambda *a, **kw: None)
    exit_code = gen.main()

    assert exit_code == 0
    assert "observations" in captured
    tuning_observations = captured["observations"]
    assert len(tuning_observations) > 0

    allowed_dates = set(real_partitions.train) | set(real_partitions.validation)
    test_dates = set(real_partitions.test)
    observed_dates = {o.as_of for o in tuning_observations}

    assert observed_dates <= allowed_dates
    assert observed_dates.isdisjoint(test_dates)
    assert max(observed_dates) < min(test_dates)


def test_tuning_purpose_cannot_reach_the_committed_test_partition(repo_root: Path) -> None:
    # AEGIS-139 M5 repair: through the SAME real partition-access abstraction
    # the generator uses (DatasetPartitions.get), against the real committed
    # configs/validation/partitions.yaml boundaries -- not a synthetic
    # fixture -- a TUNING-purpose request for TEST must raise, and it must
    # raise before any observation is returned.
    dates = _dates(120)
    train_end, validation_end = load_partition_boundaries(repo_root)
    real_partitions = partition(dates, train_end, validation_end)

    with pytest.raises(LockedTestPartitionError, match="AEGIS-139"):
        real_partitions.get(PartitionName.TEST, purpose=RunPurpose.TUNING)

    # select_non_test_for_tuning must never surface a test-partition date
    # even though it is handed every date in the series.
    tuning_dates = select_non_test_for_tuning(dates, date_of=lambda d: d, partitions=real_partitions)
    assert set(tuning_dates).isdisjoint(real_partitions.test)
    assert set(tuning_dates) == set(real_partitions.train) | set(real_partitions.validation)
