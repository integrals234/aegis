"""Versioned, validated configuration (AEGIS-231).

The acceptance criterion is "invalid configs fail with clear errors", so the
failure path is the feature here, not an afterthought. Three properties do the
work:

**One schema.** ``configs/schemas/config.v1.json`` is the only statement of what
a valid configuration is. This loader and the C++ loader both read it; neither
carries a second copy of the rules that could drift out of step.

**Mandatory, enumerated ``config_version``.** A document written against a
future schema is rejected rather than interpreted under today's rules. Silently
ignoring an unknown version is how a run ends up configured differently from
what its config file says.

**Explicit precedence.** defaults < file < environment < CLI, resolved once and
then frozen. The resolved mapping is hashed into the experiment manifest, so a
research result records the configuration that actually applied rather than the
file somebody believes was used (AEGIS-097, AEGIS-212).
"""

from __future__ import annotations

import hashlib
import json
from collections.abc import Iterable, Mapping
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import jsonschema
import yaml

SCHEMA_VERSION = 1
SCHEMA_PATH = "configs/schemas/config.v1.json"
ENV_PREFIX = "AEGIS_"

DEFAULTS: dict[str, Any] = {
    "config_version": SCHEMA_VERSION,
    "logging": {"level": "info", "format": "jsonl"},
    "metrics": {"enabled": True, "snapshot_interval_ms": 1000},
}


class ConfigError(ValueError):
    """A configuration was rejected. The message names the offending field."""


@dataclass(frozen=True)
class ResolvedConfig:
    """A validated configuration and the provenance of every override.

    ``sources`` records which layer supplied each overridden key. Without it, a
    run whose behaviour came from an environment variable looks identical to one
    configured entirely by its file, and the difference is invisible in the
    artifact months later.
    """

    values: Mapping[str, Any]
    sources: Mapping[str, str] = field(default_factory=dict)

    @property
    def config_version(self) -> int:
        return int(self.values["config_version"])

    @property
    def experiment_id(self) -> str:
        return str(self.values["run"]["experiment_id"])

    def get(self, dotted: str, default: Any = None) -> Any:
        node: Any = self.values
        for part in dotted.split("."):
            if not isinstance(node, Mapping) or part not in node:
                return default
            node = node[part]
        return node

    def canonical_json(self) -> str:
        """Deterministic serialization: sorted keys, no insignificant whitespace."""
        return json.dumps(self.values, sort_keys=True, separators=(",", ":"))

    def digest(self) -> str:
        """SHA-256 of the canonical form, for the experiment manifest."""
        return hashlib.sha256(self.canonical_json().encode("utf-8")).hexdigest()


def load_schema(root: Path) -> dict[str, Any]:
    path = root / SCHEMA_PATH
    if not path.exists():
        raise ConfigError(f"configuration schema not found at {SCHEMA_PATH}")
    schema: dict[str, Any] = json.loads(path.read_text(encoding="utf-8"))
    return schema


def _deep_merge(base: Mapping[str, Any], overlay: Mapping[str, Any], layer: str,
                sources: dict[str, str], prefix: str = "") -> dict[str, Any]:
    merged = dict(base)
    for key, value in overlay.items():
        dotted = f"{prefix}{key}"
        if isinstance(value, Mapping):
            # Recurse even when the base has nothing here, so provenance is
            # recorded per leaf. Attributing a whole section to one layer would
            # hide which specific value an environment variable actually changed.
            existing = merged.get(key)
            base_section = existing if isinstance(existing, Mapping) else {}
            merged[key] = _deep_merge(base_section, value, layer, sources, f"{dotted}.")
        else:
            merged[key] = value
            sources[dotted] = layer
    return merged


def parse_document(text: str, origin: str) -> dict[str, Any]:
    """Parse YAML or JSON. YAML is a superset of JSON, so one parser covers both."""
    try:
        document = yaml.safe_load(text)
    except yaml.YAMLError as exc:
        raise ConfigError(f"{origin}: not valid YAML or JSON: {exc}") from exc
    if document is None:
        raise ConfigError(f"{origin}: configuration document is empty")
    if not isinstance(document, dict):
        raise ConfigError(
            f"{origin}: configuration must be a mapping, got {type(document).__name__}"
        )
    return document


