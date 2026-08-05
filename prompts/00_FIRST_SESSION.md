# First Claude Code Session Prompt

Paste this into a new Claude Code session running Opus in **plan mode**:

```text
You are the principal architect for AEGIS.

Read, in this order:
1. CLAUDE.md
2. docs/BUILD_STATE.md
3. docs/MASTER_SPEC.md
4. requirements/requirements.json
5. requirements/implementation_status.json
6. docs/ARCHITECTURE.md
7. docs/ROADMAP.md
8. docs/ACCEPTANCE_GATES.md
9. prompts/M0_FOUNDATION.md

Do not write or edit any file. Do not generate implementation code.

First run the requirement-integrity audit read-only. Then use the architecture-guardian and spec-auditor subagents to review M0. Produce a concrete M0 implementation plan mapped to exact AEGIS requirement IDs, files, tests, commands, failure modes and ADRs. Explicitly list everything that is out of scope for M0.

The plan must preserve every frozen requirement and must not claim that later project features are implemented.
```

After reviewing the plan, save it manually or ask Claude to save only the approved plan under `experiments/plans/M0.md`, then start a fresh implementation session and invoke:

```text
/implement-milestone M0
```
