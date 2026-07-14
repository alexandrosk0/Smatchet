#!/usr/bin/env bash
# setup-locks-ruleset.sh — one-time setup of the GitHub Ruleset that
# protects `refs/locks/*` from unauthorised creation, update, and deletion.
#
# Phase 7b of docs/plans/shipped/git-ref-plan-locks.md. The script is opt-in —
# the ruleset is NOT auto-applied by any workflow. Run this once after
# Phase 7b's PR merges and after you've decided you want the hardening.
#
# What the ruleset enforces:
#
#   - Target ref pattern: `refs/locks/*` (every plan-lock ref).
#   - Rules: restrict-creations + restrict-updates + restrict-deletions.
#     A push that creates, fast-forwards, or deletes a ref under
#     `refs/locks/*` is rejected unless the actor is on the bypass list.
#   - Bypass list: `RepositoryRole/admin` (you, the repo owner) and
#     `Integration/github-actions` (so lock-cleanup.yml can still delete
#     refs on PR-merge via the default GITHUB_TOKEN identity).
#
# Effect: random push-rights actors (including any future collaborator
# you grant push without giving admin) can no longer mutate plan-locks.
# Only repo admins + the cleanup-action bot can.
#
# Trade-off: a contributor with plain push access can no longer claim a
# plan-lock from their own clone via `bash agents/scripts/core/lock-claim.sh`.
# This is intentional — if that's the workflow you want, leave the
# ruleset off and rely on social convention. The ruleset is for "I want
# the lock system to be hard to bypass" stance.
#
# Usage:
#   bash agents/scripts/core/setup-locks-ruleset.sh
#
# Idempotent: if a ruleset named "plan-locks" already exists on the repo,
# the script updates it in place. Run again after editing this file to
# pick up changes.
#
# Exit codes:
#   0 — ruleset created or updated
#   1 — gh not authenticated, repo not Smatchet, or API call failed
#   2 — argument / environment error

set -euo pipefail

RULESET_NAME="${RULESET_NAME:-plan-locks}"

command -v gh >/dev/null 2>&1 || { echo "setup-locks-ruleset: gh CLI is required" >&2; exit 2; }
gh auth status >/dev/null 2>&1 || { echo "setup-locks-ruleset: gh CLI not authenticated; run 'gh auth login' first" >&2; exit 1; }

# Resolve the target repo dynamically (no hardcoded slug — core-scripts-bash-07).
# $REPO overrides (test seam); else derive via gh. Fail LOUD if unresolvable —
# this script creates/updates a ref ruleset, so a wrong repo is worse than a no-op.
# shellcheck source=agents/scripts/core/lib/resolve-repo.sh
. "$(dirname "$0")/lib/resolve-repo.sh"
REPO="$(resolve_repo)" || { echo "setup-locks-ruleset: cannot resolve target repo (set REPO=owner/name, or run 'gh auth login' in this checkout)" >&2; exit 2; }

# Build the ruleset body. Schema reference:
# https://docs.github.com/en/rest/repos/rules
#
# The bypass `actor_type` enum accepts: RepositoryRole, Team, Integration,
# OrganizationAdmin, DeployKey. We use:
#   - RepositoryRole id 5 = "Admin" (the role the repo owner has)
#   - Integration id of GitHub Actions = 15368 (the well-known global ID
#     for the github-actions[bot] identity). Adjust if GitHub changes it.
#
# `bypass_mode: always` means the bypass applies on every push; `pull_request`
# means bypass only when the push is part of a merged PR. We use `always`
# for both entries because the cleanup action runs outside of a PR context
# (it runs on the `pull_request: closed` event, which is post-merge).

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT

cat >"$tmp" <<JSON
{
  "name": "${RULESET_NAME}",
  "target": "branch",
  "enforcement": "active",
  "conditions": {
    "ref_name": {
      "include": ["refs/locks/*"],
      "exclude": []
    }
  },
  "rules": [
    { "type": "creation" },
    { "type": "update" },
    { "type": "deletion" }
  ],
  "bypass_actors": [
    {
      "actor_id": 5,
      "actor_type": "RepositoryRole",
      "bypass_mode": "always"
    },
    {
      "actor_id": 15368,
      "actor_type": "Integration",
      "bypass_mode": "always"
    }
  ]
}
JSON

# Discover any existing ruleset with the same name.
existing_id=$(gh api "repos/${REPO}/rulesets" --jq ".[] | select(.name == \"${RULESET_NAME}\") | .id" 2>/dev/null | head -n1 || true)

if [ -n "$existing_id" ]; then
    echo "Updating existing ruleset '${RULESET_NAME}' (id=${existing_id}) on ${REPO}."
    gh api -X PUT "repos/${REPO}/rulesets/${existing_id}" --input "$tmp"
else
    echo "Creating new ruleset '${RULESET_NAME}' on ${REPO}."
    gh api -X POST "repos/${REPO}/rulesets" --input "$tmp"
fi

echo
echo "Done. Verify with:"
echo "  gh api repos/${REPO}/rulesets --jq '.[] | select(.name == \"${RULESET_NAME}\")'"
echo
echo "To roll back, delete via:"
echo "  gh api -X DELETE repos/${REPO}/rulesets/<id>"
