"""AEGIS-004 — the dependency checker must reject the edges the spec forbids.

Two committed fixture trees do the work: ``arch_ok`` is a legal miniature of the
platform, ``arch_violation`` commits one instance of every rule. A checker that
only ever runs over a clean repository proves nothing about what it would catch.
"""

from __future__ import annotations

from pathlib import Path

import check_architecture
import pytest

pytestmark = pytest.mark.unit

FIXTURES = Path(__file__).parent / "fixtures"
OK = FIXTURES / "arch_ok"
VIOLATION = FIXTURES / "arch_violation"


def errors_for(tree: Path) -> list[str]:
    return check_architecture.run(tree, tree / "configs/architecture_rules.yaml")


def test_legal_tree_passes():
    assert errors_for(OK) == []


def test_strategy_cannot_include_the_exchange_book():
    """The single most important edge in AEGIS: participant code never reaches
    into exchange internals (docs/ARCHITECTURE.md, AEGIS-004)."""
    assert any(
        "may not depend on cpp-exchange-order-book" in e for e in errors_for(VIOLATION)
    )


def test_strategy_cannot_reach_a_gateway():
    """AEGIS-120: a strategy that holds a gateway has bypassed risk and the OMS."""
    assert any("only the OMS may include gateway/adapter headers" in e for e in errors_for(VIOLATION))


def test_parent_relative_include_is_rejected():
    assert any("parent-relative include" in e for e in errors_for(VIOLATION))


def test_python_headers_are_confined_to_the_bindings_layer():
    assert any("Python headers are confined to" in e for e in errors_for(VIOLATION))


def test_namespace_ownership_is_enforced():
    assert any("does not own" in e for e in errors_for(VIOLATION))


def test_mutable_global_in_a_deterministic_core_is_rejected():
    assert any("file-scope mutable definition" in e for e in errors_for(VIOLATION))


def test_python_import_inverting_the_dag_is_rejected():
    assert any("python-common may not depend on python-data" in e for e in errors_for(VIOLATION))


def test_cmake_link_edge_is_checked_independently_of_includes():
    """An include-only checker misses link edges entirely."""
    assert any("link edge aegis_common -> aegis_participant_strategy" in e for e in errors_for(VIOLATION))


def test_every_designed_violation_is_reported():
    """The fixture commits eight distinct violations; none may go unreported."""
    reported = errors_for(VIOLATION)
    signatures = (
        "may not depend on cpp-exchange-order-book",
        "gateway/adapter headers",
        "parent-relative include",
        "Python headers are confined",
        "does not own",
        "file-scope mutable definition",
        "python-common may not depend on python-data",
        "link edge",
    )
    missing = [s for s in signatures if not any(s in e for e in reported)]
    assert missing == [], f"unreported violations: {missing}"


def test_unclaimed_file_fails_total_coverage(tmp_path):
    """A package added in a later milestone must fail, not go unchecked."""
    tree = tmp_path / "tree"
    (tree / "cpp/common").mkdir(parents=True)
    (tree / "cpp/newthing").mkdir(parents=True)
    (tree / "cpp/common/clock.hpp").write_text("#pragma once\n", encoding="utf-8")
    (tree / "cpp/newthing/thing.hpp").write_text("#pragma once\n", encoding="utf-8")
    rules = tree / "rules.yaml"
    rules.write_text(
        "rules_version: 1\n"
        "layers:\n"
        "  - name: cpp-common\n"
        "    paths: [cpp/common]\n"
        "    language: cpp\n"
        "    may_depend_on: []\n"
        "covered_roots: [cpp]\n"
        "banned: {}\n",
        encoding="utf-8",
    )
    errors = check_architecture.run(tree, rules)
    assert any("no layer claims this file" in e for e in errors)


def test_layer_must_be_empty_before_its_declaring_milestone(tmp_path):
    tree = tmp_path / "tree"
    (tree / "cpp/exchange/order_book").mkdir(parents=True)
    (tree / "cpp/exchange/order_book/book.hpp").write_text("#pragma once\n", encoding="utf-8")
    rules = tree / "rules.yaml"
    rules.write_text(
        "rules_version: 1\n"
        "layers:\n"
        "  - name: cpp-exchange-order-book\n"
        "    paths: [cpp/exchange/order_book]\n"
        "    language: cpp\n"
        "    may_depend_on: []\n"
        "    expect_sources_from_milestone: M1\n"
        "covered_roots: [cpp]\n"
        "banned: {}\n",
        encoding="utf-8",
    )
    errors = check_architecture.run(tree, rules, milestone="M0")
    assert any("must be empty until M1" in e for e in errors)
    assert check_architecture.run(tree, rules, milestone="M1") == []


def test_layer_must_not_stay_empty_once_its_milestone_arrives(tmp_path):
    """Otherwise every rule about that layer keeps passing vacuously."""
    tree = tmp_path / "tree"
    (tree / "cpp/exchange/order_book").mkdir(parents=True)
    rules = tree / "rules.yaml"
    rules.write_text(
        "rules_version: 1\n"
        "layers:\n"
        "  - name: cpp-exchange-order-book\n"
        "    paths: [cpp/exchange/order_book]\n"
        "    language: cpp\n"
        "    may_depend_on: []\n"
        "    expect_sources_from_milestone: M1\n"
        "covered_roots: [cpp]\n"
        "banned: {}\n",
        encoding="utf-8",
    )
    errors = check_architecture.run(tree, rules, milestone="M1")
    assert any("would pass vacuously" in e for e in errors)


def test_rules_file_rejects_an_edge_to_an_unknown_layer(tmp_path):
    rules = tmp_path / "rules.yaml"
    rules.write_text(
        "rules_version: 1\n"
        "layers:\n"
        "  - name: cpp-common\n"
        "    paths: [cpp/common]\n"
        "    language: cpp\n"
        "    may_depend_on: [cpp-nonexistent]\n"
        "covered_roots: [cpp]\n"
        "banned: {}\n",
        encoding="utf-8",
    )
    with pytest.raises(SystemExit):
        check_architecture.load_rules(rules)


def test_real_rules_file_is_well_formed(repo_root):
    """The production rules must at minimum load and name only known layers."""
    rules = check_architecture.load_rules(repo_root / "configs/architecture_rules.yaml")
    names = {layer.name for layer in rules.layers}
    assert "cpp-exchange-order-book" in names
    assert "cpp-participant-strategy" in names
    strategy = next(layer for layer in rules.layers if layer.name == "cpp-participant-strategy")
    assert not strategy.allows_gateway_adapters
    assert "cpp-participant-oms" not in strategy.may_depend_on
    assert not any(name.startswith("cpp-exchange") for name in strategy.may_depend_on)
