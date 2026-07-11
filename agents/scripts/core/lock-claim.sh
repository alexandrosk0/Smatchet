#!/usr/bin/env bash
# lock-claim.sh — claim a plan-lock atomically via a git ref.
#
# Creates a tiny commit holding `claim.json` and pushes it to
# `refs/locks/<slug>` on `origin`. Atomic compare-and-swap is provided by
# git's standard ref-update protocol: pushing to create a ref that already
# exists is rejected as non-fast-forward, so concurrent claims resolve
# deterministically (first push wins, others get a hard "lock held" error).
#
# Usage:
#   bash agents/scripts/core/lock-claim.sh <slug> <write-set-file>
#
# Arguments:
#   slug             — kebab-case identifier, [a-z0-9][a-z0-9-]{0,63}
#   write-set-file   — path to a file with one claimed repo-relative path per
#                      line. Blank lines and lines starting with '#' ignored.
#
# Optional environment:
#   AGENT_ID         — identity recorded in claim.json (default: orchestrator)
#   LOCK_BRANCH      — branch the slice ships on (default: current HEAD branch)
#   LOCK_PLAN        — originating plan path (default: empty)
#   LOCK_NOTES       — free-form one-line note (default: empty)
#   LOCK_REMOTE      — git remote name (default: origin)
#   SMATCHET_LOCK_BYPASS_REPO_CHECK — set to 1 to skip "is this Smatchet?" check
#                      (test harness uses this against a sandbox bare repo)
#
# Exit codes:
#   0 — claim pushed successfully
#   1 — lock already held; run locks-show.sh for owner
#   2 — argument / environment / repo-state error
#   3 — transient network/git failure after retries
#
# See docs/plans/shipped/git-ref-plan-locks.md § Phase 1.

set -euo pipefail

# --- backend dispatch (Phase 4 of docs/plans/shipped/git-to-perforce-migration.md)
# `SMATCHET_LOCK_BACKEND=p4-counter` defers to the Perforce-counter sibling
# script. Default backend stays git-ref (this script's body). Dispatch is at
# top-of-script so the git-ref impl below is unchanged from the original.
if [ "${SMATCHET_LOCK_BACKEND:-git-ref}" = "p4-counter" ]; then
    exec bash "$(dirname "$0")/lock-claim-p4.sh" "$@"
fi

usage() {
    echo "usage: bash agents/scripts/core/lock-claim.sh <slug> <write-set-file>" >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
slug="$1"
write_set_file="$2"

# Validate slug.
if ! printf '%s' "$slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "lock-claim: invalid slug '$slug' — must match [a-z0-9][a-z0-9-]{0,63}" >&2
    exit 2
fi

[ -f "$write_set_file" ] || { echo "lock-claim: write-set file not found: $write_set_file" >&2; exit 2; }

# Repo + remote sanity.
git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "lock-claim: not inside a git checkout" >&2; exit 2; }
remote="${LOCK_REMOTE:-origin}"
remote_url=$(git config --get "remote.${remote}.url" || true)
[ -n "$remote_url" ] || { echo "lock-claim: remote '$remote' not configured" >&2; exit 2; }
if [ "${SMATCHET_LOCK_BYPASS_REPO_CHECK:-0}" != "1" ]; then
    # Portability: derive the expected repo identifier from project.config.json
    # (project.name) instead of hardcoding "Smatchet", so the portable agents/
    # tree works in another project. Falls back to "Smatchet" when the config is
    # unreadable, so the guard is never weaker than before. Anchored at a path
    # boundary (`/` https, `:` scp-style) per H15; matched case-insensitively.
    _proj_cfg="$(git rev-parse --show-toplevel 2>/dev/null)/project.config.json"
    _proj_name="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["project"]["name"])' "$_proj_cfg" 2>/dev/null || python -c 'import json,sys;print(json.load(open(sys.argv[1]))["project"]["name"])' "$_proj_cfg" 2>/dev/null || printf 'Smatchet')"
    _proj_lc="$(printf '%s' "$_proj_name" | tr '[:upper:]' '[:lower:]')"
    _url_lc="$(printf '%s' "$remote_url" | tr '[:upper:]' '[:lower:]')"
    case "$_url_lc" in
        *[/:]"$_proj_lc"*) : ;;
        *) echo "lock-claim: remote URL '$remote_url' does not look like a $_proj_name repo (set SMATCHET_LOCK_BYPASS_REPO_CHECK=1 to override)" >&2; exit 2 ;;
    esac
