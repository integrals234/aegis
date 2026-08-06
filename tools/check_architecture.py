#!/usr/bin/env python3
"""Enforce the AEGIS layer DAG structurally (AEGIS-004).

``docs/ARCHITECTURE.md`` states that exchange-side and participant-side code are
separate systems joined only by versioned messages, and that a strategy can
never reach an exchange or a broker adapter. This checker turns that statement
into edges that can be rejected, by reading four things that actually determine
what depends on what:

1. C++ ``#include`` edges and Python ``import`` edges;
2. the namespaces a translation unit opens;
3. CMake ``target_link_libraries`` edges, which an include-only checker misses
   entirely;
4. banned constructs — parent-relative includes, Python headers outside the
   bindings layer, gateway headers outside the OMS, and file-scope mutable
   globals in the deterministic cores.

It runs enforcing from the first commit. An advisory checker is how the first
violation lands unnoticed.
"""

from __future__ import annotations

import argparse
import ast
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_RULES = "configs/architecture_rules.yaml"
BUILD_STATE = "docs/BUILD_STATE.md"

CPP_SUFFIXES = {".h", ".hpp", ".hh", ".cpp", ".cc", ".cxx", ".ipp"}
PY_SUFFIXES = {".py"}

INCLUDE_QUOTED = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)
INCLUDE_ANGLED = re.compile(r"^\s*#\s*include\s+<([^>]+)>", re.MULTILINE)
NAMESPACE_OPEN = re.compile(r"^\s*namespace\s+([A-Za-z_][\w:\s]*?)\s*\{", re.MULTILINE)
TARGET_LINK = re.compile(
    r"target_link_libraries\s*\(\s*([A-Za-z0-9_.:-]+)(.*?)\)", re.DOTALL | re.IGNORECASE
)
ADD_LIBRARY = re.compile(r"add_(?:library|executable)\s*\(\s*([A-Za-z0-9_.:-]+)", re.IGNORECASE)

PYTHON_HEADERS = ("pybind11", "Python.h")

# A file-scope definition that is neither const/constexpr nor a function.
GLOBAL_DECL = re.compile(
    r"""^(?!\s)                                   # column 0: file scope
        (?!.*\b(?:const|constexpr|consteval|using|namespace|class|struct|enum|
                  template|typedef|extern\s+"C"|static_assert|inline\s+constexpr)\b)
        (?P<decl>[A-Za-z_][\w:<>,\s\*&\[\]]*?\s[\*&]?\s*[A-Za-z_]\w*)
        \s*(?:=[^;()]*)?;                          # initialiser without a call
    """,
    re.VERBOSE,
)


@dataclass
class Layer:
    name: str
    paths: list[str]
    language: str
    may_depend_on: set[str]
    namespaces: list[str] = field(default_factory=list)
    expect_sources_from_milestone: str | None = None
    allows_python_headers: bool = False
    allows_gateway_adapters: bool = False
    cmake_target: str | None = None

    @property
    def target(self) -> str:
        """CMake target name for this layer.

        Convention: ``aegis_`` plus the layer name with its language prefix
        dropped, so ``cpp-participant-strategy`` builds ``aegis_participant_strategy``.
        A layer may override it with an explicit ``cmake_target``.
        """
        if self.cmake_target:
            return self.cmake_target
        stem = re.sub(r"^(?:cpp|python)-", "", self.name)
        return "aegis_" + stem.replace("-", "_")


@dataclass
class Rules:
    layers: list[Layer]
    covered_roots: list[str]
    banned: dict[str, Any]


def load_rules(path: Path) -> Rules:
    doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    layers = [
        Layer(
            name=entry["name"],
            paths=list(entry["paths"]),
            language=entry.get("language", "cpp"),
            may_depend_on=set(entry.get("may_depend_on", [])),
            namespaces=list(entry.get("namespaces", [])),
            expect_sources_from_milestone=entry.get("expect_sources_from_milestone"),
            allows_python_headers=bool(entry.get("allows_python_headers", False)),
            allows_gateway_adapters=bool(entry.get("allows_gateway_adapters", False)),
            cmake_target=entry.get("cmake_target"),
        )
        for entry in doc["layers"]
    ]
    names = [layer.name for layer in layers]
    if len(names) != len(set(names)):
        raise SystemExit("ERROR: duplicate layer names in the rules file")
    for layer in layers:
        unknown = layer.may_depend_on - set(names)
        if unknown:
            raise SystemExit(f"ERROR: layer {layer.name} may_depend_on unknown layers: {sorted(unknown)}")
    return Rules(layers=layers, covered_roots=list(doc.get("covered_roots", [])), banned=doc.get("banned", {}))


def active_milestone(root: Path) -> str | None:
    state = root / BUILD_STATE
    if not state.exists():
        return None
    for line in state.read_text(encoding="utf-8").splitlines():
        if line.startswith("- Active milestone:"):
            return line.split(":", 1)[1].strip()
    return None


