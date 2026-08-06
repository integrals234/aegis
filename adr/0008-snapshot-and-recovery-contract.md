# ADR-0008: Snapshot and recovery contract

- Status: Accepted
- Date: 2026-08-06
- Requirement IDs: AEGIS-237, AEGIS-003
- Milestone: M0

## Context

AEGIS-237 requires critical stateful services to document shutdown, snapshot and
recovery behaviour, and its acceptance names recovery tests for exchange and
participant state. Neither exists at M0.

The obvious M0 deliverable — a snapshot store the later services can use — is the
one thing that should not be built. A general-purpose store is where a shared
state substrate is born: by M9 the exchange and the participant would both
persist through it, and the AEGIS-004 separation between them would survive only
in the architecture diagram.

## Decision

M0 delivers the **contract and the interface**, no store and no concrete
recovery behaviour. `docs/RECOVERY_CONTRACT.md` states five obligations:

1. **Ownership is explicit.** A snapshot covers exactly the state its owning
   module is responsible for. No snapshot spans the exchange/participant
   boundary; services needing consistent snapshots coordinate through recorded
   events, not a shared file.
2. **Every snapshot carries its version**, and a build meeting a version it does
   not understand refuses. Restoring an unknown layout produces state that looks
   plausible and is wrong, and the run that follows looks normal.
3. **Snapshots are byte-stable**: fixed key order, fixed number formatting, no
   map iteration order, no wall-clock reading, and never a monotonic timestamp.
4. **Recovery is verified by round trip**, not by absence of an exception.
   `restore()` returning normally proves nothing; the check compares
   re-serialized bytes.
5. **Shutdown is a defined sequence**: stop accepting input, finish or record
   abandonment of in-flight work, snapshot, flush logs, report terminal health.

Each module implements the protocol over its own state and owns its persistence.
Exchange recovery arrives with the exchange in M1, participant recovery with the
OMS and portfolio in M3, and restart recovery for paper trading in M9.

## Alternatives considered

**A generic snapshot store in `cpp/common`.** Rejected for the reason above: it
creates the shared substrate the architecture exists to prevent, and it is
easier to add later than to remove.

**Deferring the whole requirement to M1.** Rejected: the contract is what makes
M1's implementation reviewable. Without it, M1 invents a snapshot format and M3
invents a second one.

**Requiring a specific serialization format.** Rejected: the contract's
properties (versioned, byte-stable, round-trip-verified) are what matter, and a
module's state may be better served by a format the contract need not know about.

## Consequences

- AEGIS-237 closes M0 `implemented` with a registered obligation naming the
  M1, M3 and M9 evidence still owed.
- M1 and M3 each write their own persistence, which is more code than a shared
  store and keeps the boundary real.
- The round-trip check is available from M0, so the first recovery
  implementation has a test to satisfy rather than one to invent.

## Verification

- `tests/unit/test_recovery_contract.py` exercises the contract through a real
  implementer, including a deliberately unstable implementation that `restore()`
  accepts and the round-trip check rejects.
- `docs/RECOVERY_CONTRACT.md` records the obligations table.

## Owner approval

Recorded in the approved M0 plan (`experiments/plans/M0.md`, Part 6, ADR-0008).
