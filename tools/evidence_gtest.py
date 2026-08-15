#!/usr/bin/env python3
"""Run real GoogleTest binaries and report what actually happened (AEGIS-003).

Several M3 acceptance criteria are phrased as "fixtures pass" / "scenario
tests pass" / "online-offline equivalence tests pass". The honest evidence
for such a criterion is the named acceptance tests themselves, executed
against the real production classes, with their real outcome recorded --
not a Python reimplementation of the same arithmetic, which would prove
only that the reimplementation agrees with itself.

So the M3 evidence generators call :func:`run_suite` here, which shells out
to the actual compiled test binary with a ``--gtest_filter`` and parses
GoogleTest's own JSON report. Each recorded case carries the test's source
file and line, so a reader can open the assertion that was made rather than
taking "passed" on trust.

This module deliberately reports failures rather than raising on them: a
generator that dies on the first red test cannot write an artifact saying
*which* test was red, and an evidence set that can only be produced when
everything is green is a set nobody can use to diagnose anything.

``evidence_provenance`` is the sibling module for the provenance stamp;
this one is only about test execution.
"""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]

# Default locations under the debug preset, overridable by environment so a
# clean-machine or differently-configured build can still produce evidence.
UNIT_TESTS_ENV = "AEGIS_UNIT_TESTS"
PROPERTY_TESTS_ENV = "AEGIS_PROPERTY_TESTS"
DEFAULT_UNIT_TESTS = "build/debug/tests/cpp/unit/aegis_unit_tests"
DEFAULT_PROPERTY_TESTS = "build/debug/tests/cpp/property/aegis_property_tests"


def resolve_binary(kind: str, root: Path = ROOT) -> Path:
    """Locate a compiled test binary, raising (never skipping) if absent.

    A missing binary must fail evidence generation. AEGIS-003's rule is that
    a skipped check is not evidence; an artifact written after silently
    skipping the tests it claims to have run would be worse than no artifact.
    """
    env_name, default = {
        "unit": (UNIT_TESTS_ENV, DEFAULT_UNIT_TESTS),
        "property": (PROPERTY_TESTS_ENV, DEFAULT_PROPERTY_TESTS),
    }[kind]
    override = os.environ.get(env_name)
    binary = Path(override) if override else root / default
    if not binary.exists():
        raise FileNotFoundError(
            f"{kind} test binary not found at {binary}. Build it with "
            f"'cmake --build --preset debug', or set {env_name} to its path."
        )
    return binary


def run_suite(gtest_filter: str, kind: str = "unit", root: Path = ROOT) -> dict[str, Any]:
    """Execute `gtest_filter` against the real binary and return its outcome.

    Returns the case list plus totals. ``all_passed`` is the single field a
    caller should gate on; ``cases`` is what makes the artifact auditable,
    since each entry names the source file and line of the assertion.
    """
    binary = resolve_binary(kind, root)
    with tempfile.TemporaryDirectory() as tmp:
        report = Path(tmp) / "gtest.json"
        subprocess.run(
            [str(binary), f"--gtest_filter={gtest_filter}", f"--gtest_output=json:{report}"],
            capture_output=True,
            text=True,
            check=False,  # A red test is data for the artifact, not an exception.
        )
        if not report.exists():
            raise RuntimeError(f"gtest produced no JSON report for filter {gtest_filter!r}")
        raw = json.loads(report.read_text())

    cases: list[dict[str, Any]] = []
    for suite in raw.get("testsuites", []):
        for case in suite.get("testsuite", []):
            failures = case.get("failures", [])
            cases.append(
                {
                    "test": f"{suite['name']}.{case['name']}",
                    # Repository-relative so the artifact does not record where
                    # this checkout happens to live.
                    "source": _relative(case.get("file", ""), root),
                    "line": case.get("line"),
                    "passed": not failures,
                }
            )

    total = int(raw.get("tests", 0))
    failed = int(raw.get("failures", 0)) + int(raw.get("errors", 0))
    if total == 0:
        # An empty filter is the silent-stub failure mode: it "passes" while
        # asserting nothing. Callers must never record that as evidence.
        raise RuntimeError(
            f"gtest filter {gtest_filter!r} matched zero tests; an empty suite is not evidence"
        )
    return {
        "filter": gtest_filter,
        "binary": str(resolve_binary(kind, root).relative_to(root))
        if resolve_binary(kind, root).is_relative_to(root)
        else str(resolve_binary(kind, root)),
        "test_count": total,
        "failure_count": failed,
        "all_passed": failed == 0,
        "cases": sorted(cases, key=lambda case: case["test"]),
    }


def _relative(path_text: str, root: Path) -> str:
    if not path_text:
        return ""
    path = Path(path_text)
    try:
        return str(path.resolve().relative_to(root))
    except ValueError:
        return str(path)


def merge(*suites: dict[str, Any]) -> dict[str, Any]:
    """Combine several filtered runs into one record for a requirement.

    Some requirements are proven by tests living in more than one suite (a
    unit binary and the property binary, say); the merged view keeps every
    case while still exposing one ``all_passed`` for the caller to gate on.
    """
    cases: list[dict[str, Any]] = []
    for suite in suites:
        cases.extend(suite["cases"])
    return {
        "filters": [suite["filter"] for suite in suites],
        "binaries": sorted({suite["binary"] for suite in suites}),
        "test_count": sum(suite["test_count"] for suite in suites),
        "failure_count": sum(suite["failure_count"] for suite in suites),
        "all_passed": all(suite["all_passed"] for suite in suites),
        "cases": sorted(cases, key=lambda case: case["test"]),
    }
