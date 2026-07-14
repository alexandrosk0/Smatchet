#!/usr/bin/env bash
# resolve-repo.sh — shared `owner/name` repo-slug resolver (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (core-scripts-bash-07)
#   Four core scripts each hardcoded the same fallback slug:
#       REPO="${REPO:-alexandrosk0/Smatchet}"
#   That literal is a portability trap — a forked / renamed / reused project
#   silently keeps targeting the ORIGINAL repo. Every `gh api repos/$REPO/…`
#   call then hits the wrong repository with no error (a PUT to another owner's
#   branch protection, a `gh pr list` against a stale slug). The correct source
#   of truth for "which repo is this checkout" is `gh` itself.
#
# CONTRACT — resolve_repo
#   Prints the resolved `owner/name` slug to stdout and returns 0, OR prints
#   nothing and returns non-zero when the slug cannot be resolved. It NEVER
#   invents a hardcoded fallback — that is the bug this replaces. Each caller
#   decides how to handle a non-zero return per its own degrade contract:
#     * a mutating fail-hard script (setup-*) exits non-zero (never mutate the
#       wrong repo),
#     * an advisory nudge (postmortem-/plan-archival-owed) WARNs + degrades
#       (it already exits 0 when gh is unavailable).
#
#   Resolution order:
#     1. $REPO set + non-empty  -> echo it verbatim (explicit override + the
#        bats test seam; honoured even to a fixture slug like "x/y").
#     2. gh available + authenticated -> `gh repo view --json nameWithOwner`.
#     3. otherwise -> return 1 (unresolvable; caller degrades / fails loud).
#
# This file is sourced; it defines one function and does NOT `set -e`
# (the sourcing script owns shell options).
# ----------------------------------------------------------------------------

# resolve_repo — echo the current repo slug (owner/name) or return non-zero.
resolve_repo() {
    # 1. Explicit override / test seam wins — verbatim, no gh call.
    if [ -n "${REPO:-}" ]; then
        printf '%s\n' "$REPO"
        return 0
    fi
    # 2. Derive from gh (the authoritative source for "this checkout's repo").
    if command -v gh >/dev/null 2>&1; then
        local slug
        if slug="$(gh repo view --json nameWithOwner --jq .nameWithOwner 2>/dev/null)" \
           && [ -n "$slug" ]; then
            printf '%s\n' "$slug"
            return 0
        fi
    fi
    # 3. Unresolvable — no hardcoded fallback (the whole point of this helper).
    return 1
}
