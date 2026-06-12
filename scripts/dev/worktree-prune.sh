#!/usr/bin/env bash
# worktree-prune.sh — reap worktrees whose branch's GitHub PR is MERGED. The
# acting companion to the read-only scripts/dev/git-leftover-audit.sh: it removes
# only the unambiguously-safe RESIDUE-merged subset, leaving CLOSED / OPEN / WIP /
# STALE worktrees for a human (git-janitor v5 § Stale-branch-sweep encoded this
# discipline in prose; this is the re-runnable script — tooling self-improvement
# 2026-05-18 / 2026-05-30).
#
# DRY-RUN BY DEFAULT — prints what it WOULD reap and exits 0. Pass --apply to
# actually `git worktree remove` + `git branch -D`.
#
# HARD GUARDS (a reap is performed ONLY when ALL hold):
#   * the branch's PR state is MERGED (one `gh pr list` call builds the map),
#   * the worktree is CLEAN (no uncommitted/ staged changes — dirty is skipped
#     and surfaced, never force-removed),
#   * the branch is NOT protected (develop / main) and the path is NOT the
#     current worktree nor the main integration tree.
#
# Usage:
#   bash scripts/dev/worktree-prune.sh            # dry-run (default)
#   bash scripts/dev/worktree-prune.sh --apply     # actually reap
#   bash scripts/dev/worktree-prune.sh --selftest
#
# Exit: 0 — ran (dry-run, or apply with all reaps OK) · 1 — an --apply reap
#       failed / --selftest failure · 2 — not in a git work tree.
#
# selftest: asserts-failure

set -uo pipefail

# Pure decision: (pr_state, dirty, protected) -> action. Keeps the guard logic
# unit-testable. dirty/protected are "1"/"0".
prune_decision() {
    local pr_state="$1" dirty="$2" protected="$3"
    [ "$protected" = "1" ] && { echo "SKIP-protected"; return 0; }
    [ "$pr_state" != "MERGED" ] && { echo "KEEP-$pr_state"; return 0; }
    [ "$dirty" = "1" ] && { echo "SKIP-dirty"; return 0; }
    echo "REAP"
}

if [ "${1:-}" = "--selftest" ]; then
    fail=0
    _ck() { local got; got="$(prune_decision "$2" "$3" "$4")"
        if [ "$got" = "$1" ]; then echo "  ok   [$1] state='$2' dirty=$3 prot=$4"
        else echo "  FAIL [want $1 got $got] state='$2' dirty=$3 prot=$4"; fail=1; fi; }
    echo "worktree-prune --selftest:"
    _ck REAP           MERGED 0 0
    _ck SKIP-dirty     MERGED 1 0
    _ck SKIP-protected MERGED 0 1
    _ck KEEP-OPEN      OPEN   0 0
    _ck KEEP-          ""     0 0
    # asserts-failure: a DIRTY merged worktree must NEVER be reaped (would lose
    # uncommitted work). Prove the guard holds.
    if [ "$(prune_decision MERGED 1 0)" = "REAP" ]; then
        echo "  FAIL dirty merged worktree was marked REAP"; fail=1
    else echo "  ok   dirty merged worktree is never REAP"; fi
    [ "$fail" -eq 0 ] && { echo "worktree-prune --selftest: PASS"; exit 0; }
    echo "worktree-prune --selftest: FAIL"; exit 1
fi

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "worktree-prune: not inside a git work tree" >&2; exit 2
}

APPLY=0
[ "${1:-}" = "--apply" ] && APPLY=1

main_tree="$(git worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2; exit}')"
self_tree="$(git rev-parse --show-toplevel 2>/dev/null)"

declare -A PR_STATE
if command -v gh >/dev/null 2>&1; then
    git fetch --prune origin develop >/dev/null 2>&1 || true
    while IFS=$'\t' read -r ref state; do
        [ -n "$ref" ] || continue
        if [ -z "${PR_STATE[$ref]:-}" ] || [ "$state" = "OPEN" ]; then PR_STATE["$ref"]="$state"; fi
    done < <(gh pr list --state all --limit 1000 --json headRefName,state \
                --jq '.[] | [.headRefName, .state] | @tsv' 2>/dev/null || true)
else
    echo "worktree-prune: gh not on PATH — cannot determine MERGED state; nothing to reap." >&2
    exit 0
fi

[ "$APPLY" -eq 1 ] && echo "worktree-prune: --apply (reaping MERGED + clean worktrees)" \
                    || echo "worktree-prune: DRY-RUN (pass --apply to reap). Candidates:"

reaped=0 skipped=0 rc=0
wt_path=""; wt_branch=""
consider() {
    [ -n "$wt_path" ] || return 0
    local path="$wt_path" branch="$wt_branch"
    # Protected: develop/main branch, detached, the main integration tree, or
    # the worktree this script runs in.
    local protected=0
    case "$branch" in develop|main|"") protected=1 ;; esac
    [ "$path" = "$main_tree" ] && protected=1
    [ "$path" = "$self_tree" ] && protected=1
    local dirty=0
    if ! git -C "$path" diff --quiet 2>/dev/null || ! git -C "$path" diff --cached --quiet 2>/dev/null; then dirty=1; fi
    local state="${PR_STATE[$branch]:-}"
    local action; action="$(prune_decision "$state" "$dirty" "$protected")"
    case "$action" in
        REAP)
            if [ "$APPLY" -eq 1 ]; then
                if git worktree remove "$path" 2>/dev/null && git branch -D "$branch" >/dev/null 2>&1; then
                    echo "  reaped   $(basename "$path") [$branch] (PR MERGED)"; reaped=$((reaped+1))
                else
                    echo "  FAILED   $(basename "$path") [$branch] — remove/delete error" >&2; rc=1
                fi
            else
                echo "  would-reap  $(basename "$path") [$branch] (PR MERGED, clean)"; reaped=$((reaped+1))
            fi ;;
        SKIP-dirty) echo "  skip(dirty) $(basename "$path") [$branch] — uncommitted changes"; skipped=$((skipped+1)) ;;
        *) : ;;  # KEEP-* / SKIP-protected — silent
    esac
}
while IFS= read -r line; do
    case "$line" in
        "worktree "*) consider; wt_path="${line#worktree }"; wt_branch="" ;;
        "branch refs/heads/"*) wt_branch="${line#branch refs/heads/}" ;;
        "detached") wt_branch="" ;;
    esac
done < <(git worktree list --porcelain 2>/dev/null)
consider

echo "worktree-prune: $([ "$APPLY" -eq 1 ] && echo reaped || echo would-reap)=$reaped  skip-dirty=$skipped"
exit $rc
