"""AEGIS-005, ADR-0012 — the exchange engine reads no clock and no RNG.

``ExchangeTime = max(previous, command.event_time)`` is derived purely from
input (ADR-0012); a `system_clock`/`steady_clock`/`random_device` read inside
the engine would make two runs of the same command stream diverge, which is
exactly the failure category a unit test on engine *output* would not
reliably catch — the divergence might not show up until a different process,
a different day, or a different core count. Grepping the source is cheap and
catches it structurally instead.
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest

pytestmark = pytest.mark.unit

ROOT = Path(__file__).resolve().parents[2]
EXCHANGE_ROOT = ROOT / "cpp/exchange"

FORBIDDEN = re.compile(
    r"\b(std::)?(system_clock|steady_clock|high_resolution_clock|random_device)\b|\brand\s*\(|\btime\s*\("
)

CPP_SUFFIXES = {".hpp", ".cpp", ".h", ".cc"}


def _exchange_sources() -> list[Path]:
    if not EXCHANGE_ROOT.exists():
        return []
    return [
        path
        for path in EXCHANGE_ROOT.rglob("*")
        if path.is_file() and path.suffix in CPP_SUFFIXES
    ]


def test_the_source_tree_actually_has_exchange_files():
    # A lint that finds nothing to scan proves nothing.
    assert _exchange_sources(), "cpp/exchange contains no C++ sources to lint"


@pytest.mark.parametrize("path", _exchange_sources(), ids=lambda p: p.relative_to(ROOT).as_posix())
def test_no_clock_or_random_reads_in_exchange_sources(path):
    text = path.read_text(encoding="utf-8")
    match = FORBIDDEN.search(text)
    assert match is None, (
        f"{path.relative_to(ROOT)} reads a clock or RNG directly "
        f"({match.group(0) if match else ''}); the engine must derive everything from its input "
        "(ADR-0012). MonotonicTime readings belong only in bench_main.cpp, via "
        "aegis::common::SystemSteadyClock — never a raw std::chrono call — per experiments/plans/M1.md §3"
    )
