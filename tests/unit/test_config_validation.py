"""AEGIS-231 — versioned configuration, and invalid configurations that fail clearly.

The acceptance criterion is "invalid configs fail with clear errors", so these
tests assert on message content, not merely on a non-zero result. An error that
says "validation failed" without naming the offending field costs an afternoon
of guessing, and a loader that reports only the first of five problems costs
five runs.

The same corpus under ``tests/unit/fixtures/configs`` is fed to the C++ loader
by ``tests/cpp/unit/test_config.cpp``. One corpus, two loaders, one schema: if
the two implementations disagree, a test says so.
"""

from __future__ import annotations

import json
import re
from pathlib import Path

import pytest
from common.config import (
    DEFAULTS,
    SCHEMA_VERSION,
    ConfigError,
    cli_overlay,
    environment_overlay,
    load_schema,
    parse_document,
    resolve,
    validate,
)

pytestmark = pytest.mark.unit

CORPUS = Path(__file__).parent / "fixtures/configs"
VALID = sorted((CORPUS / "valid").glob("*.json"))
INVALID = sorted((CORPUS / "invalid").glob("*.json"))
EXPECTATIONS = json.loads((CORPUS / "expectations.json").read_text(encoding="utf-8"))["invalid"]


@pytest.fixture
def schema(repo_root):
    return load_schema(repo_root)


@pytest.mark.parametrize("path", VALID, ids=lambda p: p.name)
def test_valid_corpus_is_accepted(path, repo_root):
    resolved = resolve(repo_root, path=path, environ={}, defaults={})
    assert resolved.config_version == SCHEMA_VERSION
    assert resolved.experiment_id


@pytest.mark.parametrize("path", INVALID, ids=lambda p: p.name)
def test_invalid_corpus_is_rejected_with_a_message_naming_the_field(path, repo_root):
    with pytest.raises(ConfigError) as excinfo:
        resolve(repo_root, path=path, environ={}, defaults={})
    expected = EXPECTATIONS[path.name]
    assert expected in str(excinfo.value), (
        f"{path.name} was rejected, but the message does not mention {expected!r}: {excinfo.value}"
    )


def test_every_invalid_fixture_has_an_expectation():
    """A fixture with no expectation would be asserting nothing about clarity."""
    assert {p.name for p in INVALID} == set(EXPECTATIONS)


def test_missing_config_version_is_rejected(schema):
    with pytest.raises(ConfigError, match="config_version is required"):
        validate({"run": {"experiment_id": "x", "seed": 1}}, schema)


def test_future_config_version_is_rejected_not_reinterpreted(schema):
    """Interpreting an unknown version under today's rules silently changes the run."""
    with pytest.raises(ConfigError) as excinfo:
        validate({"config_version": 99, "run": {"experiment_id": "x", "seed": 1}}, schema)
    assert "not supported by this build" in str(excinfo.value)
    assert "reinterpreted" in str(excinfo.value)


def test_all_problems_are_reported_together(schema):
    """Fixing a config one error per run is a poor use of an afternoon."""
    with pytest.raises(ConfigError) as excinfo:
        validate(
            {
                "config_version": 1,
                "run": {"experiment_id": "", "seed": -5},
                "logging": {"level": "verbose", "format": "jsonl"},
            },
            schema,
        )
    message = str(excinfo.value)
    # The empty experiment_id breaks both minLength and pattern, so four.
    assert "4 problem(s)" in message
    assert "run.experiment_id" in message
    assert "run.seed" in message
    assert "logging.level" in message


def test_unknown_field_is_rejected(schema):
    """additionalProperties: false turns a typo into an error instead of a no-op."""
    with pytest.raises(ConfigError, match="loging"):
        validate(
            {
                "config_version": 1,
                "run": {"experiment_id": "x", "seed": 1},
                "logging": {"level": "info", "format": "jsonl"},
                "loging": {},
            },
            schema,
        )


# ---------------------------------------------------------------------------
# Precedence: defaults < file < environment < CLI
# ---------------------------------------------------------------------------


def test_precedence_order(repo_root, tmp_path):
    config = tmp_path / "run.json"
    config.write_text(
        json.dumps(
            {
                "config_version": 1,
                "run": {"experiment_id": "from-file", "seed": 1},
                "logging": {"level": "info", "format": "jsonl"},
            }
        ),
        encoding="utf-8",
    )

    resolved = resolve(
        repo_root,
        path=config,
        environ={"AEGIS_RUN__SEED": "2", "AEGIS_LOGGING__LEVEL": "debug"},
        cli=["run.seed=3"],
    )

    assert resolved.get("run.experiment_id") == "from-file"
    assert resolved.get("logging.level") == "debug", "environment must override the file"
    assert resolved.get("run.seed") == 3, "CLI must override the environment"
    assert resolved.sources["run.seed"] == "cli"
    assert resolved.sources["logging.level"] == "env"