fi

# Collect agent identity + metadata.
agent_id="${AGENT_ID:-orchestrator}"
branch="${LOCK_BRANCH:-$(git symbolic-ref --quiet --short HEAD || echo 'detached')}"
case "$branch" in
    develop|main)
        # A plan-lock owned by the integration branch is almost always a wrong-tree claim:
        # the pre-push guard then rejects the feature worktree's own push as a collision.
        echo "lock-claim: WARNING: lock owner branch resolves to '$branch' (integration branch) —" >&2
        echo "lock-claim:          you usually claim from the feature worktree; pass LOCK_BRANCH explicitly if this is unintended" >&2
        ;;
esac
plan="${LOCK_PLAN:-}"
notes="${LOCK_NOTES:-}"
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# Build claim.json via the Python helper.
PYBIN="${PYBIN:-}"
if [ -z "$PYBIN" ]; then
    # Probe order: python first (on Windows, python3 is often the Microsoft
    # Store stub that exits 49). Each candidate is sanity-checked by running
    # a tiny Python 3.x assertion so the MS-Store stub fails the probe.
    for candidate in python python3; do
        if command -v "$candidate" >/dev/null 2>&1 && \
           "$candidate" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PYBIN="$candidate"
            break
        fi
    done
fi
[ -n "$PYBIN" ] || { echo "lock-claim: python3 (or python) is required" >&2; exit 2; }
HELPER="$(dirname "$0")/_lock-json.py"
[ -f "$HELPER" ] || { echo "lock-claim: helper not found at $HELPER" >&2; exit 2; }

claim_json=$(
    SLUG="$slug" \
    WS_FILE="$write_set_file" \
    OWNER="$agent_id" \
    BRANCH="$branch" \
    PLAN="$plan" \
    STARTED="$started" \
    NOTES="$notes" \
    "$PYBIN" "$HELPER" build-claim
)

# Build the commit object: a tree containing claim.json, with no parent.
blob_sha=$(printf '%s\n' "$claim_json" | git hash-object -w --stdin)
tree_sha=$(printf '%s\n' "100644 blob $blob_sha	claim.json" | git mktree)
commit_sha=$(echo "lock: claim $slug" | git commit-tree "$tree_sha")

ref="refs/locks/${slug}"

# Push with retries — only retry transient errors. CAS rejection is terminal.
attempt=0
max_attempts=3
backoff=1
while : ; do
    attempt=$((attempt + 1))
    push_output=$(git push "$remote" "${commit_sha}:${ref}" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$ref claimed at $commit_sha (owner=$agent_id, branch=$branch)"
        exit 0
    fi

    # Classify the failure.
    case "$push_output" in
        *"non-fast-forward"*|*"already exists"*|*"cannot lock ref"*|*"failed to push some refs"*)
            # Likely lock held. Confirm by ls-remote.
            held=$(git ls-remote "$remote" "$ref" 2>/dev/null | awk '{print $1}')
            if [ -n "$held" ]; then
                echo "lock-claim: $slug already held at $held — run agents/scripts/core/locks-show.sh" >&2
                exit 1
            fi
            ;;
    esac

    # Transient.
    if [ "$attempt" -ge "$max_attempts" ]; then
        echo "lock-claim: push to $ref failed after $attempt attempts:" >&2
        printf '%s\n' "$push_output" >&2
        exit 3
    fi
    sleep "$backoff"
    backoff=$((backoff * 2))
done
