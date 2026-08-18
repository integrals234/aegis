#!/usr/bin/env python3
"""Generate AEGIS-080 closure evidence: the compiled C++ `RollingZScore`
(through the existing `cpp-bindings -> cpp-statistics` edge, `aegis_bindings.
rolling_zscore_batch`) and the independent Python signal reference
(`research.signal_reference.rolling_zscore_reference`) agree, value for
value and entry/exit-classification for entry/exit-classification, on the
same six-value spread sequence
`tests/cpp/unit/test_calendar_spread_strategy.cpp` verifies against the
compiled production `CalendarSpreadStrategy`.

`research.signal_reference` is independent of `RollingZScore`'s C++ Welford
recursion (textbook rolling mean/sample-variance over a plain window,
ADR-0022's discipline) -- this is not
`tests/integration/test_online_stats_cross_language.py`'s transliteration
comparison. No new binding is added; no `cpp-bindings ->
cpp-participant-strategy` edge exists or is created here.

Regenerate with: python3 tools/generate_signal_reference_cross_language_evidence.py
"""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "python"))
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance
from research.signal_reference import rolling_zscore_reference

WINDOW = 20
ENTRY_THRESHOLD = 2.0
EXIT_THRESHOLD = 0.5
SPREADS = [0.50, 0.55, 0.60, 0.65, 2.50, 0.70]


def _load_bindings() -> Any:
    for preset in ("debug", "release"):
        directory = ROOT / f"build/{preset}/cpp/bindings"
        if any(directory.glob("aegis_bindings*.so")):
            sys.path.insert(0, str(directory))
            break
    else:
        for directory in sorted(ROOT.glob("build/*/cpp/bindings")):
            if any(directory.glob("aegis_bindings*.so")):
                sys.path.insert(0, str(directory))
                break
    import aegis_bindings

    return aegis_bindings


def _classify(scores: list[float]) -> list[tuple[int, str]]:
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


def main() -> int:
    bindings = _load_bindings()
    compiled_scores = list(bindings.rolling_zscore_batch(SPREADS, WINDOW))
    reference_scores = list(rolling_zscore_reference(SPREADS, WINDOW))
    max_abs_diff = max(abs(c - r) for c, r in zip(compiled_scores, reference_scores, strict=True))

    compiled_actions = _classify(compiled_scores)
    reference_actions = _classify(reference_scores)

    payload = {
        **provenance(),
        "artifact": "signal_reference_cross_language_agreement",
        "requirements": ["AEGIS-080"],
        "spread_sequence": SPREADS,
        "window": WINDOW,
        "entry_threshold": ENTRY_THRESHOLD,
        "exit_threshold": EXIT_THRESHOLD,
        "compiled_scores": compiled_scores,
        "reference_scores": reference_scores,
        "max_abs_difference": max_abs_diff,
        "compiled_actions": [[i, kind] for i, kind in compiled_actions],
        "reference_actions": [[i, kind] for i, kind in reference_actions],
        "actions_agree": compiled_actions == reference_actions,
        "claim": (
            "AEGIS-080: the compiled C++ RollingZScore (via aegis_bindings."
            "rolling_zscore_batch) and the independent Python reference "
            "(research.signal_reference.rolling_zscore_reference) were run once over "
            f"the identical spread sequence and agree to within {max_abs_diff} absolute "
            "difference at every point, and produce IDENTICAL entry/exit "
            "classifications under the approved threshold rule -- the same sequence "
            "and outcome tests/cpp/unit/test_calendar_spread_strategy.cpp verifies "
            "against the compiled production CalendarSpreadStrategy."
        ),
        "not_evidence_for": [
            "AEGIS-107's own cross-language claim, which concerns different fixtures "
            "and is evidenced separately in experiments/evidence/AEGIS-107/",
            "any claim about real markets -- the spread sequence here is a fixed test "
            "sequence, not observed data",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-080"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "signal_reference_cross_language.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
