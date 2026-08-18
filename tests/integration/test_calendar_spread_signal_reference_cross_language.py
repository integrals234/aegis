"""AEGIS-080 closure: the compiled C++ `RollingZScore` (through the existing
`cpp-bindings -> cpp-statistics` edge) and the independent Python signal
reference (`research/signal_reference.py`) agree, value for value, on the
identical sequence `tests/cpp/unit/test_calendar_spread_strategy.cpp` already
verifies the production `CalendarSpreadStrategy` against.

**Independence, precisely.** This is deliberately not
`tests/integration/test_online_stats_cross_language.py`'s comparison
(`aegis_bindings` against `python/common/online_stats.py`, a step-for-step
transliteration of the C++ Welford recursion, per ADR-0022 -- that proves the
binding layer transports values faithfully, not that the recursion is
correct). `research.signal_reference.rolling_zscore_reference` is computed
from the textbook rolling mean/sample-variance definition over a plain
window, a different algorithm from the Welford update, matching the
independence discipline ADR-0022 established for AEGIS-107. Agreement here is
a genuine cross-check of the leakage-free scoring rule itself, and -- via the
threshold classification below -- of the entry/exit decisions built on it.

No new binding is added: `rolling_zscore_batch` already exists
(`cpp/bindings/aegis_module.cpp`, AEGIS-107). There is no
`cpp-bindings -> cpp-participant-strategy` edge, and this file does not add
one -- it compares the generic statistic, not the strategy object.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest
from research.signal_reference import rolling_zscore_reference

pytestmark = pytest.mark.integration

ROOT = Path(__file__).resolve().parents[2]
BUILD_HINT = (
    "The compiled extension was not found. Build it with:\n"
    "    cmake --preset debug && cmake --build --preset debug\n"
    "This test fails rather than skips: a skipped test is not evidence (AEGIS-003)."
)
PRESET_PREFERENCE = ("debug", "release")

TOLERANCE = 1e-9
WINDOW = 20
ENTRY_THRESHOLD = 2.0
EXIT_THRESHOLD = 0.5

# The identical six-value spread sequence
# tests/cpp/unit/test_calendar_spread_strategy.cpp verifies produces an entry
# at index 2 and an exit at index 5 against the compiled production strategy.
SPREADS = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]


def _load_bindings():
    candidates = [ROOT / f"build/{preset}/cpp/bindings" for preset in PRESET_PREFERENCE]
    candidates += [p for p in sorted(ROOT.glob("build/*/cpp/bindings")) if p not in candidates]
    for directory in candidates:
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    try:
        import aegis_bindings
    except ImportError as exc:  # pragma: no cover - exercised when the build is absent
        pytest.fail(f"{BUILD_HINT}\n\nUnderlying import error: {exc}")
    return aegis_bindings


@pytest.fixture(scope="module")
def bindings():
    return _load_bindings()


def _classify(scores: list[float]) -> list[tuple[int, str]]:
    """The same flat/long/short state machine
    `cpp/participant/strategy/calendar_spread_strategy.cpp` and
    `research/strategy_replay.py` both implement, applied here directly to a
    raw score list so this file needs no CalendarSpreadObservation
    machinery -- only the classification rule itself is under test."""
    position = "flat"
    actions: list[tuple[int, str]] = []
    for i, z in enumerate(scores):
        if position == "flat":
            if z <= -ENTRY_THRESHOLD:
                position = "long_spread"
                actions.append((i, "enter_long"))
            elif z >= ENTRY_THRESHOLD:
                position = "short_spread"
                actions.append((i, "enter_short"))
        elif abs(z) <= EXIT_THRESHOLD:
            actions.append((i, "exit"))
            position = "flat"
    return actions


def test_compiled_and_reference_scores_agree_value_for_value(bindings) -> None:
    compiled_scores = bindings.rolling_zscore_batch(SPREADS, WINDOW)
    reference_scores = list(rolling_zscore_reference(SPREADS, WINDOW))

    assert len(compiled_scores) == len(reference_scores) == len(SPREADS)
    for i, (compiled, reference) in enumerate(zip(compiled_scores, reference_scores, strict=True)):
        assert compiled == pytest.approx(reference, abs=TOLERANCE), f"mismatch at index {i}"


def test_compiled_and_reference_entry_exit_classifications_match(bindings) -> None:
    """The same timestamps produce the same trading decision under both
    implementations -- not just numerically close scores, but the identical
    downstream entry/exit call at every index, matching the compiled
    strategy's own verified index-2-entry/index-5-exit behaviour."""
    compiled_scores = bindings.rolling_zscore_batch(SPREADS, WINDOW)
    reference_scores = list(rolling_zscore_reference(SPREADS, WINDOW))

    compiled_actions = _classify(compiled_scores)
    reference_actions = _classify(reference_scores)

    assert compiled_actions == reference_actions
    assert compiled_actions == [(2, "enter_short"), (5, "exit")]


def test_first_observation_never_leaks_into_its_own_score_in_either_implementation(bindings) -> None:
    """AEGIS-080's leakage requirement, checked in both implementations at
    once: an extreme first value must never score against itself."""
    extreme_first = [1000.0, 1.0, 2.0, 1.5]
    compiled = bindings.rolling_zscore_batch(extreme_first, WINDOW)
    reference = list(rolling_zscore_reference(extreme_first, WINDOW))

    assert compiled[0] == pytest.approx(0.0, abs=TOLERANCE)
    assert reference[0] == pytest.approx(0.0, abs=TOLERANCE)