def milestone_index(value: str | None) -> int:
    if not value or not value.startswith("M") or not value[1:].isdigit():
        return -1
    return int(value[1:])


def layer_for(rel: str, rules: Rules) -> list[Layer]:
    """Return every layer claiming ``rel``; more than one is a rules defect."""
    matches = []
    for layer in rules.layers:
        for base in layer.paths:
            if rel == base or rel.startswith(base + "/"):
                matches.append(layer)
                break
    return matches


def source_files(root: Path, rules: Rules) -> list[Path]:
    files: list[Path] = []
    for covered in rules.covered_roots:
        base = root / covered
        if not base.exists():
            continue
        for path in sorted(base.rglob("*")):
            if path.is_file() and path.suffix in (CPP_SUFFIXES | PY_SUFFIXES):
                files.append(path)
    return files


def _cpp_include_layer(include: str, rules: Rules) -> tuple[list[Layer], bool]:
    """Resolve a quoted include to its layer. Returns (layers, is_internal)."""
    if include.split("/")[0] not in {"cpp", "python"}:
        return [], False
    return layer_for(include, rules), True


def check_cpp_file(path: Path, rel: str, layer: Layer, rules: Rules) -> list[str]:
    errors: list[str] = []
    text = path.read_text(encoding="utf-8", errors="replace")
    banned = rules.banned

    for include in INCLUDE_QUOTED.findall(text):
        if banned.get("parent_relative_includes", True) and ".." in Path(include).parts:
            errors.append(f"{rel}: parent-relative include routes around the layer DAG: \"{include}\"")
            continue
        target_layers, internal = _cpp_include_layer(include, rules)
        if not internal:
            errors.append(
                f"{rel}: quoted include \"{include}\" is not repository-root-relative; "
                "see include_convention in the rules file"
            )
            continue
        if not target_layers:
            errors.append(f"{rel}: include \"{include}\" resolves to no declared layer")
            continue
        target = target_layers[0]
        if target.name != layer.name and target.name not in layer.may_depend_on:
            errors.append(
                f"{rel}: layer {layer.name} may not depend on {target.name} (include \"{include}\")"
            )

    allowed_python_layers = set(banned.get("python_headers_only_in", []))
    for include in INCLUDE_QUOTED.findall(text) + INCLUDE_ANGLED.findall(text):
        if any(marker in include for marker in PYTHON_HEADERS) and (
            layer.name not in allowed_python_layers and not layer.allows_python_headers
        ):
            errors.append(
                f"{rel}: Python headers are confined to {sorted(allowed_python_layers)} "
                f"(include \"{include}\")"
            )
        if (
            any(marker in include for marker in banned.get("gateway_header_markers", []))
            and not layer.allows_gateway_adapters
        ):
            errors.append(
                f"{rel}: only the OMS may include gateway/adapter headers (include \"{include}\")"
            )

    if layer.namespaces:
        for raw in NAMESPACE_OPEN.findall(text):
            opened = re.sub(r"\s+", "", raw)
            if not opened.startswith("aegis"):
                continue
            owned = any(
                opened == ns or ns.startswith(opened + "::") or opened.startswith(ns)
                for ns in layer.namespaces
            )
            if not owned:
                errors.append(
                    f"{rel}: opens namespace {opened}, which layer {layer.name} does not own "
                    f"(owns {layer.namespaces})"
                )

    if layer.name in banned.get("mutable_globals_forbidden_in", []):
        errors.extend(check_mutable_globals(rel, text))
    return errors


def check_mutable_globals(rel: str, text: str) -> list[str]:
    """Flag file-scope mutable definitions in the deterministic cores.

    Heuristic by necessity — this is not a C++ parser. It reports declarations
    at brace depth zero that are neither const/constexpr nor function-shaped;
    the conservative direction is to over-report, which review can dismiss,
    rather than to miss hidden global state that would surface as replay
    nondeterminism much later.
    """
    errors = []
    depth = 0
    for lineno, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if depth == 0 and stripped and not stripped.startswith(("#", "//", "*", "/*")):
            match = GLOBAL_DECL.match(line)
            if match and "(" not in match.group("decl"):
                errors.append(
                    f"{rel}:{lineno}: file-scope mutable definition '{stripped}' — "
                    "state must be owned by an injected instance, not a global"
                )
        depth += line.count("{") - line.count("}")
    return errors


