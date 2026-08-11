"""M2 slice 13 -- AEGIS-059 (M2 portion): the `Feed` protocol and
`HistoricalReplayFeed`.

Records are deliberately constructed out of both event-time and ingestion
order, so a test that only checked "iterates all records" could pass while
the sort itself was wrong.
"""

from __future__ import annotations

import pytest
from futures.replay import Feed, HistoricalReplayFeed, canonical_sort_key, sort_canonical

pytestmark = pytest.mark.unit


def record(event_time_ns: int, source_sequence: int, contract_symbol: str, record_index: int) -> dict:
    return {
        "event_time_ns": event_time_ns,
        "source_sequence": source_sequence,
        "contract_symbol": contract_symbol,
        "record_index": record_index,
    }


# Ingestion order (by record_index): 0, 1, 2, 3 -- deliberately not the
# canonical replay order, which is (200) < (200, tie broken by sequence) <
# (300) < (300, later record_index).
UNSORTED = [
    record(300, 1, "A", 0),
    record(200, 5, "B", 1),
    record(200, 2, "B", 2),
    record(300, 1, "A", 3),
]


def test_sort_canonical_orders_by_time_then_sequence_then_symbol_then_record_index():
    sorted_records = sort_canonical(UNSORTED)
    assert [r["record_index"] for r in sorted_records] == [2, 1, 0, 3]
    assert [canonical_sort_key(r) for r in sorted_records] == sorted(
        canonical_sort_key(r) for r in UNSORTED
    )


def test_sort_canonical_does_not_mutate_or_alias_input():
    original = [dict(r) for r in UNSORTED]
    sort_canonical(UNSORTED)
    assert original == UNSORTED


def test_historical_replay_feed_satisfies_the_feed_protocol():
    feed = HistoricalReplayFeed(UNSORTED)
    assert isinstance(feed, Feed)


def test_historical_replay_feed_iterates_in_canonical_order():
    feed = HistoricalReplayFeed(UNSORTED)
    assert [r["record_index"] for r in feed] == [2, 1, 0, 3]


def test_historical_replay_feed_is_exhausted_after_one_pass():
    feed = HistoricalReplayFeed(UNSORTED)
    list(feed)
    assert list(feed) == []


def test_cursor_is_none_before_the_first_record():
    feed = HistoricalReplayFeed(UNSORTED)
    assert feed.cursor() is None


def test_cursor_tracks_the_most_recently_emitted_record_index():
    feed = HistoricalReplayFeed(UNSORTED)
    next(feed)
    assert feed.cursor() == 2
    next(feed)
    assert feed.cursor() == 1


def test_resume_from_skips_through_the_given_record_and_no_further():
    feed = HistoricalReplayFeed(UNSORTED)
    feed.resume_from(2)  # the first record in canonical order
    assert [r["record_index"] for r in feed] == [1, 0, 3]


def test_resume_from_at_the_last_record_leaves_nothing_to_replay():
    feed = HistoricalReplayFeed(UNSORTED)
    feed.resume_from(3)
    assert list(feed) == []


def test_resume_from_unknown_record_index_raises():
    feed = HistoricalReplayFeed(UNSORTED)
    with pytest.raises(KeyError):
        feed.resume_from(999)


def test_empty_feed_iterates_to_nothing():
    feed = HistoricalReplayFeed([])
    assert list(feed) == []
    assert feed.cursor() is None