def _coerce_scalar(raw: str) -> Any:
    """Interpret an environment value the way the schema expects.

    Environment variables are strings; the schema demands integers and booleans.
    Coercing here — rather than loosening the schema to accept strings — keeps a
    typo like ``AEGIS_RUN__SEED=abc`` a validation failure instead of a silently
    accepted string seed.
    """
    lowered = raw.strip().lower()
    if lowered in ("true", "false"):
        return lowered == "true"
    if lowered in ("null", "none", "~"):
        return None
    try:
        return int(raw)
    except ValueError:
        pass
    try:
        return float(raw)
    except ValueError:
        return raw


def environment_overlay(environ: Mapping[str, str]) -> dict[str, Any]:
    """Build an overlay from ``AEGIS_SECTION__KEY`` variables.

    Double underscore separates nesting levels, so ``AEGIS_RUN__SEED=7`` sets
    ``run.seed``. Single underscores stay inside key names, which is why the
    separator has to be something a key cannot contain.
    """
    overlay: dict[str, Any] = {}
    for name, raw in sorted(environ.items()):
        if not name.startswith(ENV_PREFIX):
            continue
        path = name[len(ENV_PREFIX) :].lower().split("__")
        if not path or not path[0]:
            continue
        node = overlay
        for part in path[:-1]:
            node = node.setdefault(part, {})
            if not isinstance(node, dict):
                raise ConfigError(f"environment override {name} conflicts with an earlier override")
        node[path[-1]] = _coerce_scalar(raw)
    return overlay


def cli_overlay(assignments: Iterable[str]) -> dict[str, Any]:
    """Build an overlay from ``--set section.key=value`` style assignments."""
    overlay: dict[str, Any] = {}
    for assignment in assignments:
        if "=" not in assignment:
            raise ConfigError(
                f"CLI override {assignment!r} is not of the form 'section.key=value'"
            )
        dotted, _, raw = assignment.partition("=")
        parts = [p for p in dotted.strip().split(".") if p]
        if not parts:
            raise ConfigError(f"CLI override {assignment!r} has an empty key")
        node = overlay
        for part in parts[:-1]:
            node = node.setdefault(part, {})
            if not isinstance(node, dict):
                raise ConfigError(f"CLI override {assignment!r} conflicts with an earlier override")
        node[parts[-1]] = _coerce_scalar(raw)
    return overlay


def _describe(error: jsonschema.ValidationError) -> str:
    location = ".".join(str(part) for part in error.absolute_path) or "(root)"
    return f"{location}: {error.message}"


def validate(values: Mapping[str, Any], schema: Mapping[str, Any]) -> None:
    """Validate against the schema, reporting every problem rather than the first.

    A loader that stops at the first error turns fixing a configuration into a
    sequence of guesses, so the message lists all of them, each with its path.
    """
    declared = values.get("config_version")
    if declared is None:
        raise ConfigError(
            "config_version is required. AEGIS refuses to guess which schema a "
            f"configuration targets; add 'config_version: {SCHEMA_VERSION}'."
        )
    if declared != SCHEMA_VERSION:
        raise ConfigError(
            f"config_version {declared!r} is not supported by this build "
            f"(this build understands version {SCHEMA_VERSION}). "
            "A configuration written for a different schema is rejected rather than "
            "reinterpreted, because reinterpretation silently changes what the run does."
        )

    validator = jsonschema.Draft202012Validator(schema)
    problems = sorted(validator.iter_errors(values), key=lambda e: list(e.absolute_path))
    if problems:
        detail = "\n".join(f"  - {_describe(problem)}" for problem in problems)
        raise ConfigError(f"configuration is invalid ({len(problems)} problem(s)):\n{detail}")


def resolve(
    root: Path,
    path: Path | None = None,
    environ: Mapping[str, str] | None = None,
    cli: Iterable[str] = (),
    defaults: Mapping[str, Any] | None = None,
) -> ResolvedConfig:
    """Resolve configuration in precedence order and validate the result.

    Precedence is defaults < file < environment < CLI, and validation happens
    once at the end. Validating each layer separately would reject a partial
    overlay that is perfectly legal on its own.
    """
    sources: dict[str, str] = {}
    values = _deep_merge({}, defaults if defaults is not None else DEFAULTS, "default", sources)

    if path is not None:
        if not path.exists():
            raise ConfigError(f"configuration file not found: {path}")
        document = parse_document(path.read_text(encoding="utf-8"), str(path))
        values = _deep_merge(values, document, f"file:{path.name}", sources)

    if environ is not None:
        values = _deep_merge(values, environment_overlay(environ), "env", sources)

    cli_values = cli_overlay(cli)
    if cli_values:
        values = _deep_merge(values, cli_values, "cli", sources)

    validate(values, load_schema(root))
    return ResolvedConfig(values=values, sources=sources)
