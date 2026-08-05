#!/usr/bin/env python3
"""Machine-checked Python-version compatibility probe for AEGIS pinned dependencies.

AEGIS-228 requires a pinned Python environment. Before a baseline interpreter is
adopted, every pinned direct dependency must be shown to be installable on it.
This probe answers that question from PyPI metadata rather than from prose:
for each pinned distribution it reports, per candidate interpreter, whether a
compatible wheel exists, whether only an sdist exists, or whether the release is
unusable because ``requires_python`` excludes the interpreter.

The probe never installs anything and never mutates the environment.
"""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass, field
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_REQUIREMENTS = ROOT / "requirements/python-requirements.in"
DEFAULT_EVIDENCE = ROOT / "experiments/evidence/AEGIS-228/dependency_probe.json"
DEFAULT_TARGETS = ("3.12", "3.14")
PYPI_JSON = "https://pypi.org/pypi/{name}/{version}/json"

WHEEL = "bdist_wheel"
SDIST = "sdist"


@dataclass
class PackageResult:
    name: str
    version: str
    per_target: dict[str, str] = field(default_factory=dict)
    error: str | None = None


def parse_requirements(path: Path) -> list[tuple[str, str]]:
    """Parse a strictly pinned ``name==version`` requirements file."""
    pins: list[tuple[str, str]] = []
    for lineno, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        if "==" not in line:
            raise SystemExit(f"{path}:{lineno}: dependency must be pinned with '==': {raw!r}")
        name, _, version = line.partition("==")
        pins.append((name.strip(), version.strip()))
    return pins


def cpython_tag(target: str) -> str:
    major, minor = target.split(".")
    return f"cp{major}{minor}"


def requires_python_allows(spec: str | None, target: str) -> bool:
    """Evaluate the subset of PEP 440 specifiers PyPI actually uses here.

    Only ``>=``/``>``/``<=``/``<``/``!=``/``==`` against dotted release numbers
    appear in practice for ``requires_python``. Anything unrecognised is treated
    as permissive so the probe reports wheel evidence rather than a parse error.
    """
    if not spec:
        return True
    target_key = tuple(int(part) for part in target.split("."))

    def key(value: str) -> tuple[int, ...]:
        value = value.strip().rstrip(".*")
        return tuple(int(part) for part in value.split(".") if part.isdigit())

    for clause in spec.split(","):
        clause = clause.strip()
        if not clause:
            continue
        for op in ("<=", ">=", "==", "!=", "<", ">", "~="):
            if clause.startswith(op):
                bound = key(clause[len(op) :])
                if not bound:
                    break
                width = min(len(target_key), len(bound))
                lhs, rhs = target_key[:width], bound[:width]
                if op == ">=" and not lhs >= rhs:
                    return False
                if op == ">" and not target_key > bound:
                    return False
                if op == "<=" and not lhs <= rhs:
                    return False
                if op == "<" and not target_key < bound:
                    return False
                if op == "==" and lhs != rhs:
                    return False
                if op == "!=" and lhs == rhs:
                    return False
                if op == "~=" and not lhs >= rhs:
                    return False
                break
    return True


def wheel_supports(filename: str, target: str) -> bool:
    """Decide whether a wheel filename's compatibility tags cover ``target``."""
    stem = filename[: -len(".whl")]
    parts = stem.split("-")
    if len(parts) < 5:
        return False
    python_tags = parts[-3].split(".")
    tag = cpython_tag(target)
    major = target.split(".")[0]
    for python_tag in python_tags:
        if python_tag == tag:
            return True
        if python_tag in (f"py{major}", "py2.py3"):
            return True
        if python_tag.startswith("py") and python_tag[2:].isdigit() and python_tag[2:] == major:
            return True
        # abi3 wheels built for an older CPython remain usable on newer ones.
        if python_tag.startswith("cp") and "abi3" in parts[-2]:
            built = python_tag[2:]
            if built.isdigit() and len(built) >= 3:
                built_key = (int(built[0]), int(built[1:]))
                if built_key <= tuple(int(p) for p in target.split(".")):
                    return True
    return False


