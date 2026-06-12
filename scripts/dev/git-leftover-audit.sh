#!/usr/bin/env bash
# git-leftover-audit.sh — READ-ONLY map of local branches + worktrees vs
# origin/develop, classified by GitHub PR state. Answers "what's parked /
# removable / might be lost in git?" deterministically, replacing the ad-hoc
# for-each-ref + rev-list + gh-pr-view loops that historically mis-derived the
# residue map off a stale local develop (tooling self-improvement 2026-05-30 P2).
#
# NEVER mutates — no checkout / pull / branch -d / worktree remove. Pair with
# scripts/dev/worktree-prune.sh, which ACTS on the reapable subset (guarded).
#
# Classification per branch (keyed on the branch's GitHub PR state):
#   MERGED  — PR merged → branch + any worktree are RESIDUE (reap)
#   CLOSED  — PR closed unmerged → residue (review before discard)
#   OPEN    — PR open → ACTIVE (keep)
#   NO-PR   — no PR found → ahead of develop = WIP/orphan (keep+review);
#             not ahead = STALE (reap candidate)
#
# One `gh pr list` call builds the headRefName→state map (not one call/branch).
#
# Usage:
#   bash scripts/dev/git-leftover-audit.sh [--no-fetch]
#   bash scripts/dev/git-leftover-audit.sh --selftest
#
# Exit: 0 — report emitted (READ-ONLY never fails the caller) · 2 — infra error
#       (not in a git work tree). --selftest: 0 pass / 1 fail.
#
# selftest: asserts-failure

set -uo pipefail

# Pure classifier: (pr_state, ahead_of_develop) -> bucket label. pr_state is the
# GitHub state ("MERGED"/"CLOSED"/"OPEN") or "" when no PR exists. ahead is "1"
# when the branch has commits not on origin/develop, else "0".
classify_leftover() {
    local pr_state="$1" ahead="$2"
    case "$pr_state" in
        MERGED) echo "RESIDUE-merged" ;;
        CLOSED) echo "RESIDUE-closed" ;;
        OPEN)   echo "ACTIVE-open" ;;
        "")     if [ "$ahead" = "1" ]; then echo "WIP-or-orphan"; else echo "STALE-no-pr"; fi ;;
        *)      echo "UNKNOWN-$pr_state" ;;
    esac
}

if [ "${1:-}" = "--selftest" ]; then
    fail=0
    _ck() { # _ck <want> <state> <ahead>
        local got; got="$(classify_leftover "$2" "$3")"
        if [ "$got" = "$1" ]; then echo "  ok   [$1] state='$2' ahead=$3"
        else echo "  FAIL [want $1, got $got] state='$2' ahead=$3"; fail=1; fi
    }
    echo "git-leftover-audit --selftest:"
    _ck RESIDUE-merged MERGED 0
    _ck RESIDUE-closed CLOSED 1
    _ck ACTIVE-open    OPEN   1
    _ck WIP-or-orphan  ""     1
    _ck STALE-no-pr    ""     0
    # asserts-failure: a merged PR must NEVER classify as ACTIVE (the bug that
    # would keep reapable residue alive). Prove the classifier rejects it.
    if [ "$(classify_leftover MERGED 0)" = "ACTIVE-open" ]; then
        echo "  FAIL merged PR misclassified as ACTIVE"; fail=1
    else
        echo "  ok   merged PR is never ACTIVE"
    fi
    [ "$fail" -eq 0 ] && { echo "git-leftover-audit --selftest: PASS"; exit 0; }
    echo "git-leftover-audit --selftest: FAIL"; exit 1
fi

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "git-leftover-audit: not inside a git work tree" >&2; exit 2
}

if [ "${1:-}" != "--no-fetch" ]; then
    git fetch --prune origin develop >/dev/null 2>&1 || \
        echo "git-leftover-audit: WARN — 'git fetch --prune' failed (offline?); report may be stale" >&2
fi

# headRefName -> state map via ONE gh call (degrade gracefully when gh absent).
declare -A PR_STATE
have_gh=0
if command -v gh >/dev/null 2>&1; then
    have_gh=1
    while IFS=$'\t' read -r ref state; do
        [ -n "$ref" ] || continue
        # Keep the most decisive state if a ref appears twice (OPEN wins over a
        # stale CLOSED of a reused branch name).
        if [ -z "${PR_STATE[$ref]:-}" ] || [ "$state" = "OPEN" ]; then
            PR_STATE["$ref"]="$state"
        fi
    done < <(gh pr list --state all --limit 1000 --json headRefName,state \
                --jq '.[] | [.headRefName, .state] | @tsv' 2>/dev/null || true)
else
    echo "git-leftover-audit: WARN — gh not on PATH; PR state unknown (NO-PR buckets only)" >&2
fi

printf '%-42s %-22s %-16s %s\n' "WORKTREE/BRANCH" "PR-STATE" "BUCKET" "NOTES"
printf '%-42s %-22s %-16s %s\n' "---------------" "--------" "------" "-----"

# Walk worktrees (porcelain) — path + branch + dirty flag.
wt_path=""; wt_branch=""
emit_wt() {
    [ -n "$wt_path" ] || return 0
    local branch="$wt_branch" path="$wt_path"
    local base; base="$(basename "$path")"
    case "$branch" in
        develop|main|"") printf '%-42s %-22s %-16s %s\n' "$base [$branch]" "-" "PROTECTED" "integration / detached — never reaped"; return 0 ;;
    esac
    local state="${PR_STATE[$branch]:-}"
    local ahead=0
    if git -C "$path" rev-parse --verify --quiet origin/develop >/dev/null 2>&1; then
        local n; n="$(git -C "$path" rev-list --count origin/develop.."$branch" 2>/dev/null || echo 0)"
        [ "${n:-0}" -gt 0 ] && ahead=1
    fi
    local bucket; bucket="$(classify_leftover "$state" "$ahead")"
    local notes=""
    if ! git -C "$path" diff --quiet 2>/dev/null || ! git -C "$path" diff --cached --quiet 2>/dev/null; then
        notes="DIRTY (uncommitted) — prune will skip"
    fi
    [ "$have_gh" -eq 0 ] && [ -z "$state" ] && notes="${notes:+$notes; }gh unavailable"
    printf '%-42s %-22s %-16s %s\n' "$base [$branch]" "${state:-none}" "$bucket" "$notes"
}
while IFS= read -r line; do
    case "$line" in
        "worktree "*) emit_wt; wt_path="${line#worktree }"; wt_branch="" ;;
        "branch refs/heads/"*) wt_branch="${line#branch refs/heads/}" ;;
        "detached") wt_branch="" ;;
    esac
done < <(git worktree list --porcelain 2>/dev/null)
emit_wt

echo
echo "RESIDUE-* / STALE-no-pr rows are reap candidates — run: bash scripts/dev/worktree-prune.sh --dry-run"
exit 0
