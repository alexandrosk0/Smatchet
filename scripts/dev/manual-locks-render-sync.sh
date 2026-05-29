#!/usr/bin/env bash
# manual-locks-render-sync.sh — hand-fire the locks-render sync when the
# `.github/workflows/locks-render.yml` workflow can't open its PR (typically
# because the `LOCK_RENDER_PAT` secret isn't configured yet — see
# docs/plans/shipped/git-ref-plan-locks.md § Operational requirements).
#
# What it does:
#   1. Fetches develop tip + refreshes refs/locks/*.
#   2. Regenerates docs/plans/active/_plan-locks.generated.md from live refs.
#   3. If the regen differs from develop's tracked file:
#        a. Creates / force-pushes branch `bot/plan-locks-sync` rooted at
#           develop tip with the regenerated file as a single new commit.
#        b. Opens a PR against develop (or no-ops if one is already open).
#        c. Squash-merges the PR and deletes the remote branch.
#   4. If no diff: no-op.
#
# Safe to re-run. Idempotent.
#
# Usage:
#   bash scripts/dev/manual-locks-render-sync.sh
#
# Optional environment:
#   LOCK_REMOTE   — git remote name (default: origin)
#   DRY_RUN=1     — render + diff only; skip branch push / PR / merge.
#
# Exit codes:
#   0 — synced (or no-op)
#   1 — gh / git command failed
#   2 — repo state / argument error

set -euo pipefail

remote="${LOCK_REMOTE:-origin}"
dry_run="${DRY_RUN:-0}"

# Repo sanity.
git rev-parse --show-toplevel >/dev/null 2>&1 || { echo "manual-locks-render-sync: not inside a git checkout" >&2; exit 2; }
command -v gh >/dev/null 2>&1 || { echo "manual-locks-render-sync: gh CLI is required" >&2; exit 2; }

repo_root=$(git rev-parse --show-toplevel)
script_dir="${repo_root}/scripts/dev"
target_file="${repo_root}/docs/plans/active/_plan-locks.generated.md"
sync_branch="bot/plan-locks-sync"
base_branch="develop"

for s in "${script_dir}/locks-render-markdown.sh" "${script_dir}/_lock-json.py"; do
    [ -f "$s" ] || { echo "manual-locks-render-sync: missing $s" >&2; exit 2; }
done

# Refuse to operate on a dirty working tree — we'll be checking out a fresh
# branch and rewriting the target file.
if ! git diff --quiet || ! git diff --quiet --cached; then
    echo "manual-locks-render-sync: working tree has uncommitted changes; refusing" >&2
    git status --short >&2
    exit 2
fi

# Stash whichever branch we came from so we can return cleanly after.
prev_branch=$(git symbolic-ref --quiet --short HEAD || echo "")

cleanup() {
    if [ -n "$prev_branch" ] && [ "$(git symbolic-ref --quiet --short HEAD || echo)" != "$prev_branch" ]; then
        git checkout --quiet "$prev_branch" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# 1. Fetch develop tip + refs/locks/* + the sync branch itself (the last
#    is required so the `--force-with-lease` check at step 5 has a real
#    remote tip to compare against — without it the lease falls back to a
#    missing-tracking-ref state that's effectively the same as `--force`).
git fetch --quiet "$remote" "$base_branch"
git fetch --quiet --prune "$remote" '+refs/locks/*:refs/locks/*'
git fetch --quiet "$remote" "$sync_branch" 2>/dev/null || true

# 2. Move onto a fresh branch rooted at develop tip.
git checkout --quiet -B "$sync_branch" "${remote}/${base_branch}"

# 3. Regenerate the file.
bash "${script_dir}/locks-render-markdown.sh" > "$target_file"

# 4. Decide whether to ship.
if git diff --quiet -- "$target_file"; then
    echo "manual-locks-render-sync: regen matches develop's tracked file; nothing to sync."
    # Clean up the local branch (already-equal state on remote is fine).
    git checkout --quiet "${prev_branch:-${remote}/${base_branch}}"
    git branch -D "$sync_branch" 2>/dev/null || true
    exit 0
fi

echo "manual-locks-render-sync: diff detected; preparing sync commit + PR."
git diff --stat -- "$target_file"

if [ "$dry_run" = "1" ]; then
    echo "manual-locks-render-sync: DRY_RUN=1 — skipping push / PR / merge."
    exit 0
fi

# 5. Commit + force-push.
git add "$target_file"
git commit -m "chore(plan-locks): regenerate _plan-locks.generated.md from refs/locks/*

Manual sync via scripts/dev/manual-locks-render-sync.sh — invoked when
the locks-render.yml workflow cannot open its PR (typically because
LOCK_RENDER_PAT is not configured). See
docs/plans/shipped/git-ref-plan-locks.md § Operational requirements."

git push --force-with-lease --quiet -u "$remote" "$sync_branch"

# 6. Open PR if not already open.
existing=$(gh pr list --head "$sync_branch" --base "$base_branch" --state open --json number --jq '.[].number' | head -n1 || true)
if [ -z "$existing" ]; then
    gh pr create \
        --base "$base_branch" \
        --head "$sync_branch" \
        --title "chore(plan-locks): regenerate _plan-locks.generated.md from refs/locks/*" \
        --body "Manual sync via \`scripts/dev/manual-locks-render-sync.sh\`. Runs when the \`locks-render.yml\` workflow can't open its own PR (typically because \`LOCK_RENDER_PAT\` is not configured — see [Operational requirements](../blob/develop/docs/plans/shipped/git-ref-plan-locks.md#operational-requirements))." \
        >/dev/null
    pr_number=$(gh pr list --head "$sync_branch" --base "$base_branch" --state open --json number --jq '.[].number' | head -n1)
    echo "manual-locks-render-sync: opened PR #${pr_number}."
else
    pr_number="$existing"
    echo "manual-locks-render-sync: existing PR #${pr_number} updated via force-push."
fi

# 7. Merge + delete branch.
gh pr merge "$pr_number" --squash --delete-branch >/dev/null 2>&1 || {
    echo "manual-locks-render-sync: gh pr merge non-zero (likely the local-worktree post-checkout quirk on Windows). Checking PR state directly..." >&2
    sleep 4
    state=$(gh pr view "$pr_number" --json state --jq '.state')
    if [ "$state" != "MERGED" ]; then
        echo "manual-locks-render-sync: PR #${pr_number} is $state; manual merge needed" >&2
        exit 1
    fi
    echo "manual-locks-render-sync: PR #${pr_number} merged (post-checkout quirk swallowed)."
    # gh's --delete-branch may not have run if its post-merge step failed; ensure cleanup.
    git push --quiet "$remote" --delete "$sync_branch" 2>/dev/null || true
}

echo "manual-locks-render-sync: synced + merged + cleaned. refs/locks/* and develop's tracked file are now consistent."
