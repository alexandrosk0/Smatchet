#!/usr/bin/env bash
# lock-release-on-close.sh — release plan-locks by the claiming BRANCH recorded
# in each lock's claim.json, or release one named lock.
#
# Every `refs/locks/<slug>` commit carries a claim.json whose `branch` field
# names the branch the slice ships on (lock-claim.sh, LOCK_BRANCH). That pairing
# lives outside the PR body, so a close can release the PR's locks even when the
# body's `lock-slug:` line was never written, was left commented out, or was
# dropped by a later body rewrite — each of which orphaned a lock that then
# false-blocked the next overlapping PR (tooling backlog
# 2026-10-04-lock-release-on-close-keys-only-on-a-body-line,
# 2026-08-17-pr-body-rewrite-drops-lock-slug-marker).
#
# Usage:
#   bash agents/scripts/core/lock-release-on-close.sh --branch <head-ref>
#       Release every lock whose claim.json `.branch` equals <head-ref>.
#       Called by .github/workflows/lock-cleanup.yml on every PR close.
#   bash agents/scripts/core/lock-release-on-close.sh --slug <slug>
#       Release one lock by slug, whatever branch claimed it. Called by
#       .github/workflows/lock-release-dispatch.yml — the release path for a
#       lock whose branch never opened a PR (tooling backlog
#       2026-08-17-abandoned-lock-from-unpushed-branch-has-no-agent-release-path).
#
# Guards (--branch mode only; each one skips, never fails):
#   - same-repo head: when BASE_REPO is set, HEAD_REPO must equal it. A fork's
#     branch name can collide with one of ours.
#   - a bare `holds-lock:` line in PR_BODY skips the whole release: stacked
#     intermediates must leave the lock they share with the final cutover PR.
#   - an integration or protected branch is never matched: develop, main, and
#     project.config.json `vcs.protected_branches`. No PR head is an
#     integration branch, and locks claimed under one are the edit-hook claims
#     a worktree session makes from the main checkout.
#
# Both modes share one deletion path: log the claim.json, then delete the ref
# with `--force-with-lease=<ref>:<sha-that-was-read>`. A lock released and
# re-claimed between the read and the delete is left alone.
#
# Optional environment:
#   LOCK_REMOTE                       git remote holding refs/locks/* (default: origin)
#   SMATCHET_LOCK_BYPASS_REPO_CHECK   1 skips the remote-URL repo check (sandbox remotes)
#   PR_BODY                           closed PR's body (--branch mode; holds-lock guard)
#   HEAD_REPO / BASE_REPO             owner/name of the PR head repo and this repo
#
# Exit codes:
#   0 — released, nothing to release, or skipped by a guard
#   2 — argument / environment / repo-state error
#   3 — refs/locks/* could not be fetched, or a delete failed after retries

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SLUG_RE='^[a-z0-9][a-z0-9-]{0,63}$'

usage() {
    echo "usage: bash agents/scripts/core/lock-release-on-close.sh --branch <head-ref> | --slug <slug>" >&2
    exit 2
}

mode=""
target=""
while [ "$#" -gt 0 ]; do
    case "$1" in
        --branch) [ "$#" -ge 2 ] || usage; mode="branch"; target="$2"; shift 2 ;;
        --branch=*) mode="branch"; target="${1#--branch=}"; shift ;;
        --slug) [ "$#" -ge 2 ] || usage; mode="slug"; target="$2"; shift 2 ;;
        --slug=*) mode="slug"; target="${1#--slug=}"; shift ;;
        -h|--help) sed -n '2,47p' "$0"; exit 0 ;;
        *) usage ;;
    esac
done
if [ -z "$mode" ] || [ -z "$target" ]; then usage; fi

if [ "$mode" = "slug" ] && ! printf '%s' "$target" | grep -qE "$SLUG_RE"; then
    echo "lock-release-on-close: invalid slug '$target' — must match [a-z0-9][a-z0-9-]{0,63}" >&2
    exit 2
fi

git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "lock-release-on-close: not inside a git checkout" >&2; exit 2; }

# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$SCRIPT_DIR/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "lock-release-on-close: python3 (or python) is required" >&2; exit 2; }
HELPER="$SCRIPT_DIR/_lock-json.py"
[ -f "$HELPER" ] || { echo "lock-release-on-close: helper not found at $HELPER" >&2; exit 2; }

