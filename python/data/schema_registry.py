"""Versioned schema registry and data interchange (AEGIS-230).

AEGIS-230 asks for Parquet/Arrow and DuckDB "where suitable, with versioned
schemas". M0 delivers the versioning half and the JSON/CSV codecs; the columnar
half is deferred to M2, which is the first milestone with columnar data to
interchange. Adding a Parquet round trip now would mean inventing a futures
schema — that is AEGIS-026, an M2 requirement — in order to have something to
write.

What is delivered is the part every later format depends on:

* every record carries ``schema_version``, and a reader **rejects** a version it
  does not know rather than interpreting it under today's field meanings;
* a registry maps (name, version) to a schema, so two versions can coexist while
  a migration is in progress;
* a documented compatibility policy, so "is this change breaking?" has an
  answer that does not depend on who is asked.

The failure being prevented is specific: a dataset written by a newer producer
being silently read under older field semantics, which yields research results
that are wrong in a way no test detects.
"""

from __future__ import annotations

import csv
import io
import json
from collections.abc import Iterable, Mapping, Sequence
from dataclasses import dataclass
from enum import StrEnum
from pathlib import Path
from typing import Any

import jsonschema

VERSION_FIELD = "schema_version"


class Compatibility(StrEnum):
    """How a schema version relates to the one before it.

    Recorded per version rather than inferred, because the inference is exactly
    the judgement call that gets made optimistically under deadline.
    """

    #: Readers of the previous version can read this one. Fields added only.
    BACKWARD = "backward"
    #: Readers of this version can read the previous one. Fields removed only.
    FORWARD = "forward"
    #: Neither. Existing data must be migrated or re-read with the old schema.
    BREAKING = "breaking"


class SchemaError(ValueError):
    """A record or schema was rejected. The message names what and why."""


class UnknownSchemaVersion(SchemaError):
    def __init__(self, name: str, found: object, known: Sequence[int]) -> None:
        super().__init__(
            f"record declares {name} schema_version {found!r}, which this build does not know "
            f"(known versions: {sorted(known)}). Reading it under a different version's field "
            "meanings would produce data that looks valid and is wrong."
        )
        self.found = found
        self.known = tuple(known)


@dataclass(frozen=True)
class RegisteredSchema:
    name: str
    version: int
    schema: Mapping[str, Any]
    compatibility: Compatibility
    description: str = ""


