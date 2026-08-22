"""Leakage-free rolling z-score reference (AEGIS-080; ADR-0026).

Independent of ``cpp/statistics/rolling_zscore.hpp`` -- computed directly
from the textbook rolling mean/sample-variance definition over a plain
window, not a transliteration of the C++ Welford recursion, matching the
independence discipline ADR-0022 established for AEGIS-107. A later batch
cross-checks this against the compiled C++ strategy's own z-score through the
existing ``cpp-bindings -> cpp-statistics`` edge; this module ships the
reference itself.

Convention, matching ``RollingZScore``'s own contract exactly: an observation
is scored against the window as it stood *before* it, then joins the window
-- it never influences its own normalisation.

# Timing provenance (M5, AEGIS-152/153) -- estimator-derived, not formula-derived

``timing_sink``, if supplied, receives one :class:`WindowProvenance` per
yielded score. The critical property, and the one the first version of this
fix got wrong: ``fitting_window_start_index``/``fitting_window_end_index``
must be read from the SAME structure the score itself was just computed
from, never recomputed from ``index`` and a count. The first version read
``fitting_window_start_index`` that way but left
``fitting_window_end_index = index - 1`` as a constant expression -- true
for every *correctly behaving* call, but never actually checked against
what execution consumed, so a hypothetical future bug in this very function
could leak data while the emitted provenance kept reporting the honest
answer. That is fixed here by tracking ``(index, value)`` pairs in the
sliding window (``_consumed``) instead of bare values, and reading both
boundaries directly off that deque -- ``_consumed[0][0]`` /
``_consumed[-1][0]`` when non-empty, meaning the provenance is a readout of
memory the loop actually holds, not an assertion about what it should hold.

``python.validation.leakage``'s independent audit consumes this real
provenance; this module does not import that layer (``python-research`` may
not depend on ``python-validation``, ``configs/architecture_rules.yaml``),
so the dependency runs the other way -- validation reads research's own
record of what it did.

# The falsifiability seam

:func:`rolling_zscore_reference_with_seeded_leak_for_falsifiability_check`
exists ONLY so AEGIS-152's negative-gate test has a genuinely leaking
execution to catch -- never call it to score a real signal.
``_execute_rolling_zscore`` is the one execution engine both this and the
honest :func:`rolling_zscore_reference` run; the only difference between
them is *when* the current observation joins ``_consumed`` relative to
scoring -- after (honest: the window never includes itself) or before
(leaky: the window includes itself, which is the textbook look-ahead bug).
Because provenance is always read from ``_consumed`` after that join has or
hasn't happened, the leak's effect on the emitted timing records is a
natural consequence of shared arithmetic, not a hand-authored violation
bolted onto a copy of the loop.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass
from enum import Enum, auto

__all__ = [
    "WindowProvenance",
    "rolling_zscore_reference",
    "rolling_zscore_reference_with_seeded_leak_for_falsifiability_check",
]


@dataclass(frozen=True, slots=True)
class WindowProvenance:
    """What one scoring step actually read: the feature's own index, and the
    inclusive index range of the values consumed to score it. When nothing
    was consumed yet, ``fitting_window_start_index > fitting_window_end_index``
    represents the empty range honestly -- never a fabricated non-empty one."""

    feature_index: int
    fitting_window_start_index: int
    fitting_window_end_index: int


class _WindowUpdateOrder(Enum):
    """Internal only -- never part of this module's public contract.

    PRIOR_ONLY is the honest, documented behaviour every real caller of
    :func:`rolling_zscore_reference` gets: the current observation joins the
    window strictly after being scored against it. LEAK_CURRENT is exercised
    exclusively by the falsifiability check below: the current observation
    joins the window BEFORE scoring, so it scores itself -- the canonical
    look-ahead bug -- using the exact same downstream arithmetic and
    provenance readout as the honest path.
    """

    PRIOR_ONLY = auto()
    LEAK_CURRENT = auto()


def _execute_rolling_zscore(
    values: Sequence[float],
    window: int,
    timing_sink: Callable[[WindowProvenance], None] | None,
    update_order: _WindowUpdateOrder,
) -> Iterator[float]:
    if window <= 0:
        raise ValueError(f"window must be > 0, got {window}")

    # (index, value) pairs, oldest first -- the ONE structure both the score
    # and the emitted provenance are read from, so they cannot disagree.
    consumed: deque[tuple[int, float]] = deque(maxlen=window)

    for index, value in enumerate(values):
        if update_order is _WindowUpdateOrder.LEAK_CURRENT:
            consumed.append((index, value))  # THE LEAK: joins before scoring.

        observed_count = len(consumed)
        if timing_sink is not None:
            if consumed:
                start_index, end_index = consumed[0][0], consumed[-1][0]
            else:
                # Nothing consumed yet: the canonical empty range anchored at
                # this feature's own index, not a claim about a non-existent
                # window.
                start_index, end_index = index, index - 1
            timing_sink(
                WindowProvenance(
                    feature_index=index,
                    fitting_window_start_index=start_index,
                    fitting_window_end_index=end_index,
                )
            )

        if observed_count < 2:
            score = 0.0
        else:
            window_values = [v for _, v in consumed]
            mean = sum(window_values) / observed_count
            variance = sum((x - mean) ** 2 for x in window_values) / (observed_count - 1)
            score = 0.0 if variance == 0 else (value - mean) / (variance**0.5)
        yield score

        if update_order is _WindowUpdateOrder.PRIOR_ONLY:
            consumed.append((index, value))  # Honest: joins the window after being scored.


def rolling_zscore_reference(
    values: Sequence[float],
    window: int,
    timing_sink: Callable[[WindowProvenance], None] | None = None,
) -> Iterator[float]:
    """Yields one score per input value, in order. ``0.0`` when the prior
    window has fewer than two observations or zero variance -- the same
    documented edge case ``RollingZScore::push_and_score`` returns, so the
    two are directly comparable value for value.
    """
    yield from _execute_rolling_zscore(values, window, timing_sink, _WindowUpdateOrder.PRIOR_ONLY)


def rolling_zscore_reference_with_seeded_leak_for_falsifiability_check(
    values: Sequence[float],
    window: int,
    timing_sink: Callable[[WindowProvenance], None] | None = None,
) -> Iterator[float]:
    """NEGATIVE-TEST-ONLY. Exists solely so AEGIS-152's leakage detector has
    a genuinely leaking execution to catch. Runs the identical scoring and
    provenance-readout code as :func:`rolling_zscore_reference` -- the only
    difference is that the current observation joins the window before it is
    scored, so both the yielded numbers AND the emitted provenance shift
    together, as a consequence of shared arithmetic, never as a
    separately-authored violation.

    Never call this to score a real signal; ``rolling_zscore_reference`` is
    the only estimator any AEGIS strategy or validation module is judged
    against.
    """
    yield from _execute_rolling_zscore(values, window, timing_sink, _WindowUpdateOrder.LEAK_CURRENT)
