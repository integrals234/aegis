"""AEGIS-237 — the snapshot/recovery contract, exercised through a real implementer.

M0 delivers the contract, not a recovery mechanism: exchange-state recovery
arrives with the exchange in M1 and participant-state recovery with the
participant in M3. What can be tested now is that the contract has teeth —
that an unknown snapshot version is refused rather than guessed at, and that a
round trip is checked by comparison rather than by the absence of an exception.
"""

from __future__ import annotations

import json

import pytest
from common.recovery import (
    RecoveryError,
    Snapshotable,
    UnsupportedSnapshotVersion,
    check_round_trip,
)

pytestmark = pytest.mark.unit

SNAPSHOT_VERSION = 1


class Counterparty:
    """A minimal component implementing the contract over its own state.

    Deliberately owns its persistence. A shared snapshot store is where the
    exchange and the participant would end up writing through one substrate,
    and the AEGIS-004 boundary would survive only in the diagram.
    """

    def __init__(self, positions: dict[str, int] | None = None) -> None:
        self.positions = dict(positions or {})

    def snapshot_version(self) -> int:
        return SNAPSHOT_VERSION

    def write_snapshot(self) -> bytes:
        payload = {
            "snapshot_version": self.snapshot_version(),
            "positions": dict(sorted(self.positions.items())),
        }
        return json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")

    def restore(self, payload: bytes) -> None:
        try:
            document = json.loads(payload)
        except ValueError as exc:
            raise RecoveryError(f"snapshot is not valid JSON: {exc}") from exc
        found = document.get("snapshot_version")
        if found != SNAPSHOT_VERSION:
            raise UnsupportedSnapshotVersion(found, SNAPSHOT_VERSION)
        self.positions = dict(document["positions"])


class UnstableComponent(Counterparty):
    """Restores something subtly different from what it was given.

    This is the failure the round-trip check exists to catch: `restore` runs
    without raising, and the state is quietly wrong.
    """

    def restore(self, payload: bytes) -> None:
        super().restore(payload)
        self.positions.pop(next(iter(self.positions), ""), None)


def test_component_satisfies_the_protocol():
    assert isinstance(Counterparty(), Snapshotable)


def test_round_trip_restores_the_same_state():
    original = Counterparty({"ESZ6": 3, "CLF7": -2})
    restored = Counterparty()
    restored.restore(original.write_snapshot())
    assert restored.positions == original.positions


def test_snapshot_does_not_mutate_the_component():
    component = Counterparty({"ESZ6": 3})
    before = dict(component.positions)
    component.write_snapshot()
    assert component.positions == before


def test_snapshot_is_byte_stable():
    """Recovery evidence is compared byte for byte, so ordering must be fixed."""
    first = Counterparty({"b": 2, "a": 1})
    second = Counterparty({"a": 1, "b": 2})
    assert first.write_snapshot() == second.write_snapshot()


def test_unknown_snapshot_version_is_refused():
    """Restoring an unknown layout produces state that looks right and is not."""
    component = Counterparty()
    payload = json.dumps({"snapshot_version": 99, "positions": {}}).encode("utf-8")
    with pytest.raises(UnsupportedSnapshotVersion) as excinfo:
        component.restore(payload)
    assert excinfo.value.found == 99
    assert excinfo.value.supported == SNAPSHOT_VERSION
    assert "looks right and is not" in str(excinfo.value)


def test_missing_snapshot_version_is_refused():
    component = Counterparty()
    with pytest.raises(UnsupportedSnapshotVersion):
        component.restore(json.dumps({"positions": {}}).encode("utf-8"))


def test_corrupt_snapshot_is_reported_as_such():
    with pytest.raises(RecoveryError, match="not valid JSON"):
        Counterparty().restore(b"{not json")


def test_round_trip_helper_accepts_a_faithful_implementation():
    check_round_trip(Counterparty({"ESZ6": 1}), Counterparty())


def test_round_trip_helper_catches_silent_state_loss():
    """`restore` returning normally is not evidence that state survived."""
    with pytest.raises(RecoveryError, match="round trip is not stable"):
        check_round_trip(Counterparty({"ESZ6": 1, "CLF7": 2}), UnstableComponent())
