# AEGIS Claude Code Build Pack

This repository scaffold converts the full AEGIS project into a frozen, traceable implementation contract for Claude Code.

## What this pack does

- Preserves **238 explicit requirements** across every AEGIS subsystem.
- Freezes the canonical specification against accidental Claude edits.
- Gives every feature a stable ID, milestone and acceptance criterion.
- Requires implementation/test/report evidence before a feature is verified.
- Provides specialist Claude Code subagents and milestone skills.
- Adds structural audits, hooks and CI.
- Provides exact prompts for M0 through M9.
- Prevents unsupported CV and benchmark claims.

It cannot mathematically guarantee that an AI never makes a mistake. It makes mistakes visible and blocks the most dangerous forms of silent scope loss.

## Recommended environment for you

Use Windows as the host, but open the repository inside **WSL2 Ubuntu** through VS Code. Develop and test there. Treat WSL benchmark figures as development figures; run final low-latency benchmarks on native Linux if you want serious CV latency claims.

## Claude Code setup

1. Install/update VS Code and the official Claude Code extension.
2. Install the standalone Claude Code CLI inside WSL so `claude` works in the integrated terminal.
3. Open the WSL repository root in VS Code.
4. Run:

```bash
python3 tools/audit_requirements.py --quick
python3 tools/generate_traceability.py
git init
git add .
git commit -m "chore: freeze AEGIS canonical specification"
claude --model opus --permission-mode plan
```

The `opus` alias selects the latest Opus model available to your account. In the VS Code panel, choose Opus 5 when available.

## The only safe build loop

For each milestone:

1. Create a branch: `git switch -c milestone/mX-name`.
2. Start a **fresh Opus plan-mode session**.
3. Use the matching file under `prompts/`.
4. Review the requirement-mapped plan.
5. Start a fresh implementation session.
6. Invoke `/implement-milestone MX`.
7. Run `/audit-milestone MX`.
8. Fix every failure.
9. Run `/close-milestone MX`.
10. Merge only after CI and your manual demo pass.

Do not ask Claude to “build the entire project.” That creates context collapse, fake completion and feature loss.

## First action

Open `prompts/00_FIRST_SESSION.md` and paste its prompt into Claude Code.

## Useful commands

```bash
make audit
make traceability
python3 tools/audit_requirements.py --milestone M1
python3 tools/update_status.py AEGIS-001 in_progress
```

## Source of truth

- Human-readable: `docs/MASTER_SPEC.md`
- Machine-readable: `requirements/requirements.json`
- Live status/evidence: `requirements/implementation_status.json`
- Generated matrix: `docs/TRACEABILITY_MATRIX.md`

## Important truth

Do not put AEGIS on your CV as fully built until the relevant claims are independently verifiable. You may list a truthful partial version with the exact implemented scope.
