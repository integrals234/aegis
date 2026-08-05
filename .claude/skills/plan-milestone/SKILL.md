---
name: plan-milestone
description: Create a requirement-mapped implementation plan for one AEGIS milestone before any code edits.
context: fork
agent: Plan
disable-model-invocation: true
---

Plan milestone $ARGUMENTS.

Read `CLAUDE.md`, the frozen specification, requirement catalogue, implementation status, architecture, roadmap, build state and relevant code. Do not edit.

Return:
- exact in-scope requirement IDs;
- current gaps;
- file/API/data-model plan;
- dependency and clock model;
- test-first acceptance plan;
- failure modes;
- ADRs required;
- commands to verify;
- explicitly out-of-scope later milestones.
