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
"""

from __future__ import annotations

from collections import deque
from collections.abc import Iterator, Sequence

__all__ = ["rolling_zscore_reference"]


def rolling_zscore_reference(values: Sequence[float], window: int) -> Iterator[float]:
    """Yields one score per input value, in order. ``0.0`` when the prior
    window has fewer than two observations or zero variance -- the same
    documented edge case ``RollingZScore::push_and_score`` returns, so the
    two are directly comparable value for value.
    """
    if window <= 0:
        raise ValueError(f"window must be > 0, got {window}")

    history: deque[float] = deque(maxlen=window)
    for value in values:
        if len(history) < 2:
            score = 0.0
        else:
            count = len(history)
            mean = sum(history) / count
            variance = sum((x - mean) ** 2 for x in history) / (count - 1)
            score = 0.0 if variance == 0 else (value - mean) / (variance**0.5)
        yield score
        history.append(value)