remote="${LOCK_REMOTE:-origin}"
remote_url=$(git config --get "remote.${remote}.url" || true)
[ -n "$remote_url" ] || { echo "lock-release-on-close: remote '$remote' not configured" >&2; exit 2; }
if [ "${SMATCHET_LOCK_BYPASS_REPO_CHECK:-0}" != "1" ]; then
    # Same remote-URL guard as lock-claim.sh / lock-release.sh: the expected repo
    # name comes from project.config.json (project.name), anchored at a path
    # boundary, so a stray remote never has its locks deleted.
    _proj_cfg="$(git rev-parse --show-toplevel 2>/dev/null)/project.config.json"
    _proj_name="$("$PY" -c 'import json,sys;print(json.load(open(sys.argv[1]))["project"]["name"])' "$_proj_cfg" 2>/dev/null || printf 'Smatchet')"
    _proj_lc="$(printf '%s' "$_proj_name" | tr '[:upper:]' '[:lower:]')"
    _url_lc="$(printf '%s' "$remote_url" | tr '[:upper:]' '[:lower:]')"
    case "$_url_lc" in
        *[/:]"$_proj_lc"*) : ;;
        *) echo "lock-release-on-close: remote URL '$remote_url' does not look like a $_proj_name repo (set SMATCHET_LOCK_BYPASS_REPO_CHECK=1 to override)" >&2; exit 2 ;;
    esac
fi

# _release <slug> <sha> — log the claim, then delete refs/locks/<slug> only if
# it still points at <sha>. rc 0 deleted / already moved on; rc 3 delete failed.
_release() {
    local slug="$1" sha="$2" ref attempt=0 backoff=1 out rc
    ref="refs/locks/${slug}"
    echo "::notice::Releasing ${ref} (was ${sha}). claim.json, kept here for re-creation:"
    git cat-file blob "${sha}:claim.json" 2>/dev/null || echo "(claim.json unreadable)"
    while : ; do
        attempt=$((attempt + 1))
        out=$(git push --porcelain --force-with-lease="${ref}:${sha}" "$remote" ":${ref}" 2>&1) && rc=0 || rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "::notice::Deleted ${ref}."
            return 0
        fi
        # A lease mismatch means the ref was released, or released and
        # re-claimed, after it was read. Either way this run's claim is gone.
        if [[ "$out" == *"stale info"* ]]; then
            echo "::notice::${ref} changed or vanished since it was read; left as is."
            return 0
        fi
        if [ "$attempt" -ge 3 ]; then
            echo "::error::Delete of ${ref} failed after ${attempt} attempts:"
            printf '%s\n' "$out"
            return 3
        fi
        sleep "$backoff"
        backoff=$((backoff * 2))
    done
}

if [ "$mode" = "slug" ]; then
    ref="refs/locks/${target}"
    if ! git fetch --quiet "$remote" "+${ref}:${ref}" 2>/dev/null; then
        if [ -z "$(git ls-remote "$remote" "$ref" 2>/dev/null)" ]; then
            echo "::notice::${ref} is not present on ${remote}; nothing to release."
            exit 0
        fi
        echo "::error::Could not fetch ${ref} from ${remote}."
        exit 3
    fi
    _release "$target" "$(git rev-parse "$ref")"
    exit $?
fi

# --- --branch mode ----------------------------------------------------------
if [ -n "${BASE_REPO:-}" ] && [ "${HEAD_REPO:-}" != "$BASE_REPO" ]; then
    echo "::notice::PR head repo '${HEAD_REPO:-<deleted>}' is not ${BASE_REPO}; a fork branch never owns this repo's locks. No branch-keyed release."
    exit 0
fi

# Here-string, not `printf | grep -q`: grep -q exits on the first match, and
# under pipefail the SIGPIPE'd printf would turn a match into "no match".
if grep -qE '^[[:space:]]*holds-lock:' <<<"${PR_BODY:-}"; then
    echo "::notice::PR body carries a 'holds-lock:' line (stacked intermediate); the shared lock stays for the final cutover PR. No branch-keyed release."
    exit 0
fi

# Best-effort, like plan-lock-gate.sh: an unreadable config still leaves the
# develop/main floor in place.
# shellcheck source=scripts/dev/project-config.sh
. "$SCRIPT_DIR/../../../scripts/dev/project-config.sh" 2>/dev/null || true
for protected in develop main ${PC_VCS_PROTECTED_BRANCHES:-}; do
    if [ "$target" = "$protected" ]; then
        echo "::notice::'${target}' is an integration/protected branch; locks claimed under it are never released by branch match."
        exit 0
    fi
done

if ! git fetch --quiet --prune "$remote" '+refs/locks/*:refs/locks/*'; then
    echo "::error::Could not fetch refs/locks/* from ${remote}; branch-keyed release did not run."
    exit 3
fi

released=0
failed=0
while read -r sha ref; do
    [ -n "$ref" ] || continue
    slug="${ref#refs/locks/}"
    branch="$(git cat-file blob "${sha}:claim.json" 2>/dev/null | "$PY" "$HELPER" read-field branch 2>/dev/null || true)"
    [ "$branch" = "$target" ] || continue
    if _release "$slug" "$sha"; then
        released=$((released + 1))
    else
        failed=$((failed + 1))
    fi
done <<EOF
$(git for-each-ref --format='%(objectname) %(refname)' refs/locks/)
EOF

echo "::notice::Branch-keyed release for '${target}': released=${released}, failed=${failed}."
[ "$failed" -eq 0 ] || exit 3
exit 0
