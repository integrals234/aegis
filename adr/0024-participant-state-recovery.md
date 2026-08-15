# ADR-0024: Participant state snapshot and recovery

- Status: Accepted
- Date: 2026-08-12
- Requirement IDs: AEGIS-237
- Milestone: M3

## Context

`docs/RECOVERY_CONTRACT.md` states the M3 obligation plainly: "Participant-
state recovery test: OMS order lifecycle and portfolio positions survive the
same cycle" as the exchange-state recovery M1 already proved
(`cpp/exchange/state/snapshot.{hpp,cpp}`, ADR-0013). AEGIS-237's residual,
registered while closing M0 and re-dated while closing M0 a second time to
M3, is explicit that participant-state recovery needs the OMS and portfolio
layers this milestone builds.

This ADR records the design decision the plan of record calls for **now**, at
slice 1, so the recovery contract's shape is fixed before the OMS and
portfolio it covers are built out over slices 5 and 6 — the same reason M1
wrote ADR-0013 alongside `ExchangeSnapshot`'s first version rather than after
the fact. **The codec itself is slice 7 work**, per the M3 plan of record's
slice plan; nothing here is implemented in this commit, and this ADR does not
claim otherwise.

A second question this ADR settles ahead of time: whether participant-state
recovery (this ADR) and the M3-owed feed-recovery obligation (AEGIS-061,
ADR-0021) can share one mechanism. They cannot, honestly — see Decision.

## Decision

**`ParticipantSnapshot` v1 covers exactly OMS order lifecycle and portfolio
state — nothing else.** `docs/RECOVERY_CONTRACT.md` obligation 1 ("a snapshot
covers exactly the state its owning module is responsible for... no snapshot
spans the exchange/participant boundary") applies here exactly as it applied
to `ExchangeSnapshot`. Book-builder reconstruction state is explicitly
**not** part of this snapshot: it is recovered by AEGIS-070's snapshot-and-
replay mechanism (ADR-0021), which is a market-data concern, not a process-
recovery concern, and folding it in here would make one snapshot answer two
different questions.

**Byte-stable, version-refusing, verified by round trip — the same three
disciplines `ExchangeSnapshot` already established.** A `snapshot_version`
field an unrecognized build refuses; fixed field order and fixed-width
little-endian encoding (the same `cpp/events/wire.hpp` primitives every other
codec in this repository uses); and a round-trip check
(`component.write_snapshot() == restored.write_snapshot()`) rather than
"did restore() throw," per `docs/RECOVERY_CONTRACT.md` obligation 4.

**Verified by continuation equality across a process boundary, not merely a
round trip.** `RECOVERY_CONTRACT.md`'s own bar for M1 was: restore into a
fresh process, resume the same input, and produce output identical to an
uninterrupted run. The participant-state proof holds the OMS and portfolio to
the same bar — a snapshot taken mid-sequence, restored in a second process,
resumes order lifecycle and position tracking with no observable difference
from a process that never stopped.

**AEGIS-061 (feed recovery) and AEGIS-237 (participant-state recovery) share
a *contract*, not a *mechanism*.** AEGIS-061/070 is in-stream recovery: a gap
detected mid-feed, deltas buffered, a fresh **market-data snapshot message**
(a `cpp/events` wire type) re-bases the book, buffered deltas replay onto it
— the artifact is a message that flows through the same channel the feed
already uses. AEGIS-237 is process-boundary recovery: a byte-stable **file
snapshot** of OMS and portfolio state, restored into a fresh process — the
artifact is a file, produced and consumed by a shutdown/startup sequence, not
part of the ordinary message stream. Forcing these into one system would
answer "what changed since the gap" and "what was true when the process
stopped" with the same tool, which is not what either question asks. They are
mapped to independent tests and independent evidence; neither is claimed from
the other's proof.

## Alternatives considered

- **One snapshot covering exchange, book and participant state together** —
  rejected outright: violates `docs/RECOVERY_CONTRACT.md` obligation 1
  directly, and ADR-0008 already rejected a shared substrate for the same
  reason at M0.
- **A general-purpose snapshot store shared across modules** — rejected;
  ADR-0008 rejected this at M0 for the same reason it is wrong here: "by M9
  the exchange and the participant would both persist through it, and the
  AEGIS-004 separation between them would exist only in the architecture
  diagram."
- **Reusing AEGIS-070's in-stream buffer-and-rebase mechanism for process
  recovery** — rejected; see Decision. A market-data message and a process
  snapshot file are different artifacts serving different failure modes, and
  presenting one as the other would be a false economy, not a simplification.

## Consequences

- Slice 7 implements `ParticipantSnapshot` v1 in `cpp/participant/oms` and
  `cpp/participant/portfolio` (each module owns its own portion, composed by
  `cpp-participant-app` — no snapshot type crosses a layer that does not own
  the state inside it) and extends `aegis_participant_run` with
  `--snapshot-out`/`--restore-from`, mirroring `aegis_exchange_replay`'s
  existing flags.
- M9's restart recovery (AEGIS-221/AEGIS-222) extends this codec rather than
  inventing a third one, the same way M9's feed adapter reuses AEGIS-070's
  recovery path rather than AEGIS-061's fixture-specific mechanics.
- `docs/RECOVERY_CONTRACT.md`'s M3 row is updated from "owed" to "paid" only
  when slice 7 lands with the evidence this ADR's Verification section
  describes — not by this commit.

## Verification

Not yet implemented; this ADR fixes the design slice 7 builds against. Slice
7 will add:

- a participant-state analogue of `tests/cpp/unit/test_snapshot_roundtrip.cpp`
  — round-trip equality for `ParticipantSnapshot` v1;
- a participant-state analogue of `tests/replay/test_exchange_determinism.py`
  — continuation equality for OMS order lifecycle and portfolio positions
  across a real process boundary, using `aegis_participant_run
  --snapshot-out`/`--restore-from`;
- an update to `docs/RECOVERY_CONTRACT.md`'s M3 row from "owed" to "paid,"
  and a `tools/update_status.py --clear-obligation` call discharging
  AEGIS-237's `verification_blocked_until`.

## Owner approval

Authorized under the owner-approved M3 plan of record
(`experiments/plans/M3.md`), 2026-08-12.
