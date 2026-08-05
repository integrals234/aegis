# M1 Prompt — Deterministic Exchange Core

Implement the correctness-first exchange core: typed events/IDs, FIFO price-time book, limit/market/add/cancel/modify, rejects, partial/full fills, invariant checker, event log, snapshot and deterministic replay. Use baseline safe containers first. Do not add lock-free queues, custom allocators, strategies or UI.

Run the full plan → implement → audit → close workflow for M1.
