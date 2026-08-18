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
# Idempotent: a PUT REPLACES the full protection object; re-running converges.
#
# That replace is a trap, not a convenience: the endpoint is not a patch, so an
# optional protection flag omitted from the body is reset to its API default
# (false). Omission is a silent REMOVAL, and the script converges on whatever
# the body states — not on the live state. This bit an apply once already: the
# body used to omit `required_conversation_resolution`, which was true live, so
# running the script would have turned that gate off while ostensibly tightening
# `enforce_admins` (see docs/plans/branch-protection-config-completeness.md).
# Hence the contract: project.config.json § branch_protection is the COMPLETE
# desired state over the PUT BODY SURFACE, every flag it states is projected into
# the body below, and tests/bats/setup_branch_protection.bats fails if a config
# flag stops reaching the wire. Add a flag to the config -> add it to the
# projection below.
#
# "Complete over the PUT body surface" is narrower than "complete over the GET
# response", deliberately: `required_signatures` comes back from the GET but is
# NOT a PUT body parameter — GitHub owns it through its own sub-resource
# (POST/DELETE .../protection/required_signatures), so this script can neither
# set nor clear it and a PUT leaves it untouched. It is therefore absent from
# the config block on purpose. Do not read the enumeration below as covering
# every key the GET returns; it covers every key the PUT accepts.
#
# Completeness is enforced, not merely documented: every key the projection
# reads must be PRESENT in the config block. A `.get(key, False)` fallback would
# turn a deleted key into a silent `false` on the wire — the exact failure this
# script exists to prevent — and would also delete the bats completeness case's
# own assertion along with the key it asserted. Missing key -> exit 2.
#
# Exit codes:
#   0 — applied (or dry-run printed)
#   1 — gh not authenticated / API call failed
#   2 — argument / environment error (missing config block or config key, no gh)

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

# Config path is a test seam only ($SMATCHET_BP_CONFIG lets the bats suite drive
# a mutated copy without touching the tracked file); production always reads the
# repo's own project.config.json.
CFG="${SMATCHET_BP_CONFIG:-project.config.json}"

# Build the GitHub branch-protection PUT body from the config block.
BODY="$("$PY" - "$CFG" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1], encoding="utf-8"))
bp = cfg.get("branch_protection")
if not bp:
    sys.stderr.write("setup-branch-protection: no 'branch_protection' block in project.config.json\n")
    sys.exit(2)
# Optional protection flags, projected config -> wire. The endpoint replaces
# the whole object, so a flag missing from this projection is switched OFF on
# every apply regardless of its live value (see the header comment). Config key
# and wire key are identical today; the explicit lists keep the projection
# greppable and give the bats suite something to assert completeness against.
TOP_LEVEL_FLAGS = [
    "required_conversation_resolution",
    "required_linear_history",
    "allow_force_pushes",
    "allow_deletions",
    "block_creations",
    "lock_branch",
    "allow_fork_syncing",
]
# These live INSIDE required_pull_request_reviews on the wire, but are stated
# flat in the config block (matching `strict`, which is flat in config and
# nested under required_status_checks on the wire).
REVIEW_FLAGS = [
    "dismiss_stale_reviews",
    "require_code_owner_reviews",
    "require_last_push_approval",
]
# Every config key this projection reads. Presence is REQUIRED, never defaulted:
# a `.get(k, False)` would let a DELETED config key sail through as `false` and
# be applied as a real protection change on the next PUT — the same silent
# removal the whole "complete statement" contract exists to stop, just entered
# from the config side instead of the projection side. It would also gut the
# bats completeness case, which diffs the keys PRESENT in config against the
# body: delete a key and its own assertion leaves with it. The schema's nested
# `required` list carries this same set for the CI jsonschema validator; this
# check is the local, dependency-free backstop that runs on every apply.
REQUIRED_KEYS = [
    "branch",
    "required_contexts",
    "required_checks_app_id",
    "required_review_count",
    "enforce_admins",
    "strict",
] + TOP_LEVEL_FLAGS + REVIEW_FLAGS

