"""AEGIS-236 — the sample-data policy must fail when it should.

Both halves of the requirement are tested: committed samples must be small,
allowed and traceable, and external datasets must be referenced by an immutable
version. The second is the one that quietly rots — a reference to "the latest
vendor file" looks fine in review and makes the research irreproducible the next
time the vendor publishes.
"""

from __future__ import annotations

import check_pack_manifest
import check_sample_data
import pytest
import yaml

pytestmark = pytest.mark.unit

GOOD_PROVENANCE = {
    "provenance_version": 1,
    "samples": {
        "sample.csv": {
            "source": "Generated in this repository",
            "licence": "Same as this repository",
            "redistributable": True,
            "description": "Synthetic rows for codec tests",
            "collected_on": "2026-08-06",
        }
    },
}

GOOD_EXTERNAL = {
    "registry_version": 1,
    "datasets": [
        {
            "dataset_id": "cme-es-futures-1min",
            "version": "2026-01-15",
            "licence": "Vendor licence, redistribution not permitted",
            "access": "Vendor portal, entitlement required",
            "checksum": "sha256:" + "0" * 64,
        }
    ],
}


@pytest.fixture
def tree(tmp_path):
    root = tmp_path / "repo"
    (root / "data_samples").mkdir(parents=True)
    (root / "configs").mkdir(parents=True)
    (root / "data_samples/sample.csv").write_text("a,b\n1,2\n", encoding="utf-8")
    (root / "data_samples/PROVENANCE.yaml").write_text(yaml.safe_dump(GOOD_PROVENANCE), encoding="utf-8")
    (root / "configs/external_datasets.yaml").write_text(yaml.safe_dump(GOOD_EXTERNAL), encoding="utf-8")
    return root


def write_provenance(root, document):
    (root / "data_samples/PROVENANCE.yaml").write_text(yaml.safe_dump(document), encoding="utf-8")


def write_external(root, document):
    (root / "configs/external_datasets.yaml").write_text(yaml.safe_dump(document), encoding="utf-8")


def test_a_compliant_tree_passes(tree):
    assert check_sample_data.run(tree) == []


def test_a_sample_without_provenance_is_rejected(tree):
    (tree / "data_samples/orphan.csv").write_text("x\n1\n", encoding="utf-8")
    errors = check_sample_data.run(tree)
    assert any("no provenance entry" in e for e in errors)


def test_a_sample_not_asserted_redistributable_is_rejected(tree):
    document = {"provenance_version": 1, "samples": {"sample.csv": dict(GOOD_PROVENANCE["samples"]["sample.csv"])}}
    document["samples"]["sample.csv"]["redistributable"] = False
    write_provenance(tree, document)
    errors = check_sample_data.run(tree)
    assert any("redistributable: true" in e for e in errors)


@pytest.mark.parametrize("field", ["source", "licence", "description", "collected_on"])
def test_each_provenance_field_is_required(tree, field):
    document = {"provenance_version": 1, "samples": {"sample.csv": dict(GOOD_PROVENANCE["samples"]["sample.csv"])}}
    document["samples"]["sample.csv"].pop(field)
    write_provenance(tree, document)
    assert any(field in e for e in check_sample_data.run(tree))


def test_an_oversized_sample_is_rejected(tree):
    (tree / "data_samples/big.csv").write_bytes(b"x" * (check_sample_data.MAX_SAMPLE_BYTES + 1))
    document = dict(GOOD_PROVENANCE)
    document["samples"] = dict(GOOD_PROVENANCE["samples"])
    document["samples"]["big.csv"] = dict(GOOD_PROVENANCE["samples"]["sample.csv"])
    write_provenance(tree, document)
    errors = check_sample_data.run(tree)
    assert any("exceeds the" in e and "per-file" in e for e in errors)


def test_a_disallowed_extension_is_rejected(tree):
    (tree / "data_samples/binary.bin").write_bytes(b"\x00\x01")
    errors = check_sample_data.run(tree)
    assert any("not in the sample allowlist" in e for e in errors)


def test_a_provenance_entry_for_a_missing_file_is_rejected(tree):
    document = dict(GOOD_PROVENANCE)
    document["samples"] = dict(GOOD_PROVENANCE["samples"])
    document["samples"]["gone.csv"] = dict(GOOD_PROVENANCE["samples"]["sample.csv"])
    write_provenance(tree, document)
    assert any("missing file gone.csv" in e for e in check_sample_data.run(tree))


@pytest.mark.parametrize("version", ["latest", "current", "HEAD", "main"])
def test_a_mutable_external_version_is_rejected(tree, version):
    """The data changes, the research does not, and the result stops reproducing."""
    document = {"registry_version": 1, "datasets": [dict(GOOD_EXTERNAL["datasets"][0])]}
    document["datasets"][0]["version"] = version
    write_external(tree, document)
    errors = check_sample_data.run(tree)
    assert any("not immutable" in e for e in errors)


@pytest.mark.parametrize("field", ["dataset_id", "version", "licence", "access", "checksum"])
def test_each_external_field_is_required(tree, field):
    document = {"registry_version": 1, "datasets": [dict(GOOD_EXTERNAL["datasets"][0])]}
    document["datasets"][0].pop(field)
    write_external(tree, document)
    assert any(field in e for e in check_sample_data.run(tree))


def test_a_duplicate_dataset_id_is_rejected(tree):
    entry = dict(GOOD_EXTERNAL["datasets"][0])
    write_external(tree, {"registry_version": 1, "datasets": [entry, dict(entry)]})
    assert any("duplicate dataset_id" in e for e in check_sample_data.run(tree))


def test_a_missing_external_registry_is_rejected(tree):
    (tree / "configs/external_datasets.yaml").unlink()
    assert any("external_datasets.yaml is missing" in e for e in check_sample_data.run(tree))


def test_an_empty_external_registry_is_permitted(tree):
    """M0 has run no research, so pre-registering a vendor feed would record an
    intention rather than a fact."""
    write_external(tree, {"registry_version": 1, "datasets": []})
    assert check_sample_data.run(tree) == []


def test_the_live_repository_complies(repo_root):
    assert check_sample_data.run(repo_root) == []


# ---------------------------------------------------------------------------
# Pack manifest
# ---------------------------------------------------------------------------


def test_pack_manifest_reports_the_files_m0_changed(repo_root):
    """Nothing validated PACK_MANIFEST.json before this tool existed."""
    errors, notices = check_pack_manifest.run(repo_root, check_pack_manifest.DEFAULT_ALLOW_MISSING)
    assert errors == [], errors
    changed = {n.split(": ")[-1] for n in notices if "changed since" in n}
    assert "tools/audit_requirements.py" in changed
    assert ".gitignore" in changed


def test_pack_manifest_flags_a_missing_file_outside_the_allowance(repo_root, tmp_path):
    """Removing a scaffold file must not pass silently."""
    import json
    import shutil

    scratch = tmp_path / "copy"
    scratch.mkdir()
    shutil.copy(repo_root / "PACK_MANIFEST.json", scratch / "PACK_MANIFEST.json")
    entries = json.loads((scratch / "PACK_MANIFEST.json").read_text(encoding="utf-8"))
    kept = [e for e in entries if not e["path"].endswith(":Zone.Identifier")][:1]
    (scratch / "PACK_MANIFEST.json").write_text(json.dumps(kept), encoding="utf-8")

    errors, _ = check_pack_manifest.run(scratch, ())
    assert any("pack file missing" in e for e in errors)
