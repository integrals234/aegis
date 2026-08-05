---
name: close-milestone
description: Close an AEGIS milestone only after independent audit and all gates pass.
disable-model-invocation: true
---

Close milestone $ARGUMENTS only if the latest independent audit passes.

1. Run `python3 tools/audit_requirements.py --milestone $ARGUMENTS`.
2. Run the milestone's full build/test/reproduction commands.
3. Verify every evidence path exists.
4. Change passing requirements from `implemented` to `verified`.
5. Write `experiments/milestone-reports/$ARGUMENTS.md`.
6. Update `docs/BUILD_STATE.md` to the next owner-approved milestone.
7. Summarize limitations and deferred work.
Do not modify frozen files or mark future requirements complete.
