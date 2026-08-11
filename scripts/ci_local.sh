#!/usr/bin/env bash
# Run the full AEGIS gate matrix locally (AEGIS-234).
#
# This is the *same* set of checks .github/workflows/ci.yml runs. Keeping one
# list in two places would guarantee drift, so the workflow calls into the same
# stages by name and tests/unit/test_ci_parity.py asserts the two agree.
#
# It is deliberately described as "the local gate matrix" and never as
# satisfying AEGIS-234, whose acceptance names a passing workflow on the
# protected default branch. Only a real CI run can satisfy that, and the
# requirement carries a registered verification obligation until one exists.
#
# Every stage runs even after an earlier one fails, then the summary reports all
# of them. Stopping at the first failure means finding the next one on the next
# run, and the run is not fast.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

VENV="${AEGIS_VENV:-$ROOT/.venv}"
PYTHON="${AEGIS_PYTHON:-$VENV/bin/python}"
export PATH="$VENV/bin:$PATH"

STAGE_NAMES=()
STAGE_STATUS=()
FAILURES=0

stage() {
  local name="$1"
  shift
  printf '\n\033[1m=== %s ===\033[0m\n' "$name"
  if "$@"; then
    STAGE_NAMES+=("$name")
    STAGE_STATUS+=("PASS")
  else
    STAGE_NAMES+=("$name")
    STAGE_STATUS+=("FAIL")
    FAILURES=$((FAILURES + 1))
  fi
}

traceability_is_current() {
  "$PYTHON" tools/generate_traceability.py && git diff --exit-code docs/TRACEABILITY_MATRIX.md
}

deferred_register_is_current() {
  "$PYTHON" tools/generate_deferred_register.py && git diff --exit-code docs/DEFERRED_VERIFICATION.md
}

cpp_preset() {
  local preset="$1"
  cmake --preset "$preset" >/dev/null && cmake --build --preset "$preset" && ctest --preset "$preset"
}

negative_gates() {
  # Each gate must fail on its committed counter-example. A gate that cannot
  # fail is a gate that proves nothing, and this stage is what keeps that
  # honest — it fails if any of them starts passing.
  local failed=0
  local -a checks=(
    "architecture:$PYTHON tools/check_architecture.py --root tests/unit/fixtures/arch_violation --rules tests/unit/fixtures/arch_violation/configs/architecture_rules.yaml"
    "claims:$PYTHON tools/check_claims.py --root tests/unit/fixtures/claims_bad --policy tests/unit/fixtures/claims_bad/claims_policy.yaml"
    "secrets:$PYTHON tools/scan_secrets.py --path tests/unit/fixtures/secrets_bad"
    "governance:$PYTHON tools/governance/authoritative_check.py --trusted-root tests/unit/fixtures/governance_tampered/trusted --candidate-dir tests/unit/fixtures/governance_tampered/candidate --changed-files-from tests/unit/fixtures/governance_tampered/changed_files.txt"
  )
  for entry in "${checks[@]}"; do
    local label="${entry%%:*}"
    local command="${entry#*:}"
    if eval "$command" >/dev/null 2>&1; then
      echo "NEGATIVE GATE BROKEN: the $label checker accepted its counter-example fixture"
      failed=1
    else
      echo "OK: the $label checker rejects its counter-example fixture"
    fi
  done
  # The determinism harness has its own inversion flag.
  "$PYTHON" tools/determinism_check.py --producer nondeterministic --expect-failure || failed=1
  return "$failed"
}

# --- Governance -------------------------------------------------------------
stage "requirement audit"        "$PYTHON" tools/audit_requirements.py
stage "milestone audit"          "$PYTHON" tools/audit_requirements.py --milestone M2
# Both of these are ADVISORY since R8 (ADR-0014): they live in the branch they
# judge. The authoritative verdict is the "Authoritative governance gate (R8)"
# job, which runs tools/governance/authoritative_check.py from protected main.
# scripts/governance_preflight.sh reproduces that verdict locally; it is not run
# here because it needs the network, and this matrix must work offline.
stage "frozen files (advisory)"  "$PYTHON" tools/check_frozen.py --base main
stage "scope guard (advisory)"   "$PYTHON" tools/check_scope.py --base main
stage "architecture"             "$PYTHON" tools/check_architecture.py
stage "claims audit"             "$PYTHON" tools/check_claims.py
stage "decision records"         "$PYTHON" tools/check_adrs.py --base main
stage "documentation"            "$PYTHON" tools/check_docs.py
stage "secret scan"              "$PYTHON" tools/scan_secrets.py --staged --history
stage "sample data"              "$PYTHON" tools/check_sample_data.py
stage "pack manifest"            "$PYTHON" tools/check_pack_manifest.py
stage "traceability drift"       traceability_is_current
stage "deferred register drift"  deferred_register_is_current

# --- C++ --------------------------------------------------------------------
stage "cpp debug"                cpp_preset debug
stage "cpp release"              cpp_preset release
stage "cpp asan-ubsan"           cpp_preset asan-ubsan
stage "cpp style"                bash scripts/check_cpp_style.sh

# --- Python -----------------------------------------------------------------
stage "ruff"                     "$VENV/bin/ruff" check .
stage "mypy"                     "$VENV/bin/mypy"
stage "test layers"              "$PYTHON" tools/run_test_layers.py --python "$PYTHON"

# --- Determinism and environment -------------------------------------------
stage "determinism"              "$PYTHON" tools/determinism_check.py --runs 2 --seed 42
stage "negative gates"           negative_gates
stage "environment"              bash scripts/check_environment.sh

# --- Summary ----------------------------------------------------------------
printf '\n\033[1m=== Summary ===\033[0m\n'
for index in "${!STAGE_NAMES[@]}"; do
  printf '%-28s %s\n' "${STAGE_NAMES[$index]}" "${STAGE_STATUS[$index]}"
done

if [ "$FAILURES" -ne 0 ]; then
  printf '\n\033[31m%d of %d stages failed.\033[0m\n' "$FAILURES" "${#STAGE_NAMES[@]}"
  exit 1
fi
printf '\n\033[32mAll %d stages passed.\033[0m\n' "${#STAGE_NAMES[@]}"