def fetch(name: str, version: str, timeout: float) -> dict:
    url = PYPI_JSON.format(name=name, version=version)
    with urllib.request.urlopen(url, timeout=timeout) as response:  # noqa: S310 - fixed https host
        return json.load(response)


def probe_package(name: str, version: str, targets: tuple[str, ...], timeout: float) -> PackageResult:
    result = PackageResult(name=name, version=version)
    try:
        payload = fetch(name, version, timeout)
    except (urllib.error.URLError, urllib.error.HTTPError, TimeoutError) as exc:
        result.error = f"metadata unavailable: {exc}"
        return result

    requires_python = payload.get("info", {}).get("requires_python")
    urls = payload.get("urls", [])
    wheels = [u["filename"] for u in urls if u.get("packagetype") == WHEEL]
    has_sdist = any(u.get("packagetype") == SDIST for u in urls)

    for target in targets:
        if not requires_python_allows(requires_python, target):
            result.per_target[target] = f"excluded (requires_python {requires_python})"
            continue
        if any(wheel_supports(filename, target) for filename in wheels):
            result.per_target[target] = "wheel"
        elif has_sdist:
            result.per_target[target] = "sdist-only"
        else:
            result.per_target[target] = "unavailable"
    return result


def summarise(results: list[PackageResult], targets: tuple[str, ...]) -> dict[str, str]:
    """Reduce per-package outcomes to one verdict per interpreter."""
    verdicts: dict[str, str] = {}
    for target in targets:
        outcomes = [r.per_target.get(target, "error") for r in results]
        if any(r.error for r in results):
            verdicts[target] = "inconclusive"
        elif all(o == "wheel" for o in outcomes):
            verdicts[target] = "supported"
        elif any(o.startswith("excluded") or o in ("unavailable", "error") for o in outcomes):
            verdicts[target] = "unsupported"
        else:
            verdicts[target] = "supported-with-source-builds"
    return verdicts


def render(results: list[PackageResult], targets: tuple[str, ...], verdicts: dict[str, str]) -> str:
    width = max(len(f"{r.name}=={r.version}") for r in results)
    lines = [f"{'distribution'.ljust(width)}  " + "  ".join(t.ljust(28) for t in targets)]
    for r in results:
        label = f"{r.name}=={r.version}".ljust(width)
        if r.error:
            lines.append(f"{label}  ERROR: {r.error}")
            continue
        lines.append(label + "  " + "  ".join(r.per_target[t].ljust(28) for t in targets))
    lines.append("")
    for target in targets:
        lines.append(f"verdict python{target}: {verdicts[target]}")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requirements", type=Path, default=DEFAULT_REQUIREMENTS)
    parser.add_argument("--target", action="append", dest="targets", default=[])
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--write-evidence", action="store_true")
    parser.add_argument("--evidence-path", type=Path, default=DEFAULT_EVIDENCE)
    args = parser.parse_args(argv)

    targets = tuple(args.targets) if args.targets else DEFAULT_TARGETS
    pins = parse_requirements(args.requirements)
    if not pins:
        print(f"ERROR: no pinned dependencies in {args.requirements}", file=sys.stderr)
        return 2

    results = [probe_package(name, version, targets, args.timeout) for name, version in pins]
    verdicts = summarise(results, targets)
    print(render(results, targets, verdicts))

    if args.write_evidence:
        record = {
            "probe_version": 1,
            "requirements_file": args.requirements.relative_to(ROOT).as_posix(),
            "targets": list(targets),
            "packages": [
                {
                    "name": r.name,
                    "version": r.version,
                    "per_target": r.per_target,
                    "error": r.error,
                }
                for r in results
            ],
            "verdicts": verdicts,
        }
        args.evidence_path.parent.mkdir(parents=True, exist_ok=True)
        args.evidence_path.write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        print(f"\nwrote {args.evidence_path.relative_to(ROOT).as_posix()}")

    if any(r.error for r in results):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
