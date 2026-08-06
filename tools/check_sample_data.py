#!/usr/bin/env python3
"""Enforce the sample-data policy (AEGIS-236).

Two halves, because the requirement has two: *commit only small legally
redistributable samples*, and *large or licensed data stays external*.

**Committed samples** must be small, of an allowed type, and carry provenance —
where the data came from, under what licence, and what it may be used for. A
sample with no provenance is a file nobody can decide about later: not whether
it may stay, not whether a result derived from it may be published.

**External datasets** must be referenced by immutable version identifier in
``configs/external_datasets.yaml``, with a checksum and access instructions.
"Download the latest from the vendor" is not a reference: the data changes, the
research does not, and the mismatch surfaces as a result nobody can reproduce.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[1]
SAMPLE_DIR = "data_samples"
PROVENANCE_FILE = "PROVENANCE.yaml"
EXTERNAL_DATASETS = "configs/external_datasets.yaml"

# Small enough that a clone stays cheap and a reviewer can actually inspect the
# file. Anything larger is a dataset, and datasets live outside the repository.
MAX_SAMPLE_BYTES = 512 * 1024
MAX_TOTAL_BYTES = 4 * 1024 * 1024

ALLOWED_SUFFIXES = {".csv", ".json", ".jsonl", ".yaml", ".yml", ".md", ".txt", ".parquet"}
EXEMPT_NAMES = {PROVENANCE_FILE, ".gitkeep", "README.md"}

REQUIRED_PROVENANCE_FIELDS = ("source", "licence", "redistributable", "description", "collected_on")
REQUIRED_EXTERNAL_FIELDS = ("dataset_id", "version", "licence", "access", "checksum")


def check_samples(root: Path) -> list[str]:
    errors: list[str] = []
    directory = root / SAMPLE_DIR
    if not directory.exists():
        return [f"{SAMPLE_DIR}/ is missing"]

    files = [p for p in sorted(directory.rglob("*")) if p.is_file()]
    data_files = [p for p in files if p.name not in EXEMPT_NAMES]

    provenance_path = directory / PROVENANCE_FILE
    provenance: dict[str, dict[str, Any]] = {}
    if data_files and not provenance_path.exists():
        errors.append(
            f"{SAMPLE_DIR}/{PROVENANCE_FILE} is missing, so no committed sample can be traced "
            "to a source or a licence"
        )
    elif provenance_path.exists():
        document = yaml.safe_load(provenance_path.read_text(encoding="utf-8")) or {}
        provenance = document.get("samples", {}) or {}

    total = 0
    for path in data_files:
        rel = path.relative_to(directory).as_posix()
        size = path.stat().st_size
        total += size

        if path.suffix not in ALLOWED_SUFFIXES:
            errors.append(
                f"{SAMPLE_DIR}/{rel}: extension {path.suffix or '(none)'} is not in the sample "
                f"allowlist {sorted(ALLOWED_SUFFIXES)}"
            )
        if size > MAX_SAMPLE_BYTES:
            errors.append(
                f"{SAMPLE_DIR}/{rel}: {size} bytes exceeds the {MAX_SAMPLE_BYTES}-byte per-file "
                "cap; a file this size is a dataset and belongs in "
                f"{EXTERNAL_DATASETS}"
            )

        entry = provenance.get(rel)
        if entry is None:
            errors.append(
                f"{SAMPLE_DIR}/{rel}: no provenance entry. A sample nobody can trace is a sample "
                "nobody can decide about later."
            )
            continue
        for required in REQUIRED_PROVENANCE_FIELDS:
            if not entry.get(required):
                errors.append(f"{SAMPLE_DIR}/{rel}: provenance is missing '{required}'")
        if entry.get("redistributable") is not True:
            errors.append(
                f"{SAMPLE_DIR}/{rel}: provenance does not assert redistributable: true; "
                "AEGIS-236 permits committing only redistributable samples"
            )

    for rel in provenance:
        if not (directory / rel).exists():
            errors.append(f"{SAMPLE_DIR}/{PROVENANCE_FILE}: entry for missing file {rel}")

    if total > MAX_TOTAL_BYTES:
        errors.append(
            f"{SAMPLE_DIR}/ totals {total} bytes, over the {MAX_TOTAL_BYTES}-byte budget"
        )
    return errors


def check_external(root: Path) -> list[str]:
    errors: list[str] = []
    path = root / EXTERNAL_DATASETS
    if not path.exists():
        return [
            f"{EXTERNAL_DATASETS} is missing; large or licensed data must be referenced by "
            "immutable version, and there is nowhere to record that"
        ]

    document = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    datasets = document.get("datasets", []) or []
    seen: set[str] = set()
    for index, dataset in enumerate(datasets):
        label = dataset.get("dataset_id", f"entry {index}")
        for required in REQUIRED_EXTERNAL_FIELDS:
            if not dataset.get(required):
                errors.append(f"{EXTERNAL_DATASETS}: {label} is missing '{required}'")
        version = str(dataset.get("version", ""))
        if version.lower() in ("latest", "current", "head", "master", "main"):
            errors.append(
                f"{EXTERNAL_DATASETS}: {label} pins version {version!r}, which is not immutable. "
                "The data changes, the research does not, and the result becomes irreproducible."
            )
        if label in seen:
            errors.append(f"{EXTERNAL_DATASETS}: duplicate dataset_id {label!r}")
        seen.add(label)
    return errors


def run(root: Path) -> list[str]:
    return check_samples(root) + check_external(root)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    errors = run(root)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(f"Sample-data audit failed: {len(errors)} problem(s)", file=sys.stderr)
        return 2
    print("Sample-data audit passed: committed samples are small, allowed and traceable")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
