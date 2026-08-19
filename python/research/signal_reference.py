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

# Timing provenance (M5, AEGIS-152)

``timing_sink``, if supplied, receives one :class:`WindowProvenance` per
yielded score, describing the window THIS CALL ACTUALLY READ --
``fitting_window_start_index``/``fitting_window_end_index`` are computed
from ``len(history)`` at the moment of scoring, not reconstructed afterward
from the documented convention above. ``python.validation.leakage``'s
independent audit consumes this real provenance; this module does not
import that layer (``python-research`` may not depend on
``python-validation``, ``configs/architecture_rules.yaml``), so the
dependency runs the other way -- validation reads research's own record of
what it did.
"""

from __future__ import annotations

from collections import deque
from collections.abc import Callable, Iterator, Sequence
from dataclasses import dataclass

__all__ = ["WindowProvenance", "rolling_zscore_reference"]


@dataclass(frozen=True, slots=True)
class WindowProvenance:
    """What one scoring step actually read: the feature's own index, and the
    inclusive index range of the values consumed to score it."""

    feature_index: int
    fitting_window_start_index: int
    fitting_window_end_index: int


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
    if window <= 0:
        raise ValueError(f"window must be > 0, got {window}")

    history: deque[float] = deque(maxlen=window)
    for index, value in enumerate(values):
        observed_count = len(history)  # Read BEFORE this value joins the window.
        if timing_sink is not None:
            timing_sink(
                WindowProvenance(
                    feature_index=index,
                    fitting_window_start_index=index - observed_count,
                    fitting_window_end_index=index - 1,
                )
            )
        if observed_count < 2:
            score = 0.0
        else:
            mean = sum(history) / observed_count
            variance = sum((x - mean) ** 2 for x in history) / (observed_count - 1)
            score = 0.0 if variance == 0 else (value - mean) / (variance**0.5)
        yield score
        history.append(value)
