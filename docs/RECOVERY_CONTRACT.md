# Snapshot and Recovery Contract

AEGIS-237 requires every critical stateful service to document its shutdown,
snapshot and recovery behaviour. This document is that contract. At M0 it
describes obligations and the interface that expresses them; the implementations
arrive with the services they belong to.

## What exists at M0, and what does not

| Delivered | Deferred |
|---|---|
| The contract below | Exchange-state recovery (M1, with the exchange) |
| `python/common/recovery.py` — protocol, version refusal, round-trip check | Participant-state recovery (M3, with the OMS and portfolio) |
| `tests/unit/test_recovery_contract.py` — the contract exercised through an implementer | Restart recovery for paper trading (M9, AEGIS-222) |

**There is no snapshot store, and that is deliberate.** A general-purpose store
is where a shared state substrate is born: by M9 the exchange and the
participant would both persist through it, and the AEGIS-004 separation between
them would exist only in the architecture diagram. Each module implements the
contract over its own state and owns its own persistence.

## The contract

A service is *recoverable* when it satisfies all five obligations.

### 1. State ownership is explicit

A snapshot covers exactly the state its owning module is responsible for. No
module snapshots another module's state, and no snapshot spans the
exchange/participant boundary. Two services that need consistent snapshots
coordinate through recorded events, not through a shared file.

### 2. Every snapshot carries its version

`snapshot_version()` is written into the payload. A build that meets a version it
does not understand **refuses**. Restoring an unknown layout produces state that
looks plausible and is wrong, and the run that follows looks normal — which is
the worst possible failure mode for a research platform.

### 3. Snapshots are byte-stable

The same state serializes to the same bytes: fixed key order, fixed number
formatting, no map iteration order, no wall-clock reading inside the payload. A
monotonic timestamp is never persisted — its origin differs per process, so it
would replay to a different value (see `cpp/common/time.hpp`).

Byte stability is what allows a recovery test to compare snapshots directly
rather than inspecting whichever fields somebody remembered to check.

### 4. Recovery is verified by round trip, not by absence of an exception

`restore()` returning normally proves nothing. The check is:

```
component.write_snapshot() == restored_component.write_snapshot()
```

`check_round_trip()` performs it. A component that drops or reorders state during
restore fails this comparison and passes a "did it throw?" test.

### 5. Shutdown is a defined sequence

On shutdown a service must, in order:

1. stop accepting new input;
2. finish or explicitly abandon in-flight work, recording which;
3. write a snapshot;
4. flush its log sink;
5. report its terminal health state.

A service that exits before step 3 has lost state; one that snapshots before
step 2 has recorded state that never existed.

## Verification obligations

| Milestone | Owed evidence |
|---|---|
| M1 | Exchange-state recovery test: sequencer position and book state survive a snapshot/restore cycle, verified by round trip. |
| M3 | Participant-state recovery test: OMS order lifecycle and portfolio positions survive the same cycle. |
| M9 | Restart recovery against the paper adapter, including reconciliation of positions after restore (AEGIS-221, AEGIS-222). |

These are registered in `requirements/implementation_status.json` under
`verification_blocked_until`, so AEGIS-237 cannot be marked `verified` before the
evidence exists, and `tools/audit_requirements.py --check-deferred` fails the
owing milestone if it closes without paying.