def test_defaults_apply_when_nothing_overrides_them(repo_root, tmp_path):
    config = tmp_path / "run.json"
    config.write_text(
        json.dumps({"config_version": 1, "run": {"experiment_id": "defaults", "seed": 1}}),
        encoding="utf-8",
    )
    resolved = resolve(repo_root, path=config, environ={})
    assert resolved.get("logging.format") == DEFAULTS["logging"]["format"]
    assert resolved.sources["logging.format"] == "default"


def test_environment_values_are_coerced_to_schema_types():
    """Environment variables are strings; the schema wants integers and booleans."""
    overlay = environment_overlay(
        {"AEGIS_RUN__SEED": "7", "AEGIS_METRICS__ENABLED": "false", "PATH": "/usr/bin"}
    )
    assert overlay == {"run": {"seed": 7}, "metrics": {"enabled": False}}


def test_a_mistyped_environment_value_still_fails_validation(repo_root, tmp_path):
    """Coercion must not become a way to smuggle a string past an integer field."""
    config = tmp_path / "run.json"
    config.write_text(
        json.dumps(
            {
                "config_version": 1,
                "run": {"experiment_id": "typo", "seed": 1},
                "logging": {"level": "info", "format": "jsonl"},
            }
        ),
        encoding="utf-8",
    )
    with pytest.raises(ConfigError, match=re.escape("run.seed")):
        resolve(repo_root, path=config, environ={"AEGIS_RUN__SEED": "not-a-number"})


def test_cli_overlay_requires_an_assignment():
    with pytest.raises(ConfigError, match=re.escape("section.key=value")):
        cli_overlay(["run.seed"])


def test_missing_file_is_reported_by_path(repo_root, tmp_path):
    with pytest.raises(ConfigError, match="configuration file not found"):
        resolve(repo_root, path=tmp_path / "absent.json", environ={})


def test_empty_document_is_rejected():
    with pytest.raises(ConfigError, match="empty"):
        parse_document("", "run.yaml")


def test_non_mapping_document_is_rejected():
    with pytest.raises(ConfigError, match="must be a mapping"):
        parse_document("- one\n- two\n", "run.yaml")


def test_malformed_yaml_names_its_origin():
    with pytest.raises(ConfigError, match=re.escape("run.yaml")):
        parse_document("key: [unclosed\n", "run.yaml")


def test_yaml_and_json_produce_the_same_configuration(repo_root, tmp_path):
    """YAML is a superset of JSON, so one parser serves both formats."""
    as_json = tmp_path / "run.json"
    as_yaml = tmp_path / "run.yaml"
    as_json.write_text(
        json.dumps(
            {
                "config_version": 1,
                "run": {"experiment_id": "same", "seed": 5},
                "logging": {"level": "info", "format": "jsonl"},
            }
        ),
        encoding="utf-8",
    )
    as_yaml.write_text(
        "config_version: 1\nrun:\n  experiment_id: same\n  seed: 5\n"
        "logging:\n  level: info\n  format: jsonl\n",
        encoding="utf-8",
    )
    assert (
        resolve(repo_root, path=as_json, environ={}, defaults={}).canonical_json()
        == resolve(repo_root, path=as_yaml, environ={}, defaults={}).canonical_json()
    )


# ---------------------------------------------------------------------------
# The resolved configuration is what gets recorded, so it must hash stably.
# ---------------------------------------------------------------------------


def test_digest_is_stable_and_order_independent(repo_root, tmp_path):
    """The manifest records what applied, so key order must not change the hash."""
    first = tmp_path / "a.json"
    second = tmp_path / "b.json"
    first.write_text(
        json.dumps(
            {
                "config_version": 1,
                "logging": {"format": "jsonl", "level": "info"},
                "run": {"seed": 9, "experiment_id": "stable"},
            }
        ),
        encoding="utf-8",
    )
    second.write_text(
        json.dumps(
            {
                "run": {"experiment_id": "stable", "seed": 9},
                "config_version": 1,
                "logging": {"level": "info", "format": "jsonl"},
            }
        ),
        encoding="utf-8",
    )
    a = resolve(repo_root, path=first, environ={}, defaults={})
    b = resolve(repo_root, path=second, environ={}, defaults={})
    assert a.digest() == b.digest()
    assert a.digest() == a.digest()


def test_digest_changes_when_the_configuration_changes(repo_root):
    a = resolve(repo_root, path=CORPUS / "valid/minimal.json", environ={}, defaults={})
    b = resolve(repo_root, path=CORPUS / "valid/minimal.json", environ={}, cli=["run.seed=1"], defaults={})
    assert a.digest() != b.digest()


def test_schema_file_is_the_single_source_of_truth(repo_root):
    """If this file moves, the C++ loader must move with it — hence one constant."""
    from common.config import SCHEMA_PATH

    assert (repo_root / SCHEMA_PATH).exists()
    schema = load_schema(repo_root)
    assert schema["properties"]["config_version"]["enum"] == [SCHEMA_VERSION]
