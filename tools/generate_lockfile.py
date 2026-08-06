#!/usr/bin/env python3
"""Turn a fully-resolved ``pip freeze`` listing into a hash-pinned lockfile.

AEGIS-228 requires the Python environment to be reproducible, which means
``pip install --require-hashes``. This tool reads pinned ``name==version`` lines
and emits every sha256 PyPI publishes for that exact release, so the same
lockfile installs on each supported interpreter without re-resolving.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PYPI_JSON = "https://pypi.org/pypi/{name}/{version}/json"

HEADER = """\
# AEGIS pinned Python environment (AEGIS-228). GENERATED - do not edit by hand.
#
# Regenerate with:  bash scripts/lock_python.sh
# Install with:     pip install --require-hashes -r requirements/requirements.lock
#
# Direct dependencies are declared in requirements/python-requirements.in.
# Every release listed here carries all sha256 digests PyPI publishes for that
# version, so one lockfile serves every interpreter in docs/ENVIRONMENT.md.
"""


def parse_pins(text: str) -> list[tuple[str, str]]:
    pins: list[tuple[str, str]] = []
    for raw in text.splitlines():
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "==" not in line:
            raise SystemExit(f"lock input must be fully pinned, got: {raw!r}")
        name, _, version = line.partition("==")
        pins.append((name.strip(), version.strip()))
    return sorted(pins, key=lambda pin: pin[0].lower())


def hashes_for(name: str, version: str, timeout: float) -> list[str]:
    url = PYPI_JSON.format(name=name, version=version)
    try:
        with urllib.request.urlopen(url, timeout=timeout) as response:
            payload = json.load(response)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as exc:
        raise SystemExit(f"cannot fetch hashes for {name}=={version}: {exc}") from exc
    digests = sorted(
        {entry["digests"]["sha256"] for entry in payload.get("urls", []) if entry.get("digests", {}).get("sha256")}
    )
    if not digests:
        raise SystemExit(f"no sha256 digests published for {name}=={version}")
    return digests


def render(pins: list[tuple[str, str]], timeout: float) -> str:
    blocks = [HEADER]
    for name, version in pins:
        digests = hashes_for(name, version, timeout)
        lines = [f"{name}=={version} \\"]
        for index, digest in enumerate(digests):
            suffix = "" if index == len(digests) - 1 else " \\"
            lines.append(f"    --hash=sha256:{digest}{suffix}")
        blocks.append("\n".join(lines))
    return "\n".join(blocks) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, help="pip freeze output; defaults to stdin")
    parser.add_argument("--output", type=Path, default=ROOT / "requirements/requirements.lock")
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args(argv)

    text = args.input.read_text(encoding="utf-8") if args.input else sys.stdin.read()
    pins = parse_pins(text)
    if not pins:
        print("ERROR: no pins supplied", file=sys.stderr)
        return 2
    args.output.write_text(render(pins, args.timeout), encoding="utf-8")
    print(f"wrote {args.output} with {len(pins)} pinned distributions")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