def check_python_file(path: Path, rel: str, layer: Layer, rules: Rules) -> list[str]:
    errors: list[str] = []
    try:
        tree = ast.parse(path.read_text(encoding="utf-8"))
    except SyntaxError as exc:
        return [f"{rel}: cannot parse: {exc}"]

    def resolve(module: str) -> list[Layer]:
        parts = module.split(".")
        for root_dir in ("python", ""):
            candidate = "/".join(([root_dir] if root_dir else []) + parts)
            matches = layer_for(candidate, rules)
            if matches:
                return matches
        return []

    for node in ast.walk(tree):
        modules: list[str] = []
        if isinstance(node, ast.Import):
            modules = [alias.name for alias in node.names]
        elif isinstance(node, ast.ImportFrom):
            if node.level:
                continue  # relative import inside the same package
            if node.module:
                modules = [node.module]
        for module in modules:
            targets = resolve(module)
            if not targets:
                continue  # stdlib or third-party
            target = targets[0]
            if target.name != layer.name and target.name not in layer.may_depend_on:
                errors.append(
                    f"{rel}: layer {layer.name} may not depend on {target.name} (import {module})"
                )
    return errors


def check_cmake(root: Path, rules: Rules) -> list[str]:
    """Check target_link_libraries edges against the same DAG.

    Targets are named after their layer (``aegis_common`` for ``cpp-common``), so
    a link edge that no include edge would justify is still caught.
    """
    errors: list[str] = []
    # Only C++ layers build CMake targets; including Python layers here would
    # collide on names like aegis_common and misattribute every link edge.
    by_target = {layer.target: layer for layer in rules.layers if layer.language == "cpp"}
    cpp_layers = [layer for layer in rules.layers if layer.language == "cpp"]
    duplicates = [t for t in by_target if sum(1 for x in cpp_layers if x.target == t) > 1]
    if duplicates:
        errors.append(f"rules defect: CMake target names collide: {sorted(set(duplicates))}")
    for cmake in sorted(root.rglob("CMakeLists.txt")):
        rel_parts = cmake.relative_to(root).parts
        if {".venv", "build"} & set(rel_parts) or "fixtures" in rel_parts:
            continue  # fixture trees are checked explicitly by their own tests
        text = cmake.read_text(encoding="utf-8")
        rel = cmake.relative_to(root).as_posix()
        for target, body in TARGET_LINK.findall(text):
            layer = by_target.get(target)
            if layer is None:
                continue  # test binaries and third-party targets are not layers
            for token in re.split(r"\s+", body.strip()):
                token = token.strip()
                if not token or token.upper() in {"PUBLIC", "PRIVATE", "INTERFACE"}:
                    continue
                linked = by_target.get(token)
                if linked is None:
                    continue
                if linked.name != layer.name and linked.name not in layer.may_depend_on:
                    errors.append(
                        f"{rel}: link edge {target} -> {token} violates the layer DAG "
                        f"({layer.name} may not depend on {linked.name})"
                    )
    return errors


def check_layer_population(root: Path, rules: Rules, active: str | None) -> list[str]:
    """Emptiness is a checked fact, in both directions."""
    errors: list[str] = []
    active_index = milestone_index(active)
    for layer in rules.layers:
        if not layer.expect_sources_from_milestone:
            continue
        files = [
            path
            for base in layer.paths
            for path in (root / base).rglob("*")
            if path.is_file() and path.suffix in (CPP_SUFFIXES | PY_SUFFIXES)
        ]
        expected_index = milestone_index(layer.expect_sources_from_milestone)
        if active_index < 0:
            continue
        if active_index < expected_index and files:
            names = ", ".join(sorted(p.relative_to(root).as_posix() for p in files)[:5])
            errors.append(
                f"layer {layer.name} must be empty until {layer.expect_sources_from_milestone} "
                f"(active milestone {active}), but contains: {names}"
            )
        if active_index >= expected_index and not files:
            errors.append(
                f"layer {layer.name} declares sources from {layer.expect_sources_from_milestone} "
                f"(active milestone {active}) but is empty — the rule would pass vacuously"
            )
    return errors


def run(root: Path, rules_path: Path, milestone: str | None = None) -> list[str]:
    rules = load_rules(rules_path)
    errors: list[str] = []
    active = milestone or active_milestone(root)

    for path in source_files(root, rules):
        rel = path.relative_to(root).as_posix()
        matches = layer_for(rel, rules)
        if not matches:
            errors.append(f"{rel}: no layer claims this file (total coverage is required)")
            continue
        if len(matches) > 1:
            errors.append(f"{rel}: claimed by multiple layers: {[m.name for m in matches]}")
            continue
        layer = matches[0]
        if path.suffix in CPP_SUFFIXES:
            errors.extend(check_cpp_file(path, rel, layer, rules))
        else:
            errors.extend(check_python_file(path, rel, layer, rules))

    errors.extend(check_cmake(root, rules))
    errors.extend(check_layer_population(root, rules, active))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--rules", type=Path)
    parser.add_argument("--milestone", help="override the active milestone from docs/BUILD_STATE.md")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    rules_path = args.rules or root / DEFAULT_RULES
    errors = run(root, rules_path, args.milestone)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        print(f"Architecture check failed: {len(errors)} violation(s)", file=sys.stderr)
        return 2
    print("Architecture check passed: layer DAG, namespaces, link edges and banned constructs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
