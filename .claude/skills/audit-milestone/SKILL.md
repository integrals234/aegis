---
name: audit-milestone
description: Independently audit an AEGIS milestone against every requirement and acceptance criterion.
context: fork
agent: aegis-spec-auditor
disable-model-invocation: true
---

Audit milestone $ARGUMENTS. Inspect the diff, implementation status, evidence paths, tests and reports. Run relevant non-destructive checks. Return a requirement-by-requirement PASS/FAIL report and exact remediation. Do not edit.
