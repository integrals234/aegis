#!/usr/bin/env bash
# Mint a short-lived GitHub App installation access token (R8, ADR-0014).
#
# THIS IS THE DOCUMENTED COPY, committed so the credential setup is
# reproducible and reviewable. The OPERATIVE copy lives outside the repository
# at ~/.config/aegis/aegis-token.sh, alongside the App private key, so neither
# can ever be committed. Editing this file does not change how the agent
# authenticates.
#
# Why an App and not a token: `integrals234/aegis` is a personal-account
# repository, and GitHub's fine-grained personal access tokens cannot be used to
# contribute to a repository whose token owner is an outside collaborator on it
# — which is exactly what a machine-account collaborator would be. A GitHub App
# installation is the supported way to get a distinct, least-privilege identity
# on a personal repository.
#
# What this credential can and cannot do (verified by
# tools/governance/verify_credential_boundary.py):
#
#   CAN     push feature branches, open pull requests, read checks and logs,
#           read the ruleset
#   CANNOT  write rulesets, change Actions settings, reach admin endpoints,
#           approve any pull request
#
# It CAN call the merge endpoint — `Contents: Write` is required to push
# branches and is the same permission merging needs, so the two cannot be
# separated by permission. That is not the boundary. The boundary is the
# "Protect main" ruleset:
#
#     no commit enters `main` without a fresh approving review
#     from the separate owner identity
#
# enforced by required_approving_review_count=1, require_last_push_approval and
# dismiss_stale_reviews_on_push, with an empty bypass list — and by GitHub
# refusing to let the App approve its own pull request.
#
# Setup (owner, once):
#   1. Register a private GitHub App owned by the repository owner, installed
#      only on this repository, with exactly:
#        Contents: Write, Pull requests: Write, Workflows: Write,
#        Actions: Read, Metadata: Read, Administration: READ
#      Administration: Write must NOT be granted — that single setting is what
#      denies ruleset and Actions-settings writes.
#   2. Generate a private key and store it outside the repository, mode 0600.
#   3. Export AEGIS_APP_ID, AEGIS_APP_KEY and (optionally) AEGIS_INSTALLATION_ID.
#
# Usage:
#   mint_installation_token.sh              installation access token (secret)
#   mint_installation_token.sh --jwt        app JWT (secret, 9 minutes)
#   mint_installation_token.sh --json       full response INCLUDING the token
#   mint_installation_token.sh --describe   response with .token removed; safe to log
#   mint_installation_token.sh --installation   installation id
set -euo pipefail

APP_ID="${AEGIS_APP_ID:?set AEGIS_APP_ID}"
INSTALLATION_ID="${AEGIS_INSTALLATION_ID:-}"
KEY_FILE="${AEGIS_APP_KEY:?set AEGIS_APP_KEY to the private key path, outside the repository}"
API="https://api.github.com"

[[ -r "$KEY_FILE" ]] || { echo "mint: cannot read $KEY_FILE" >&2; exit 1; }

b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

mint_jwt() {
  local now header payload h p sig
  now=$(date +%s)
  header='{"alg":"RS256","typ":"JWT"}'
  # iat backdated 60s for clock skew; exp 9 min (GitHub rejects beyond 10).
  payload=$(printf '{"iat":%d,"exp":%d,"iss":"%s"}' "$((now - 60))" "$((now + 540))" "$APP_ID")
  h=$(printf '%s' "$header" | b64url)
  p=$(printf '%s' "$payload" | b64url)
  sig=$(printf '%s.%s' "$h" "$p" | openssl dgst -sha256 -sign "$KEY_FILE" -binary | b64url)
  printf '%s.%s.%s' "$h" "$p" "$sig"
}

installation_id() {
  if [[ -n "$INSTALLATION_ID" ]]; then
    printf '%s' "$INSTALLATION_ID"
    return
  fi
  curl -fsS -H "Authorization: Bearer $(mint_jwt)" \
       -H "Accept: application/vnd.github+json" \
       "$API/app/installations" \
    | python3 -c 'import json,sys; d=json.load(sys.stdin); print(d[0]["id"] if d else "", end="")'
}

token_response() {
  curl -fsS -X POST \
       -H "Authorization: Bearer $(mint_jwt)" \
       -H "Accept: application/vnd.github+json" \
       "$API/app/installations/$(installation_id)/access_tokens"
}

case "${1:-token}" in
  --jwt)          mint_jwt; echo ;;
  --installation) installation_id; echo ;;
  --json)         token_response ;;
  --describe)     token_response \
                    | python3 -c 'import json,sys; d=json.load(sys.stdin); d.pop("token",None); print(json.dumps(d,indent=2,sort_keys=True))' ;;
  token|"")       token_response \
                    | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"], end="")' ;;
  *) echo "mint: unknown argument: $1" >&2; exit 2 ;;
esac