missing = [k for k in REQUIRED_KEYS if k not in bp]
if missing:
    sys.stderr.write(
        "setup-branch-protection: project.config.json branch_protection is missing "
        "required key(s): " + ", ".join(missing) + "\n"
        "  Every PUT-body key must be stated explicitly. The endpoint REPLACES the\n"
        "  whole protection object, so an absent key is applied as its API default\n"
        "  (false) rather than left alone. Re-add the key with the value you want\n"
        "  live, or drop it from the projection in this script if it is retired.\n")
    sys.exit(2)

# Keep the reviews object (not null) so the dismiss_stale / code_owner /
# last-push knobs stay available; the approving-review COUNT comes from
# required_review_count (0 — docs/adr/0013-solo-no-required-review.md).
reviews = {"required_approving_review_count": int(bp["required_review_count"])}
for k in REVIEW_FLAGS:
    reviews[k] = bool(bp[k])

# Emit the `checks` form, NOT the deprecated `contexts` string array. They are
# not interchangeable on the wire: a check entry is {context, app_id}, and the
# contexts[] spelling carries no app id at all, so a PUT built from it leaves
# every required context unpinned — GitHub then decides per context who may
# report it, with no statement of intent recorded anywhere. That is a silent
# loosening of who can turn a required gate green, applied by a script whose
# whole job is to tighten protection.
#
# app_id has THREE distinct spellings on this endpoint, and they are not
# synonyms (they were once conflated in this comment — CodeRabbit, PR #2070):
#   * a positive id  — pin: only that app may satisfy the check
#                      (15368 = GitHub Actions, what this repo uses)
#   * key OMITTED    — GitHub AUTO-SELECTS the app that most recently provided
#                      that context; if no app ever set it, any source may
#   * app_id = -1    — the explicit "any app may set this status" spelling
# So an omitted key is auto-selection, NOT "any app": those differ the moment a
# second app starts reporting a context. required_checks_app_id models all
# three — a positive int pins, -1 sends the explicit any-app value, and null
# omits the key (deliberate auto-selection, spelled as an omission rather than
# a JSON null so the request body reads as the API documents it).
#
# All three wire states come from three CONFIG VALUES, never from the presence
# or absence of the config key: the key is in REQUIRED_KEYS above, so deleting
# it exits 2. It is deliberately NOT a fourth spelling of auto-selection —
# reading config absence as "omit the wire key" would reinstate the silent
# default this script exists to close, and a deleted line would then quietly
# change who may satisfy every required check with nothing to flag it.
APP_ID = bp["required_checks_app_id"]
checks = []
for ctx in bp["required_contexts"]:
    entry = {"context": ctx}
    if APP_ID is not None:
        entry["app_id"] = int(APP_ID)
    checks.append(entry)

body = {
    "required_status_checks": {
        "strict": bool(bp["strict"]),
        "checks": checks,
    },
    "enforce_admins": bool(bp["enforce_admins"]),
    "required_pull_request_reviews": reviews,
    "restrictions": None,
}
for k in TOP_LEVEL_FLAGS:
    body[k] = bool(bp[k])
print(json.dumps(body))
PY
)" || exit 2

# Subscript, not .get(…, default): the body build above already hard-failed on a
# missing key, so a default here could only mask a divergence between these two
# readers of the same block.
BRANCH="$("$PY" -c 'import json,sys;print(json.load(open(sys.argv[1],encoding="utf-8"))["branch_protection"]["branch"])' "$CFG")"

if [ "$DRY_RUN" -eq 1 ]; then
    echo "setup-branch-protection: would PUT to repos/${REPO}/branches/${BRANCH}/protection:"
    printf '%s\n' "$BODY" | "$PY" -m json.tool
    exit 0
fi

gh auth status >/dev/null 2>&1 || { echo "setup-branch-protection: gh not authenticated; run 'gh auth login' (needs repo-admin)" >&2; exit 1; }

RC="$("$PY" -c 'import json,sys;print(json.load(open(sys.argv[1],encoding="utf-8"))["branch_protection"]["required_review_count"])' "$CFG")"
if printf '%s' "$BODY" | gh api -X PUT "repos/${REPO}/branches/${BRANCH}/protection" --input - >/dev/null; then
    echo "setup-branch-protection: applied to ${REPO}@${BRANCH} (review_count=${RC})"
else
    echo "setup-branch-protection: PUT failed (need a repo-admin token?)" >&2
    exit 1
fi
