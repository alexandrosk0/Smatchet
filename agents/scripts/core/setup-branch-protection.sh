#!/usr/bin/env bash
# setup-branch-protection.sh — codify GitHub branch protection for the default
# branch so the live state is reproducible and can't drift silently.
#
# Sibling of setup-locks-ruleset.sh (which protects refs/locks/*); this one
# owns the branch-protection object. The desired state is read from the
# `branch_protection` block in project.config.json (config-as-code: a reused
# project rewrites the config, the script re-targets) — never hardcoded here.
#
# WHY review-count 0 (see docs/adr/0013-solo-no-required-review.md): on a solo
# repo GitHub forbids approving your own PR, so a `required_approving_review_count`
# of 1 is an unsatisfiable deadlock — every PR sits BLOCKED even with CI +
# CodeRabbit fully green. The harness merge model (merge-gates.sh: reviewDecision
# in {APPROVED, NONE} -> pass) already does NOT require a human approval; CR
# (hard-blocking) + the required CI contexts are the real gates. This removes a
# redundant, impossible-to-satisfy human-approval gate, NOT any correctness gate.
#
# Usage:
#   bash agents/scripts/core/setup-branch-protection.sh            # apply
#   bash agents/scripts/core/setup-branch-protection.sh --dry-run  # print body only
#
# Needs a repo-admin-scoped token (the protection API is admin-only).
#
# Idempotent: a PUT replaces the full protection object; re-running converges.
#
# Exit codes:
#   0 — applied (or dry-run printed)
#   1 — gh not authenticated / API call failed
#   2 — argument / environment error (missing config block, no gh)

set -euo pipefail

cd "$(dirname "$0")/../../.."

command -v gh >/dev/null 2>&1 || { echo "setup-branch-protection: gh CLI is required" >&2; exit 2; }

# Resolve the target repo dynamically (no hardcoded slug — core-scripts-bash-07).
# $REPO overrides (test seam); else derive via gh. Fail LOUD if unresolvable —
# this script mutates branch protection, so silently targeting the wrong repo is
# worse than not running.
# shellcheck source=agents/scripts/core/lib/resolve-repo.sh
. agents/scripts/core/lib/resolve-repo.sh
REPO="$(resolve_repo)" || { echo "setup-branch-protection: cannot resolve target repo (set REPO=owner/name, or run 'gh auth login' in this checkout)" >&2; exit 2; }
DRY_RUN=0
[ "${1:-}" = "--dry-run" ] && DRY_RUN=1

# Pick a working python (Windows Store-stub aware), mirroring project-config.sh.
PY=""
for c in python3 python py; do
    if command -v "$c" >/dev/null 2>&1 && "$c" --version >/dev/null 2>&1; then PY="$c"; break; fi
done
[ -n "$PY" ] || { echo "setup-branch-protection: no working python found" >&2; exit 2; }

# Build the GitHub branch-protection PUT body from the config block.
BODY="$("$PY" - <<'PY'
import json, sys
cfg = json.load(open("project.config.json", encoding="utf-8"))
bp = cfg.get("branch_protection")
if not bp:
    sys.stderr.write("setup-branch-protection: no 'branch_protection' block in project.config.json\n")
    sys.exit(2)
body = {
    "required_status_checks": {
        "strict": bool(bp.get("strict", False)),
        "contexts": bp.get("required_contexts", []),
    },
    "enforce_admins": bool(bp.get("enforce_admins", False)),
    # Keep the object (not null) so dismiss_stale / code_owner knobs stay
    # available; only the approving-review COUNT is set to the config value.
    "required_pull_request_reviews": {
        "required_approving_review_count": int(bp.get("required_review_count", 0)),
        "dismiss_stale_reviews": False,
        "require_code_owner_reviews": False,
    },
    "restrictions": None,
}
print(json.dumps(body))
PY
)" || exit 2

BRANCH="$("$PY" -c 'import json;print(json.load(open("project.config.json",encoding="utf-8"))["branch_protection"].get("branch","develop"))')"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "setup-branch-protection: would PUT to repos/${REPO}/branches/${BRANCH}/protection:"
    printf '%s\n' "$BODY" | "$PY" -m json.tool
    exit 0
fi

gh auth status >/dev/null 2>&1 || { echo "setup-branch-protection: gh not authenticated; run 'gh auth login' (needs repo-admin)" >&2; exit 1; }

RC="$("$PY" -c 'import json;print(json.load(open("project.config.json",encoding="utf-8"))["branch_protection"].get("required_review_count",0))')"
if printf '%s' "$BODY" | gh api -X PUT "repos/${REPO}/branches/${BRANCH}/protection" --input - >/dev/null; then
    echo "setup-branch-protection: applied to ${REPO}@${BRANCH} (review_count=${RC})"
else
    echo "setup-branch-protection: PUT failed (need a repo-admin token?)" >&2
    exit 1
fi
