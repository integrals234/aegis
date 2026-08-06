"""Snapshot and recovery contract (AEGIS-237).

Interface only. AEGIS-237 requires critical stateful services to document
shutdown, snapshot and recovery behaviour, and ``docs/RECOVERY_CONTRACT.md``
records the sequence. What is deliberately **absent** is a snapshot *store*.

A general-purpose store is where a shared state substrate is born: by M9 the
exchange and the participant would both be persisting through it, and the
AEGIS-004 boundary between them would exist only in the diagram. So each module
implements this protocol over its own state and owns its own persistence —
exchange recovery arrives with the exchange in M1, participant recovery with the
participant in M3.

Two rules make a snapshot trustworthy, and both are enforced here rather than
described:

* a snapshot carries its **version**, and restoring an unknown version fails
  rather than guessing at the layout;
* a restored component **equals** the one that was snapshotted, checked by
  round-tripping rather than by inspection.
"""

from __future__ import annotations

from typing import Protocol, runtime_checkable


class RecoveryError(RuntimeError):
    """A snapshot could not be restored. The message says why, not "failed"."""


class UnsupportedSnapshotVersion(RecoveryError):
    """The snapshot was written by a version this build does not understand.

    Refusing is the point. A component that restores an unknown layout produces
    plausible state that is silently wrong, and the resulting run looks normal.
    """

    def __init__(self, found: int, supported: int) -> None:
        super().__init__(
            f"snapshot version {found} cannot be restored by this build, which writes and "
            f"understands version {supported}. Recovering a layout this build does not know "
            "would produce state that looks right and is not."
        )
        self.found = found
        self.supported = supported


@runtime_checkable
class Snapshotable(Protocol):
    """A component that can persist and restore its own state.

    ``bytes`` in and out on purpose: the contract says nothing about format, so
    a module may choose whatever suits its state without every other module
    inheriting the choice.
    """

    def snapshot_version(self) -> int:
        """The version this component writes and can restore."""
        ...

    def write_snapshot(self) -> bytes:
        """Serialize current state. Must not mutate the component."""
        ...

    def restore(self, payload: bytes) -> None:
        """Replace state from a snapshot, or raise RecoveryError."""
        ...


def check_round_trip(component: Snapshotable, restored: Snapshotable) -> None:
    """Assert that a snapshot round-trips byte-for-byte.

    The property worth testing is not "restore ran without raising" but "the
    restored component is the one that was saved". Comparing re-serialized
    bytes tests that; inspecting a few fields tests the fields somebody
    remembered to inspect.
    """
    original = component.write_snapshot()
    restored.restore(original)
    if restored.write_snapshot() != original:
        raise RecoveryError(
            "snapshot round trip is not stable: the restored component serializes differently "
            "from the one it was restored from, so recovery loses or reorders state"
        )
