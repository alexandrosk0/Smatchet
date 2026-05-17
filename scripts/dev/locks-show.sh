#!/usr/bin/env bash
# locks-show.sh — render the current plan-lock state as a stdout table.
#
# Discovers all `refs/locks/*` on `origin`, fetches each into the local
# `refs/locks/*` namespace (pruning stale entries), reads the `claim.json`
# blob on each, and prints a human-readable table.
#
# Usage:
#   bash scripts/dev/locks-show.sh
#
# Output columns:
#   slug | owner | branch | started | paths | first-path
#
# Optional environment:
#   LOCK_REMOTE                       — git remote name (default: origin)
#   SMATCHET_LOCK_BYPASS_REPO_CHECK   — set to 1 to skip Smatchet repo check
#   LOCKS_SHOW_FORMAT                 — "table" (default) or "json"
#
# Exit codes:
#   0 — rendered successfully (zero locks is still success)
#   2 — argument / environment / repo-state error

set -euo pipefail

remote="${LOCK_REMOTE:-origin}"
remote_url=$(git config --get "remote.${remote}.url" || true)
[ -n "$remote_url" ] || { echo "locks-show: remote '$remote' not configured" >&2; exit 2; }
if [ "${SMATCHET_LOCK_BYPASS_REPO_CHECK:-0}" != "1" ]; then
    case "$remote_url" in
        *[Ss]matchet*) : ;;
        *) echo "locks-show: remote URL does not look like a Smatchet repo" >&2; exit 2 ;;
    esac
fi
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
[ -n "$PYBIN" ] || { echo "locks-show: python3 (or python) is required" >&2; exit 2; }
HELPER="$(dirname "$0")/_lock-json.py"
[ -f "$HELPER" ] || { echo "locks-show: helper not found at $HELPER" >&2; exit 2; }

format="${LOCKS_SHOW_FORMAT:-table}"

# Refresh local refs/locks/* from remote (prune stale).
git fetch --quiet --prune "$remote" '+refs/locks/*:refs/locks/*' 2>/dev/null || true

# Enumerate local refs after fetch.
refs=$(git for-each-ref --format='%(refname)' refs/locks/ 2>/dev/null || true)

if [ -z "$refs" ]; then
    case "$format" in
        json) echo "[]" ;;
        *)    echo "(no plan-locks held on $remote)" ;;
    esac
    exit 0
fi

# Collect claim records into a single JSON array via the helper.
tmp_array=$(mktemp)
trap 'rm -f "$tmp_array"' EXIT
{
    printf '['
    sep=''
    for ref in $refs; do
        slug=${ref#refs/locks/}
        claim=$(git cat-file blob "${ref}:claim.json" 2>/dev/null || echo '{}')
        # Inject _slug into each record by piping through a tiny Python merge.
        merged=$(printf '%s' "$claim" | SLUG="$slug" "$PYBIN" -c '
import json, os, sys
obj = json.load(sys.stdin)
obj["_slug"] = os.environ["SLUG"]
json.dump(obj, sys.stdout, separators=(",",":"))
')
        printf '%s%s' "$sep" "$merged"
        sep=','
    done
    printf ']'
} >"$tmp_array"

case "$format" in
    json)
        "$PYBIN" "$HELPER" format-json <"$tmp_array"
        ;;
    table)
        "$PYBIN" "$HELPER" format-table <"$tmp_array"
        ;;
    *)
        echo "locks-show: unknown LOCKS_SHOW_FORMAT '$format' (expected table|json)" >&2
        exit 2
        ;;
esac
