#!/usr/bin/env python3
"""Record the environment a run executed in (AEGIS-009, AEGIS-053).

``docs/BENCHMARK_POLICY.md`` requires every performance claim to disclose the
CPU, memory, OS, virtualisation status, compiler and flags, build type,
sanitizer and assertion state, and whether logging was active. Collecting that
by hand at the moment a benchmark is written is how disclosure quietly becomes
approximate — the machine is remembered rather than measured, and "run on a
laptop" becomes "run on a server" in the retelling.

So the record is captured mechanically, versioned, and attached to the
experiment manifest. This tool is deliberately available from M0, long before
there is anything to benchmark: it is much easier to require the field than to
reconstruct it afterwards.

It detects and reports virtualisation rather than hiding it. AEGIS is developed
under WSL2, whose timing behaviour differs materially from bare metal, and
BENCHMARK_POLICY rule 2 requires such results to be labelled.
"""

from __future__ import annotations

import argparse
import json
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[1]
RECORD_VERSION = 1


def run(*command: str) -> str | None:
    executable = shutil.which(command[0])
    if executable is None:
        return None
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    return result.stdout.strip() if result.returncode == 0 else None


def first_line(text: str | None) -> str | None:
    return text.splitlines()[0].strip() if text else None


def cpu_facts() -> dict[str, Any]:
    facts: dict[str, Any] = {"machine": platform.machine(), "processor": platform.processor()}
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        text = cpuinfo.read_text(encoding="utf-8", errors="replace")
        model = re.search(r"^model name\s*:\s*(.+)$", text, re.MULTILINE)
        if model:
            facts["model"] = model.group(1).strip()
        facts["logical_cpus"] = len(re.findall(r"^processor\s*:", text, re.MULTILINE))
    return facts


def memory_facts() -> dict[str, Any]:
    meminfo = Path("/proc/meminfo")
    if not meminfo.exists():
        return {}
    text = meminfo.read_text(encoding="utf-8", errors="replace")
    total = re.search(r"^MemTotal:\s*(\d+) kB$", text, re.MULTILINE)
    return {"total_kb": int(total.group(1))} if total else {}


def virtualisation_facts() -> dict[str, Any]:
    """Report virtualisation rather than hide it.

    BENCHMARK_POLICY rule 2 requires WSL, VM and container results to be labelled
    as such. A figure measured under WSL2 and quoted without that label is a
    misleading figure even when the number itself is correct.
    """
    release = platform.release().lower()
    facts: dict[str, Any] = {
        "kernel": platform.release(),
        "wsl": "microsoft" in release or "wsl" in release,
        "container": Path("/.dockerenv").exists(),
    }
    detected = run("systemd-detect-virt")
    if detected:
        facts["detected_virt"] = detected
    facts["bare_metal_claimable"] = not (facts["wsl"] or facts["container"] or bool(detected))
    return facts


def toolchain_facts() -> dict[str, Any]:
    return {
        "python": {
            "version": platform.python_version(),
            "implementation": platform.python_implementation(),
            "executable": sys.executable,
        },
        "clang": first_line(run("clang++", "--version")),
        "gcc": first_line(run("g++", "--version")),
        "cmake": first_line(run("cmake", "--version")),
        "ninja": run("ninja", "--version"),
        "clang_format": first_line(run("clang-format", "--version")),
        "clang_tidy": first_line(run("clang-tidy", "--version")),
        "git": first_line(run("git", "--version")),
    }


def build_facts(root: Path, preset: str) -> dict[str, Any]:
    """Read the build's own description out of the compiled binary.

    The alternative — describing the build in the record — is exactly the
    approximation this tool exists to remove.
    """
    facts: dict[str, Any] = {"preset": preset}
    bindings = sorted((root / "build" / preset / "cpp/bindings").glob("aegis_bindings*.so"))
    if not bindings:
        facts["build_info"] = None
        facts["note"] = f"build/{preset} has no compiled bindings; build it to capture build_info"
        return facts

    script = (
        f"import sys; sys.path.insert(0, {str(bindings[0].parent)!r}); "
        "import aegis_bindings; print(aegis_bindings.build_info())"
    )
    facts["build_info"] = run(sys.executable, "-c", script)
    return facts


def repository_facts(root: Path) -> dict[str, Any]:
    commit = run("git", "-C", str(root), "rev-parse", "HEAD")
    status = run("git", "-C", str(root), "status", "--porcelain")
    return {
        "commit": commit,
        "dirty": bool(status),
        "branch": run("git", "-C", str(root), "rev-parse", "--abbrev-ref", "HEAD"),
    }


def capture(root: Path, preset: str) -> dict[str, Any]:
    return {
        "record_version": RECORD_VERSION,
        "os": {
            "system": platform.system(),
            "release": platform.release(),
            "version": platform.version(),
            "distribution": first_line(run("lsb_release", "-ds")),
        },
        "cpu": cpu_facts(),
        "memory": memory_facts(),
        "virtualisation": virtualisation_facts(),
        "toolchain": toolchain_facts(),
        "build": build_facts(root, preset),
        "repository": repository_facts(root),
        "disclosure_note": (
            "docs/BENCHMARK_POLICY.md requires this record alongside any performance figure. "
            "Where virtualisation.bare_metal_claimable is false, results must be labelled as "
            "WSL/VM/container figures."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--preset", default="release", help="CMake preset whose build to describe")
    parser.add_argument("--output", type=Path, help="write the record here instead of stdout")
    args = parser.parse_args(argv)

    record = capture(args.root.resolve(), args.preset)
    text = json.dumps(record, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(text, encoding="utf-8")
        print(f"wrote {args.output}")
    else:
        print(text, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