class SchemaRegistry:
    """Maps (name, version) to a schema and validates records against it."""

    def __init__(self) -> None:
        self._schemas: dict[tuple[str, int], RegisteredSchema] = {}

    def register(
        self,
        name: str,
        version: int,
        schema: Mapping[str, Any],
        compatibility: Compatibility = Compatibility.BREAKING,
        description: str = "",
    ) -> RegisteredSchema:
        if version < 1:
            raise SchemaError(f"{name}: schema version must be >= 1, got {version}")
        key = (name, version)
        if key in self._schemas:
            # Re-registering a version under a different definition would make
            # the same declared version mean two things depending on import order.
            raise SchemaError(
                f"{name} v{version} is already registered; bump the version instead of "
                "redefining an existing one"
            )
        declared = schema.get("properties", {}).get(VERSION_FIELD)
        if declared is None:
            raise SchemaError(
                f"{name} v{version}: schema must declare a '{VERSION_FIELD}' property, or its "
                "records cannot say which version they were written under"
            )
        registered = RegisteredSchema(name, version, dict(schema), compatibility, description)
        self._schemas[key] = registered
        return registered

    def register_file(
        self, path: Path, compatibility: Compatibility = Compatibility.BREAKING
    ) -> RegisteredSchema:
        """Register from a committed JSON Schema file, taking name and version from it."""
        document = json.loads(path.read_text(encoding="utf-8"))
        name = document.get("x-aegis-schema-name")
        version = document.get("x-aegis-schema-version")
        if not name or not isinstance(version, int):
            raise SchemaError(
                f"{path}: schema file must carry 'x-aegis-schema-name' and an integer "
                "'x-aegis-schema-version'; a schema that cannot name itself cannot be registered"
            )
        return self.register(name, version, document, compatibility, document.get("description", ""))

    def versions(self, name: str) -> list[int]:
        return sorted(version for (registered, version) in self._schemas if registered == name)

    def latest(self, name: str) -> RegisteredSchema:
        versions = self.versions(name)
        if not versions:
            raise SchemaError(f"no schema registered under the name {name!r}")
        return self._schemas[(name, versions[-1])]

    def get(self, name: str, version: int) -> RegisteredSchema:
        try:
            return self._schemas[(name, version)]
        except KeyError:
            raise UnknownSchemaVersion(name, version, self.versions(name)) from None

    def validate(self, name: str, record: Mapping[str, Any]) -> RegisteredSchema:
        """Validate a record against the version it declares.

        The version comes from the record, never from the caller: a reader that
        assumes a version is a reader that can be wrong about one.
        """
        if VERSION_FIELD not in record:
            raise SchemaError(
                f"{name}: record has no '{VERSION_FIELD}'. AEGIS refuses to guess which schema a "
                "record was written under."
            )
        version = record[VERSION_FIELD]
        if not isinstance(version, int):
            raise UnknownSchemaVersion(name, version, self.versions(name))
        registered = self.get(name, version)

        validator = jsonschema.Draft202012Validator(registered.schema)
        problems = sorted(validator.iter_errors(record), key=lambda e: list(e.absolute_path))
        if problems:
            detail = "\n".join(
                f"  - {'.'.join(str(p) for p in problem.absolute_path) or '(root)'}: {problem.message}"
                for problem in problems
            )
            raise SchemaError(f"{name} v{version} record is invalid:\n{detail}")
        return registered


# ---------------------------------------------------------------------------
# Codecs. JSON and CSV at M0; Parquet/Arrow/DuckDB arrive with M2's data.
# ---------------------------------------------------------------------------


def to_jsonl(records: Iterable[Mapping[str, Any]]) -> str:
    """One record per line, keys sorted, no insignificant whitespace.

    Sorted keys so a dataset is diffable and hashable: an interchange format
    whose bytes depend on insertion order cannot be compared between runs.
    """
    return "".join(
        json.dumps(record, sort_keys=True, separators=(",", ":")) + "\n" for record in records
    )


def from_jsonl(text: str) -> list[dict[str, Any]]:
    records = []
    for lineno, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except ValueError as exc:
            raise SchemaError(f"line {lineno}: not valid JSON: {exc}") from exc
        if not isinstance(record, dict):
            raise SchemaError(f"line {lineno}: each line must be a JSON object")
        records.append(record)
    return records


def to_csv(records: Sequence[Mapping[str, Any]], columns: Sequence[str]) -> str:
    """Write CSV with an explicit column order and \\n line endings.

    Explicit columns rather than "whatever the first record had": a CSV whose
    column order depends on the data is a CSV that changes shape when the data
    does.
    """
    buffer = io.StringIO()
    writer = csv.DictWriter(buffer, fieldnames=list(columns), lineterminator="\n")
    writer.writeheader()
    for record in records:
        missing = set(columns) - set(record)
        if missing:
            raise SchemaError(f"record is missing columns {sorted(missing)}")
        writer.writerow({column: record[column] for column in columns})
    return buffer.getvalue()


def from_csv(text: str, integer_columns: Sequence[str] = ()) -> list[dict[str, Any]]:
    """Read CSV, converting the named columns back to integers.

    CSV has no types, so the caller states which columns are integers. Guessing
    from the values would turn an instrument code like "007" into 7.
    """
    reader = csv.DictReader(io.StringIO(text))
    records: list[dict[str, Any]] = []
    for row in reader:
        record: dict[str, Any] = dict(row)
        for column in integer_columns:
            if column in record and record[column] != "":
                try:
                    record[column] = int(record[column])
                except ValueError as exc:
                    raise SchemaError(f"column {column!r} is not an integer: {record[column]!r}") from exc
        records.append(record)
    return records
