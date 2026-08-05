---
name: implement-milestone
description: Implement one approved AEGIS milestone plan with tests, evidence and truthful status updates.
disable-model-invocation: true
---

Implement only milestone $ARGUMENTS after an approved plan exists.

Rules:
1. Re-read active requirement IDs and acceptance criteria.
2. Write failing focused tests first where practical.
3. Implement the smallest coherent vertical slice.
4. Run relevant tests after each slice.
5. Do not optimize or expand scope.
6. Add evidence paths only after commands pass.
7. Set status to `implemented`, not `verified`.
8. Request independent `/audit-milestone $ARGUMENTS` before closure.
9. Never edit frozen files.
