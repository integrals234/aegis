# ADR-0011: Order lifecycle semantics

- Status: Accepted
- Date: 2026-08-07
- Requirement IDs: AEGIS-029, AEGIS-030, AEGIS-031, AEGIS-032, AEGIS-035
- Milestone: M1

## Context

M1 needs three more lifecycle decisions beyond acceptance and matching
(ADR-0009): what a cancel or modify targets and how it can fail, what
distinguishes a priority-retaining quantity decrease from a cancel-replace,
and what happens to a market order's unfilled residual. Each choice becomes a
golden fixture later milestones would have to regenerate if changed, so each
is recorded here rather than left to whichever slice happens to touch the
code path first.

## Decision

### Market-order acceptance and residual policy

A market order is validated exactly like a limit order and, once accepted,
is never rejected for lack of liquidity. `AEGIS-029`'s frozen text is
"execute market orders against available liquidity with explicit residual
handling" — nowhere does it require rejection, so absent liquidity is an
execution outcome, not a validation failure. Concretely: the order is
accepted (`kOrderAccepted`), matched against whatever is available (zero or
more trades), and any unfilled residual is terminated
(`TerminationReason::kResidualCanceled`) rather than rested. An **empty
book** is the same path with zero trades: accepted, then immediately
terminated with the whole quantity as the residual. The only validation
specific to a market order is that it must not carry a price
(`RejectReason::kPriceOnMarketOrder`) — decided before the order exists,
exactly like an off-tick limit price, never a book-state outcome.

### Modify semantics: decrease-in-place vs. cancel-replace

`ModifyOrderCommand.new_quantity_units` is the order's new **total**
quantity (FIX `OrderQty` convention) — the same interpretation an
`original_quantity` would carry if the order were being resubmitted from
scratch. This is what makes `kModifyBelowFilled` a meaningful, checkable
condition: `new_quantity_units < cumulative_filled` is rejected before
anything else is decided, because no total below what has already executed
is coherent.

Given a passing quantity, the modify is classified by comparing against the
**live order's current state**:

- `new_price_units == current price` **and** the implied new remaining
  (`new_quantity_units − cumulative_filled`) **is not greater** than the
  current `remaining` → **decrease-in-place**. `original_quantity` is *not*
  overwritten — P1 (`experiments/plans/M1.md` §4.10) requires it stay fixed
  at whatever it was when the order was created — only `remaining` moves to
  the implied value, and the difference is added to `cancelled_quantity`.
  Priority is untouched: the order keeps its position at the front of
  wherever it already was in its level's queue. Emits `OrderModifiedEvent`.
- Any price change, **or** an implied remaining greater than the current
  `remaining` (a quantity increase) → **cancel-replace**. The live order
  terminates (`TerminationReason::kReplaced`, its whole `remaining` added to
  `cancelled_quantity`) and a **new, distinct `OrderId`** is created with its
  own fresh `original_quantity = new_quantity_units`, `remaining =
  new_quantity_units` (nothing filled yet — it is a new order, not a
  continuation) and a **new `Priority`** from the modify command's own
  `CommandSequence` — the tail of its level, per AEGIS-032. The replacement
  is matched exactly like a `NewOrder` (ADR-0009): it can trade immediately
  if the new price crosses, and any of *its* residual rests or not by the
  ordinary limit-order rule, since a modify never targets a market order (a
  market order never rests, so there is nothing live to modify).

Both paths share one lookup/ownership check, because both a cancel and a
modify target an already-live `OrderId`, not a fresh submission:

1. `order_id` is not live (never existed, already terminated, or was
   replaced) → `RejectReason::kUnknownOrderId`.
2. Live but owned by a different `participant_id` → `RejectReason::kNotOrderOwner`.

`MatchingEngine` exposes this as one private helper both `apply_cancel_order`
and `apply_modify_order` call, rather than duplicating the check — the two
commands differ in what they do with a validated, owned, live order, not in
how they find one.

### `kOrderAlreadyTerminal` does not exist

Retaining a distinct "already terminal" reject reason would require an
unbounded tombstone set — every `OrderId` that ever terminated, kept forever
so a later cancel/modify against it can be told apart from one that was
never assigned. No requirement specifies a retention policy for that set,
and M1 must not invent one (`docs/LIMITATIONS.md`). A cancel or modify
against an unknown, already-filled, already-cancelled, or already-replaced
`OrderId` therefore returns the same `kUnknownOrderId` as one that was never
assigned — one reason, one testable row, no unbounded state.

### `(participant_id, client_order_id)` scope and reuse

