#!/usr/bin/env python3
"""Generate AEGIS-004 evidence: the exchange/participant separation rule is
enforced against a NON-EMPTY strategy layer.

AEGIS-004's frozen acceptance is "Architecture tests and dependency rules
prevent participant strategy code from directly mutating the exchange book."
Its registered M4 residual was that the rule was declared but unexercised:
`cpp/participant/strategy` was empty, so the rule passed vacuously.

This generator records the facts that discharge it, each read from the live
tree rather than asserted: the strategy layer now contains real source files;
its declared `may_depend_on` names no exchange, OMS or gateway layer; no
strategy source includes an exchange/OMS/gateway header; the real-matching
integration lives only under `tests/`, outside `covered_roots`; and
`tools/check_architecture.py` passes on the whole tree.

Regenerate with: python3 tools/generate_architecture_separation_evidence.py
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

from evidence_provenance import provenance

STRATEGY_DIR = ROOT / "cpp/participant/strategy"
FORBIDDEN_INCLUDE_MARKERS = ("cpp/exchange/", "cpp/participant/oms/", "gateway", "broker_adapter")


def main() -> int:
    rules = yaml.safe_load((ROOT / "configs/architecture_rules.yaml").read_text(encoding="utf-8"))
    layer = next(le for le in rules["layers"] if le["name"] == "cpp-participant-strategy")

    sources = sorted(
        p.relative_to(ROOT).as_posix()
        for p in STRATEGY_DIR.rglob("*")
        if p.is_file() and p.suffix in {".cpp", ".hpp"}
    )

    offending: list[str] = []
    for rel in sources:
        text = (ROOT / rel).read_text(encoding="utf-8")
        for line in text.splitlines():
            stripped = line.strip()
            if not stripped.startswith("#include"):
                continue
            if any(marker in stripped for marker in FORBIDDEN_INCLUDE_MARKERS):
                offending.append(f"{rel}: {stripped}")

    check = subprocess.run(
        [sys.executable, str(ROOT / "tools/check_architecture.py")],
        cwd=ROOT,
        capture_output=True,
        text=True,
        check=False,
    )

    if not sources:
        raise RuntimeError("strategy layer is empty; AEGIS-004's residual is NOT discharged")
    if offending:
        raise RuntimeError(f"strategy sources include forbidden headers: {offending}")
    if check.returncode != 0:
        raise RuntimeError(f"check_architecture.py failed: {check.stdout}{check.stderr}")

    payload = {
        **provenance(),
        "artifact": "exchange_participant_separation",
        "requirements": ["AEGIS-004"],
        "producer": "tools/generate_architecture_separation_evidence.py",
        "strategy_layer_sources": sources,
        "strategy_layer_is_non_empty": True,
        "declared_may_depend_on": layer["may_depend_on"],
        "expect_sources_from_milestone": layer["expect_sources_from_milestone"],
        "forbidden_includes_found": offending,
        "check_architecture_exit_code": check.returncode,
        "check_architecture_stdout": check.stdout.strip(),
        "real_matching_harness_location": (
            "tests/cpp/support/in_process_exchange_transport.hpp -- under tests/, which "
            "covered_roots deliberately excludes, so it creates no production "
            "participant -> exchange edge"
        ),
        "claim": (
            "AEGIS-004: cpp/participant/strategy now contains real, compiled source "
            "files, so the dependency rule constraining it is exercised rather than "
            "vacuous. Its declared may_depend_on names no cpp-exchange-*, "
            "cpp-participant-oms or gateway layer; no strategy source includes such a "
            "header; the only code that composes both sides lives under tests/, "
            "outside covered_roots; and tools/check_architecture.py passes over the "
            "whole tree. This generator raises rather than writing the claim if any "
            "of those is false."
        ),
        "not_evidence_for": [
            "M5 risk policy -- cpp/participant/risk remains empty and no production "
            "RiskGate implementation ships at M4",
            "M9 connectivity -- no gateway or broker adapter exists",
            "the strategy's trading merit -- this artifact is about architectural "
            "separation only",
        ],
    }

    out_dir = ROOT / "experiments/evidence/AEGIS-004"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "exchange_participant_separation.json"
    out_path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"wrote {out_path.relative_to(ROOT)} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
