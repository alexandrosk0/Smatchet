#!/usr/bin/env bash
# scripts/dev/p4-git-sync-check.sh — guard against git/p4 path mismatches.
#
# In the dual-VCS topology (SMATCHET_AGENT_VCS=p4), follow-up edits can drift
# out of sync between git working-tree state and p4 client state: files
# modified-in-git but never `p4 edit`-ed, or `p4 add`-ed files never staged
# in git. This script reports both directions in one shot so the orchestrator
# can correct before shipping.
#
# Usage:
#   bash scripts/dev/p4-git-sync-check.sh           # exit 0 if synced; exit 1 if mismatch
#   bash scripts/dev/p4-git-sync-check.sh --quiet   # silent on success
#
# Exit codes:
#   0 — git pending paths and p4 opened paths are aligned
#   1 — mismatch found (see stderr for the diff)
#   2 — p4 unreachable (informational; not a fatal blocker for the script)

set -uo pipefail

QUIET=0
if [ "${1:-}" = "--quiet" ] || [ "${1:-}" = "-q" ]; then
    QUIET=1
fi

if ! command -v p4 >/dev/null 2>&1; then
    echo "p4-git-sync-check: p4 CLI not on PATH; skipping (this is fine for git-only sessions)" >&2
    exit 2
fi

if ! p4 info >/dev/null 2>&1; then
    echo "p4-git-sync-check: p4 info failed; p4 server not reachable" >&2
    exit 2
fi

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
cd "$REPO_ROOT"

# Pending git paths (relative to repo root): modified + added + deleted +
# untracked (excluding ignored). Normalized to forward-slash for p4 compare.
git_pending="$(git status --porcelain | awk '{
    # Skip ignored / untracked-only-deeply lines — porcelain v1 already filters via .gitignore
    sub(/^...?/, "");
    print
}' | sed 's|\\|/|g' | sort -u)"

# p4 opened — relative to client root. `p4 opened` lists `//depot/path#rev - action change ...`.
# Convert depot paths to local paths via `p4 -ztag opened` or simpler: use `p4 where`.
# Cheaper: list opened, extract depot path, run `p4 where` to convert.
p4_opened_depot="$(p4 opened 2>/dev/null | sed -n 's|^\(//[^#]*\)#.*|\1|p' | sort -u)"

if [ -z "$p4_opened_depot" ] && [ -z "$git_pending" ]; then
    [ "$QUIET" -eq 1 ] || echo "p4-git-sync-check: both git and p4 are clean (no pending paths)"
    exit 0
fi

# Convert opened depot paths to repo-relative local paths via `p4 where`.
p4_opened_local=""
if [ -n "$p4_opened_depot" ]; then
    p4_opened_local="$(printf '%s\n' "$p4_opened_depot" | while read -r dpath; do
        [ -z "$dpath" ] && continue
        # `p4 where //depot/...` returns "depot client local" — last field is the local abs path.
        local_path="$(p4 where "$dpath" 2>/dev/null | awk 'END { print $NF }')"
        if [ -n "$local_path" ]; then
            # Don't gate on `-e "$local_path"` — `p4 opened` includes deleted /
            # opened-for-delete files whose client copy is absent by design.
            # Gate on "is it inside the repo root" instead so we emit those too.
            case "$local_path" in
                "$REPO_ROOT"/*)
                    rel="${local_path#"$REPO_ROOT/"}"
                    printf '%s\n' "$rel" | sed 's|\\|/|g'
                    ;;
            esac
        fi
    done | sort -u)"
fi

# Diff the two sets.
mismatch=0
git_not_p4="$(comm -23 <(printf '%s\n' "$git_pending") <(printf '%s\n' "$p4_opened_local") | sed '/^$/d' || true)"
p4_not_git="$(comm -13 <(printf '%s\n' "$git_pending") <(printf '%s\n' "$p4_opened_local") | sed '/^$/d' || true)"

if [ -n "$git_not_p4" ]; then
    mismatch=1
    {
        echo "p4-git-sync-check: paths pending in git but NOT opened in p4:"
        # Read line-by-line so paths containing spaces / glob chars aren't
        # split or expanded by the shell when printf iterates.
        while IFS= read -r p; do printf '  - %s\n' "$p"; done <<< "$git_not_p4"
        echo "  -> Run \`p4 edit <path>\` (or \`p4 add\` for new files) before shipping."
    } >&2
fi
if [ -n "$p4_not_git" ]; then
    mismatch=1
    {
        echo "p4-git-sync-check: paths opened in p4 but NOT pending in git:"
        while IFS= read -r p; do printf '  - %s\n' "$p"; done <<< "$p4_not_git"
        echo "  -> Stage the change in git (\`git add <path>\`), revert the p4 op (\`p4 revert\`), or both."
    } >&2
fi

if [ "$mismatch" -eq 0 ]; then
    [ "$QUIET" -eq 1 ] || echo "p4-git-sync-check: git pending paths align with p4 opened paths"
    exit 0
fi

exit 1
