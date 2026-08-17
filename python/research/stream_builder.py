"""Deterministic two-leg market-data stream for the M4 calendar-spread demo
(ADR-0025).

Constructs one two-sided quote per leg per date from each
:class:`~research.calendar_spread.CalendarSpreadObservation`'s near/far
prices, via a fixed, documented tick-spread rule. **This is not observed
tick data.** ``data_samples/futures/`` carries daily OHLC/settlement bars
only; ADR-0025 records this same disclosed construction for both the C++ CLI
demo (`aegis_participant_run --calendar-spread`) and the committed test
fixture the real-M1-matching integration test replays.
"""

from __future__ import annotations

from collections.abc import Sequence
from dataclasses import dataclass
from decimal import Decimal

from research.calendar_spread import CalendarSpreadObservation

__all__ = ["StreamBuildConfig", "build_two_leg_stream_records", "price_to_units"]


def price_to_units(price: Decimal, scale: int = 100) -> int:
    """A documented, fixed price -> integer-units convention for this
    demo/fixture only (cents, ``scale=100``): ``round(price * scale)``. Never
    applied to, or read as a claim about, any other module's price
    representation -- ``cpp/events`` price units are a wire convention this
    module happens to also use at this specific scale, not something it
    derives from.
    """
    return int((price * scale).to_integral_value())


@dataclass(frozen=True, slots=True)
class StreamBuildConfig:
    near_instrument_id: int
    far_instrument_id: int
    tick_spread_units: int
    quote_quantity_units: int
    price_scale: int = 100

    def __post_init__(self) -> None:
        if self.tick_spread_units <= 0:
            raise ValueError(f"tick_spread_units must be > 0, got {self.tick_spread_units}")
        if self.quote_quantity_units <= 0:
            raise ValueError(f"quote_quantity_units must be > 0, got {self.quote_quantity_units}")


def build_two_leg_stream_records(
    observations: Sequence[CalendarSpreadObservation], config: StreamBuildConfig
) -> list[dict[str, object]]:
    """One ``near`` record followed by one ``far`` record per observation, in
    ``as_of`` order -- the order ``run_calendar_spread_scenario`` (C++)
    requires to act once each date's far leg has landed
    (``cpp/participant/app/participant_run.hpp``). ``md_sequence`` increments
    independently per leg, starting at 1. The bid/ask around each leg's mid
    price are symmetric (``mid - tick_spread_units``, ``mid +
    tick_spread_units``), so the reconstructed book's own
    ``mid_price_units`` reproduces each observation's ``near_price``/
    ``far_price`` (scaled) exactly.
    """
    records: list[dict[str, object]] = []
    for sequence, observation in enumerate(observations, start=1):
        near_mid = price_to_units(observation.near_price, config.price_scale)
        records.append(
            {
                "leg": "near",
                "instrument_id": config.near_instrument_id,
                "md_sequence": sequence,
                "as_of": observation.as_of.isoformat(),
                "bid_price_units": near_mid - config.tick_spread_units,
                "bid_quantity_units": config.quote_quantity_units,
                "ask_price_units": near_mid + config.tick_spread_units,
                "ask_quantity_units": config.quote_quantity_units,
            }
        )

        far_mid = price_to_units(observation.far_price, config.price_scale)
        records.append(
            {
                "leg": "far",
                "instrument_id": config.far_instrument_id,
                "md_sequence": sequence,
                "as_of": observation.as_of.isoformat(),
                "bid_price_units": far_mid - config.tick_spread_units,
                "bid_quantity_units": config.quote_quantity_units,
                "ask_price_units": far_mid + config.tick_spread_units,
                "ask_quantity_units": config.quote_quantity_units,
            }
        )
    return records
