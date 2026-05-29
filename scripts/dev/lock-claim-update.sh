#!/usr/bin/env bash
# lock-claim-update.sh — update an existing plan-lock claim (e.g. scope growth).
#
# Builds a new commit whose parent is the current `refs/locks/<slug>` tip,
# replaces `claim.json` with refreshed metadata, and pushes with
# `--force-with-lease` so the push fails if another actor mutated the ref
# between fetch and push (atomic compare-and-swap on the existing-ref case).
#
# Usage:
#   bash scripts/dev/lock-claim-update.sh <slug> <write-set-file>
#
# Arguments identical to lock-claim.sh. New write_set fully replaces the
# previous one; updated_at is bumped, started_at carries over from the
# original claim.
#
# Optional environment: same as lock-claim.sh, plus
#   LOCK_RETRY_MAX   — max lease-conflict retries (default 3)
#
# Exit codes:
#   0 — update pushed successfully
#   1 — lock does not exist; use lock-claim.sh first
#   2 — argument / environment / repo-state error
#   3 — lease conflict after retries (someone else keeps updating the ref)
#   4 — transient network failure after retries

set -euo pipefail

# --- backend dispatch (Phase 4 of docs/design/archive/git-to-perforce-migration.md)
# `SMATCHET_LOCK_BACKEND=p4-counter` has no claim-update equivalent today:
# `lock-claim-p4.sh` ships claim + release only (CAS via `p4 counter`).
# Update-in-place against an existing counter is feasible but unscoped; the
# safe interim behaviour is to refuse loudly so the caller routes around
# (release + re-claim) instead of silently dropping write-set growth.
if [ "${SMATCHET_LOCK_BACKEND:-git-ref}" = "p4-counter" ]; then
    cat >&2 <<'EOF'
lock-claim-update: not supported in the p4-counter backend.

The Perforce backend (lock-claim-p4.sh + lock-release-p4.sh) ships claim
+ release only. To grow an existing lock's write-set, run release then a
fresh claim:

    bash scripts/dev/lock-release.sh <slug>
    bash scripts/dev/lock-claim.sh <slug> <new-write-set-file>

Backlog: implement lock-claim-update-p4.sh as a follow-up. Filed in
docs/self-improvement/categories/tooling.md.
EOF
    exit 2
fi

usage() {
    echo "usage: bash scripts/dev/lock-claim-update.sh <slug> <write-set-file>" >&2
    exit 2
}

[ "$#" -eq 2 ] || usage
slug="$1"
write_set_file="$2"

if ! printf '%s' "$slug" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "lock-claim-update: invalid slug '$slug'" >&2
    exit 2
fi
[ -f "$write_set_file" ] || { echo "lock-claim-update: write-set file not found: $write_set_file" >&2; exit 2; }

PYBIN="${PYBIN:-}"
if [ -z "$PYBIN" ]; then
    for candidate in python python3; do
        if command -v "$candidate" >/dev/null 2>&1 && \
           "$candidate" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)' 2>/dev/null; then
            PYBIN="$candidate"
            break
        fi
    done
fi
[ -n "$PYBIN" ] || { echo "lock-claim-update: python3 (or python) is required" >&2; exit 2; }
HELPER="$(dirname "$0")/_lock-json.py"
[ -f "$HELPER" ] || { echo "lock-claim-update: helper not found at $HELPER" >&2; exit 2; }

remote="${LOCK_REMOTE:-origin}"
remote_url=$(git config --get "remote.${remote}.url" || true)
[ -n "$remote_url" ] || { echo "lock-claim-update: remote '$remote' not configured" >&2; exit 2; }
if [ "${SMATCHET_LOCK_BYPASS_REPO_CHECK:-0}" != "1" ]; then
    case "$remote_url" in
        *[Ss]matchet*) : ;;
        *) echo "lock-claim-update: remote URL does not look like a Smatchet repo" >&2; exit 2 ;;
    esac
fi

ref="refs/locks/${slug}"
agent_id="${AGENT_ID:-orchestrator}"
branch="${LOCK_BRANCH:-$(git symbolic-ref --quiet --short HEAD || echo 'detached')}"
plan="${LOCK_PLAN:-}"
notes="${LOCK_NOTES:-}"
max_retries="${LOCK_RETRY_MAX:-3}"

attempt=0
backoff=1
while : ; do
    attempt=$((attempt + 1))

    # Fetch current ref state.
    current_sha=$(git ls-remote "$remote" "$ref" 2>/dev/null | awk '{print $1}')
    if [ -z "$current_sha" ]; then
        echo "lock-claim-update: $ref does not exist on $remote — use lock-claim.sh" >&2
        exit 1
    fi

    # Read previous claim to carry over started_at.
    git fetch --quiet "$remote" "${ref}:${ref}" 2>/dev/null || true
    prev_started=$(git cat-file blob "${current_sha}:claim.json" 2>/dev/null \
        | "$PYBIN" "$HELPER" read-field started 2>/dev/null || true)
    [ -n "$prev_started" ] || prev_started=$(date -u +%Y-%m-%dT%H:%M:%SZ)

    updated=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    claim_json=$(
        SLUG="$slug" \
        WS_FILE="$write_set_file" \
        OWNER="$agent_id" \
        BRANCH="$branch" \
        PLAN="$plan" \
        STARTED="$prev_started" \
        UPDATED="$updated" \
        NOTES="$notes" \
        "$PYBIN" "$HELPER" build-claim
    )

    blob_sha=$(printf '%s\n' "$claim_json" | git hash-object -w --stdin)
    tree_sha=$(printf '%s\n' "100644 blob $blob_sha	claim.json" | git mktree)
    commit_sha=$(echo "lock: update $slug" | git commit-tree "$tree_sha" -p "$current_sha")

    # Push with --force-with-lease to enforce CAS.
    push_output=$(git push --force-with-lease="${ref}:${current_sha}" "$remote" "${commit_sha}:${ref}" 2>&1) && rc=0 || rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "$ref updated to $commit_sha (was $current_sha)"
        exit 0
    fi

    # Lease conflict — refetch and retry.
    case "$push_output" in
        *"stale info"*|*"non-fast-forward"*|*"rejected"*)
            if [ "$attempt" -ge "$max_retries" ]; then
                echo "lock-claim-update: lease conflict after $attempt attempts; refusing to keep racing" >&2
                printf '%s\n' "$push_output" >&2
                exit 3
            fi
            sleep "$backoff"
            backoff=$((backoff * 2))
            continue
            ;;
    esac

    if [ "$attempt" -ge "$max_retries" ]; then
        echo "lock-claim-update: transient push failure after $attempt attempts:" >&2
        printf '%s\n' "$push_output" >&2
        exit 4
    fi
    sleep "$backoff"
    backoff=$((backoff * 2))
done
