"""M5 validation and anti-overfitting framework (AEGIS-139..155).

Ownership boundary (ADR-0029): ``python/research`` owns *how a strategy
behaves* (signals, replay, term structure, roll policy). This package owns
*whether a research result may be believed* -- partitioning, resampling,
sensitivity, baselines, leakage detection, and formal rejection. Reports
render and stamp provenance only (``python/reports``); nothing here computes
a claim ``python/reports`` did not itself derive from committed inputs.
"""

from __future__ import annotations
