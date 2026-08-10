"""Unified feed abstraction and the historical replay feed (AEGIS-059, M2 slice 13).

`Feed` is the interface every event source this milestone owns implements:
anything iterable for normalized record dicts, consumed in the order it
wants to be consumed. `HistoricalReplayFeed` is the one concrete
implementation M2 builds -- it takes already-ingested `futures_bar.v1`
records (`python/futures/ingest.py`, which deliberately preserves file
order and does *not* sort them) and iterates them in the actual replay
order: `(event_time_ns, source_sequence, contract_symbol, record_index)`,
the same canonical order `cpp/replay/replay_event.hpp` defines and
`cpp/replay/replay_engine.hpp`'s `ReplayEngine` walks.

# AEGIS-059 scope

M2 builds the `Feed` interface and `HistoricalReplayFeed`. Live and paper
feeds, and multi-feed contract tests run against a real strategy, are the
M9 residual -- no strategy exists before M4 (`configs/architecture_rules.yaml`
dates `cpp-participant-strategy` to M4), so a contract test against one
cannot be written honestly yet.

# Why the sort is native Python, not a call into the bindings

`cpp_bindings.sort_canonical` (M2 slice 13) proves the C++ engine's
`canonical_less` and this module's own sort agree on the same input --
see `tests/integration/test_replay_bindings_roundtrip.py`. This feed does
not *depend* on the compiled extension to do that sort itself: a feed
usable on a machine with no C++ toolchain, consistent with every other
`python/futures` module, is worth more than saving one `sorted()` call.

# Cursor and resume

`cursor()` and `resume_from()` mirror `ReplayEngine::cursor()` /
`ReplayEngine::resume_from()` (`cpp/replay/replay_engine.hpp`, M2 slice 9)
in Python terms: `cursor()` reports the `record_index` of the most
recently emitted record (or `None` before the first), and `resume_from`
seeks so the next record emitted is the one immediately after a given
`record_index` in canonical order -- never re-emitting that record or
anything before it.
"""

from __future__ import annotations

from collections.abc import Iterator, Mapping, Sequence
from typing import Any, Protocol, runtime_checkable

__all__ = ["Feed", "HistoricalReplayFeed", "canonical_sort_key", "sort_canonical"]


def canonical_sort_key(record: Mapping[str, Any]) -> tuple[int, int, str, int]:
    """The same four-part key `cpp/replay/replay_event.hpp`'s `canonical_compare`
    orders by: `(event_time_ns, source_sequence, contract_symbol, record_index)`.
    `record_index` is read from the record, never recomputed here."""
    return (
        record["event_time_ns"],
        record["source_sequence"],
        record["contract_symbol"],
        record["record_index"],
    )


def sort_canonical(records: Sequence[Mapping[str, Any]]) -> list[dict[str, Any]]:
    """Sort already-normalized records into canonical replay order.

    Python peer of the C++ binding of the same name (`cpp_bindings.sort_canonical`,
    M2 slice 13) -- the symmetry test compares the two directly on the same
    input rather than assuming they agree.
    """
    return sorted((dict(record) for record in records), key=canonical_sort_key)


@runtime_checkable
class Feed(Protocol):
    """Anything iterable for normalized event dict records, in the order it
    wants to be consumed. `HistoricalReplayFeed` is the one M2-owned
    implementation; the M9 residual adds live/paper feeds behind this same
    Protocol."""

    def __iter__(self) -> Iterator[dict[str, Any]]: ...


class HistoricalReplayFeed:
    """Replays already-ingested records in canonical replay order.

    Ingestion (`futures.ingest.ingest`) deliberately preserves file order,
    not replay order -- this is the one place that order is actually
    imposed, once, at construction, so every consumer downstream sees the
    same sequence regardless of how the records happened to arrive.
    """

    def __init__(self, records: Sequence[Mapping[str, Any]]) -> None:
        self._records: tuple[dict[str, Any], ...] = tuple(sort_canonical(records))
        self._position_by_record_index: dict[int, int] = {
            record["record_index"]: position for position, record in enumerate(self._records)
        }
        self._next_position = 0

    def __iter__(self) -> Iterator[dict[str, Any]]:
        return self

    def __next__(self) -> dict[str, Any]:
        if self._next_position >= len(self._records):
            raise StopIteration
        record = self._records[self._next_position]
        self._next_position += 1
        return record

    def __len__(self) -> int:
        return len(self._records)

    def cursor(self) -> int | None:
        """`record_index` of the most recently emitted record, or `None` before
        the first `__next__` call."""
        if self._next_position == 0:
            return None
        return int(self._records[self._next_position - 1]["record_index"])

    def resume_from(self, record_index: int) -> None:
        """Seek so the next emitted record is the one immediately after
        `record_index` in canonical order -- mirrors
        `ReplayEngine::resume_from` (`cpp/replay/replay_engine.hpp`, M2
        slice 9). Never re-emits `record_index` itself or anything before it.
        """
        if record_index not in self._position_by_record_index:
            raise KeyError(f"no record with record_index={record_index} in this feed")
        self._next_position = self._position_by_record_index[record_index] + 1
