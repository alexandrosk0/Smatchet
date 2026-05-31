#!/usr/bin/env bash
# lock-release.sh — release a plan-lock by deleting its `refs/locks/<slug>`.
#
# Idempotent: if the ref does not exist on the remote, exits 0 with a notice.
# This is the local equivalent of the `lock-cleanup` GitHub Action (Phase 3)
# and is meant for abandoned branches or pre-cutover manual housekeeping.
# Routine release on PR merge will be handled by the action — agents should
# not call this in the happy path.
#
# Usage:
#   bash agents/scripts/core/lock-release.sh <slug>
#
# Optional environment:
#   LOCK_REMOTE                       — git remote name (default: origin)
#   SMATCHET_LOCK_BYPASS_REPO_CHECK   — set to 1 to skip Smatchet repo check
#
# Exit codes:
#   0 — ref deleted, or already absent
#   2 — argument / environment / repo-state error
#   3 — transient network/git failure after retries

set -euo pipefail

# --- backend dispatch (Phase 4 of docs/plans/shipped/git-to-perforce-migration.md)
if [ "${SMATCHET_LOCK_BACKEND:-git-ref}" = "p4-counter" ]; then
    exec bash "$(dirname "$0")/lock-release-p4.sh" "$@"
fi

usage() {
    echo "usage: bash agents/scripts/core/lock-release.sh <slug>" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
slug="$1"

if ! printf '%s' "$slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "lock-release: invalid slug '$slug'" >&2
    exit 2
fi

remote="${LOCK_REMOTE:-origin}"
remote_url=$(git config --get "remote.${remote}.url" || true)
[ -n "$remote_url" ] || { echo "lock-release: remote '$remote' not configured" >&2; exit 2; }
if [ "${SMATCHET_LOCK_BYPASS_REPO_CHECK:-0}" != "1" ]; then
    # H15: path-boundary anchor — matches `lock-claim.sh`'s tightened check.
    case "$remote_url" in
        *[/:][Ss]matchet*) : ;;
        *) echo "lock-release: remote URL does not look like a Smatchet repo" >&2; exit 2 ;;
    esac
fi

ref="refs/locks/${slug}"

# Check whether the ref exists; if not, declare success.
existing=$(git ls-remote "$remote" "$ref" 2>/dev/null | awk '{print $1}')
if [ -z "$existing" ]; then
    echo "lock-release: $ref already absent on $remote (no-op)"
    exit 0
fi

attempt=0
max_attempts=3
backoff=1
while : ; do
    attempt=$((attempt + 1))
    push_output=$(git push "$remote" ":$ref" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$ref deleted (was $existing)"
        exit 0
    fi

    # If the remote says ref already gone, treat as success.
    # H13: split the multi-substring case-glob into explicit alternatives
    # so a future error string that happens to carry both "unable to delete"
    # and "does not exist" out-of-context (e.g. a remote message naming a
    # *different* ref that does not exist) doesn't silently get swallowed.
    # The two real shapes:
    #   "remote ref does not exist"  → ref already deleted, no-op
    #   "unable to delete '<ref>': remote ref does not exist"
    #                                → same outcome, different phrasing
    if [[ "$push_output" == *"remote ref does not exist"* ]]; then
        echo "lock-release: $ref vanished mid-delete — treating as no-op"
        exit 0
    fi
    if [[ "$push_output" == *"unable to delete"* && "$push_output" == *"does not exist"* ]]; then
        echo "lock-release: $ref vanished mid-delete — treating as no-op"
        exit 0
    fi

    if [ "$attempt" -ge "$max_attempts" ]; then
        echo "lock-release: delete of $ref failed after $attempt attempts:" >&2
        printf '%s\n' "$push_output" >&2
        exit 3
    fi
    sleep "$backoff"
    backoff=$((backoff * 2))
done
