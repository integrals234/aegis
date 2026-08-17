"""AEGIS M4 report foundation (AEGIS-079, AEGIS-081, AEGIS-024).

`python-reports` may depend on `python-common`, `python-data`,
`python-futures` and `python-research` only (`configs/architecture_rules.yaml`)
-- explicitly not `python-validation` (M5) or `python-attribution` (M6),
neither of which exists yet. `report_model.py` is the shared deterministic
foundation every M4 report content module builds on; report *content*
(stationarity, roll/expiry attribution, roll-method sensitivity) is later
Batch 1 work.
"""