Duplicate detection is over the pair `(participant_id, client_order_id)`,
not `client_order_id` alone — two participants may reuse the same value.
Uniqueness is enforced over **live orders only**: the book's
`live_client_ids_` map (`cpp/exchange/order_book/book.{hpp,cpp}`, landed in
slice 1) holds exactly the currently open orders and is derivable from the
book's own state, so a snapshot restore rebuilds it rather than persisting
it — see ADR-0013 for the snapshot and recovery design. Reuse of a client order id after its order
has terminated is accepted — enforcing otherwise is exactly the tombstone
problem `kOrderAlreadyTerminal`'s removal avoids. `RejectReason::kDuplicateClientOrderId`
is the enumerator for the live-collision case; the check itself and its
dedicated tests land with the full reject matrix (AEGIS-035, slice 5), since
it is a `NewOrder`-only concern and does not block modify/cancel from
functioning.

### The reject taxonomy is a closed set

Every enumerator is a *validation* failure decided before an order's state
changes — reachable from a malformed or invalid command alone, never from
book state:

`kUnknownInstrument`, `kDuplicateClientOrderId`, `kNonPositiveQuantity`,
`kQuantityNotOnLot`, `kQuantityOutOfRange`, `kPriceNotOnTick`,
`kPriceOutOfBand`, `kPriceOnMarketOrder`, `kUnknownOrderId`,
`kNotOrderOwner`, `kModifyBelowFilled`, `kMalformedMessage`.

An unfilled market residual and an empty book are not in this set (they are
terminations of an accepted order, §above), and `kOrderAlreadyTerminal` was
never added to it (§above).

### `OrderRejectedEvent` gains an `order_id` field

`cpp/events/exchange_messages.hpp`'s `OrderRejectedEvent` (ADR-0009) was
shaped for `NewOrder` rejections only: `client_order_id` identifies which
submission was rejected, because no `OrderId` exists yet to reject by. A
`CancelOrder`/`ModifyOrder` rejection has the opposite shape — it targets an
already-assigned `OrderId` and carries no `client_order_id` at all. Rather
than a second event type for a one-field difference, `OrderRejectedEvent`
gains `order_id` (0 when not applicable) alongside the existing
`client_order_id` (0 when not applicable): exactly one of the two is nonzero
for any given rejection. This is a wire-format revision to a struct
introduced in slice 1 of this same milestone, before any external consumer
existed; the golden fixture (`tests/unit/fixtures/exchange_messages/golden.json`)
and both language implementations were updated together in the same commit
that introduces the first caller needing the new field.

## Alternatives considered

- **`new_quantity_units` as "new remaining" instead of "new total."**
  Rejected: it makes `kModifyBelowFilled` unreachable in any natural
  reading (remaining and cumulative-filled are independent quantities under
  that interpretation), whereas "new total" gives the reason exactly the
  check its name describes.
- **A second wire event type for cancel/modify rejections.** Rejected: the
  two shapes differ in one field's meaning, and every other field
  (`causing_command_sequence`, `instrument_id`, `participant_id`, `reason`)
  is identical; a second type would double the encode/decode/golden-fixture
  surface for that.
- **Retaining `kOrderAlreadyTerminal`.** Rejected in the original design
  discussion recorded in `experiments/plans/M1.md` §4.8; reaffirmed here
  because the tombstone problem it requires does not shrink by the time
  modify/cancel actually exist to exercise it.

## Consequences

- `MatchingEngine` grows a private "find the live, owned order or produce
  the reject reason" helper shared by `apply_cancel_order` and
  `apply_modify_order`.
- A cancel-replace allocates a new `OrderId` even when the net economic
  effect (same price, larger quantity) might look like "the same order,
  bigger" to a caller — the identifier space records it faithfully as a
  distinct order with its own history, which is what makes P1 conservation
  well-formed on both sides of a replace (`experiments/plans/M1.md` §4.10).
- `docs/LIMITATIONS.md` gains an entry for client-id reuse after termination
  and the absence of an "already terminal" tombstone.

## Verification

- `tests/cpp/unit/test_modify_semantics.cpp` — decrease retains priority
  (proved by a following aggressor); increase and price change emit
  `OrderReplacedEvent`/`OrderTerminatedEvent{kReplaced}` with a new `OrderId`
  at the tail; modify below cumulative filled → `kModifyBelowFilled`;
  unknown/not-owner rejects for both cancel and modify.
- `tests/cpp/unit/test_market_orders.cpp` (slice 3) — market acceptance and
  residual policy.
- `tests/cpp/unit/test_reject_matrix.cpp` (slice 5) — the closed set,
  parameterized over every enumerator via a compile-time-checked table.
- `tests/unit/test_exchange_message_codec.py`,
  `tests/cpp/unit/test_exchange_messages.cpp` — the extended
  `OrderRejectedEvent` golden fixture, including a cancel/modify-shaped case.

## Owner approval

Confirmed by the owner per experiments/plans/M1.md §11.3 (market-order
acceptance, cancel-replace on increase/price-change, live-only client-id
uniqueness with reuse after termination, removal of `kOrderAlreadyTerminal`).
