#!/usr/bin/env bash
# scripts/dev/git-janitor.sh — automate the deterministic post-merge cleanup steps.
#
# Wraps the manual flow at agents/git-janitor.md § Standard cleanup loop into a
# single command. Refuses to act on uncommitted user work; refuses to
# force-push; refuses to delete branches that aren't merged on GitHub.
#
# Usage:
#   bash scripts/dev/git-janitor.sh --post-merge <pr-number>
#
# What it does (post-merge mode):
#   1. Verify clean working tree (no uncommitted modifications outside
#      .fetchcontent-* / build/*).
#   2. Verify the PR is MERGED on GitHub (refuse if not).
#   3. fetch + prune remotes.
#   4. Switch to develop, fast-forward to origin/develop.
#   5. Delete the local PR branch (after GitHub remote-delete on squash-merge).
#   6. Run `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
#      as the final regression gate.
#   7. Print a concise report.
#
# Exit codes:
#   0 — clean cleanup; dual-target build passed.
#   1 — refused to act (uncommitted work, PR not merged, build failed).
#   2 — usage error / required tool missing.

set -euo pipefail

usage() {
    echo "Usage: bash scripts/dev/git-janitor.sh --post-merge <pr-number>" >&2
    exit 2
}

[ "${1:-}" = "--post-merge" ] || usage
PR_NUMBER="${2:-}"
[ -n "$PR_NUMBER" ] || usage
[[ "$PR_NUMBER" =~ ^[0-9]+$ ]] || { echo "git-janitor: pr-number must be numeric (got: $PR_NUMBER)" >&2; exit 2; }

command -v gh >/dev/null 2>&1 || { echo "git-janitor: gh CLI required on PATH" >&2; exit 2; }
command -v cmake >/dev/null 2>&1 || { echo "git-janitor: cmake required on PATH" >&2; exit 2; }

# ---------- Step 1: clean working tree --------------------------------------
DIRTY="$(git status --porcelain | grep -Ev '^.. (\.fetchcontent-|build/)' || true)"
if [ -n "$DIRTY" ]; then
    echo "git-janitor: REFUSE — uncommitted work in tree:" >&2
    printf '%s\n' "$DIRTY" >&2
    echo "Commit or stash before running --post-merge cleanup." >&2
    exit 1
fi

# ---------- Step 2: verify PR is merged --------------------------------------
echo "[git-janitor] checking PR #${PR_NUMBER} state..."
PR_JSON="$(gh pr view "$PR_NUMBER" --json state,headRefName,mergedAt 2>/dev/null)" || {
    echo "git-janitor: gh pr view #${PR_NUMBER} failed; cannot verify merge state." >&2
    exit 1
}
PR_STATE="$(printf '%s' "$PR_JSON" | python -c 'import json,sys; d=json.load(sys.stdin); print(d.get("state",""))' 2>/dev/null || echo "")"
PR_BRANCH="$(printf '%s' "$PR_JSON" | python -c 'import json,sys; d=json.load(sys.stdin); print(d.get("headRefName",""))' 2>/dev/null || echo "")"
if [ "$PR_STATE" != "MERGED" ]; then
    echo "git-janitor: REFUSE — PR #${PR_NUMBER} state=${PR_STATE} (expected MERGED). Aborting." >&2
    exit 1
fi
echo "[git-janitor] PR #${PR_NUMBER} (${PR_BRANCH}) is MERGED on GitHub."

# ---------- Step 3: fetch + prune --------------------------------------------
echo "[git-janitor] fetching + pruning remotes..."
git fetch --all --prune

# ---------- Step 4: switch to develop, fast-forward --------------------------
CURRENT_BRANCH="$(git rev-parse --abbrev-ref HEAD)"
if [ "$CURRENT_BRANCH" != "develop" ]; then
    echo "[git-janitor] switching from ${CURRENT_BRANCH} to develop..."
    git checkout develop
fi
echo "[git-janitor] fast-forwarding develop to origin/develop..."
git merge --ff-only origin/develop || {
    echo "git-janitor: FAIL — develop is not FF-clean against origin/develop; resolve manually." >&2
    exit 1
}

# ---------- Step 5: delete the local PR branch -------------------------------
if [ -n "$PR_BRANCH" ] && git show-ref --quiet "refs/heads/$PR_BRANCH"; then
    echo "[git-janitor] deleting local branch ${PR_BRANCH}..."
    # Use -D since squash-merge leaves the local tip orphaned (not ancestor of develop tip).
    git branch -D "$PR_BRANCH" || {
        echo "git-janitor: WARN — failed to delete local branch ${PR_BRANCH} (worktree owns it?)" >&2
    }
else
    echo "[git-janitor] local branch ${PR_BRANCH:-<unknown>} already gone (or never existed locally)."
fi

# ---------- Step 6: dual-target regression build -----------------------------
echo "[git-janitor] running dual-target regression build..."
if ! cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12 2>&1 | tail -20; then
    echo "git-janitor: FAIL — dual-target build failed. Develop is broken." >&2
    exit 1
fi

# ---------- Step 7: report ---------------------------------------------------
echo
echo "[git-janitor] === cleanup complete ==="
echo "  PR:             #${PR_NUMBER} (MERGED)"
echo "  Branch:         ${PR_BRANCH} (local deleted, remote already cleaned)"
echo "  Develop:        $(git rev-parse --short HEAD) (== origin/develop)"
echo "  Build:          PASS (SmatchetStandalone + SmatchetCore_DX12)"
exit 0
