#!/usr/bin/env bash
# worktree-prune.sh — reap worktrees whose branch's GitHub PR is MERGED. The
# acting companion to the read-only scripts/dev/git-leftover-audit.sh: it removes
# only the unambiguously-safe RESIDUE-merged subset, leaving CLOSED / OPEN / WIP /
# STALE worktrees for a human (git-janitor v5 § Stale-branch-sweep encoded this
# discipline in prose; this is the re-runnable script — tooling self-improvement
# 2026-05-18 / 2026-05-30 / 2026-06-11).
#
# DRY-RUN BY DEFAULT — prints what it WOULD reap and exits 0. Pass --apply to
# actually `git worktree remove` + `git branch -D`.
#
# HARD GUARDS (a reap is performed ONLY when ALL hold):
#   * the branch's PR state is MERGED (one `gh pr list` call builds the map),
#   * the worktree is CLEAN (no uncommitted, staged or untracked files — dirty
#     is skipped and surfaced, never force-removed),
#   * the worktree is IDLE: its HEAD, index and HEAD reflog are all older than
#     --idle-hours (default 24), so a sweep never races a session still using it,
#   * the worktree's HEAD is still the merged PR's head commit — a follow-up
#     commit made after the merge (committed, so the tree reads clean, but never
#     pushed) would otherwise die with the `git branch -D` (SKIP-moved),
#   * the branch is NOT protected (develop / main / project.config.json
#     `vcs.protected_branches`) and the path is NOT the current worktree nor the
#     main integration tree.
#
# --branches MODE prunes LOCAL BRANCHES instead of worktrees — the mid-session
# light prune for merged-branch residue that parallel agents leave behind
# (tooling 2026-05-30). Same dry-run/--apply contract. A branch is deleted ONLY
# when its PR is MERGED, no worktree holds it, it is not protected, and its tip
# is still the merged PR head (commits added after the merge are never thrown
# away). OPEN, CLOSED and no-PR branches are never touched.
#
# Usage:
#   bash scripts/dev/worktree-prune.sh                     # dry-run (default)
#   bash scripts/dev/worktree-prune.sh --apply             # actually reap
#   bash scripts/dev/worktree-prune.sh --idle-hours 6      # idle threshold (0 = off)
#   bash scripts/dev/worktree-prune.sh --branches [--apply]  # local-branch prune
#   bash scripts/dev/worktree-prune.sh --selftest
#
# Exit: 0 — ran (dry-run, or apply with all reaps OK) · 1 — an --apply reap
#       failed / --selftest failure · 2 — not in a git work tree / bad usage.
#
# selftest: asserts-failure

set -uo pipefail

# Pure decision: (pr_state, dirty, protected, active, at_pr_head) -> action.
# Keeps the guard logic unit-testable. All flags are "1"/"0"; active means the
# worktree saw HEAD/index activity inside the idle threshold; at_pr_head means
# the worktree's HEAD is the merged PR's head commit (absent = "0", fail-safe).
prune_decision() {
    local pr_state="$1" dirty="$2" protected="$3" active="${4:-0}" at_head="${5:-0}"
    [ "$protected" = "1" ] && { echo "SKIP-protected"; return 0; }
    [ "$pr_state" != "MERGED" ] && { echo "KEEP-$pr_state"; return 0; }
    [ "$dirty" = "1" ] && { echo "SKIP-dirty"; return 0; }
    [ "$active" = "1" ] && { echo "SKIP-active"; return 0; }
    [ "$at_head" = "1" ] || { echo "SKIP-moved"; return 0; }
    echo "REAP"
}

# Pure --branches decision: (pr_state, held, protected, at_pr_head) -> action.
# held = a worktree has the branch checked out; at_pr_head = the local tip is
# the merged PR's head commit. All flags are "1"/"0".
branch_decision() {
    local pr_state="$1" held="$2" protected="$3" at_head="$4"
    [ "$protected" = "1" ] && { echo "SKIP-protected"; return 0; }
    [ "$held" = "1" ] && { echo "SKIP-held"; return 0; }
    [ "$pr_state" != "MERGED" ] && { echo "KEEP-$pr_state"; return 0; }
    [ "$at_head" = "1" ] || { echo "SKIP-moved"; return 0; }
    echo "DELETE"
}

if [ "${1:-}" = "--selftest" ]; then
    fail=0
    _ck() { local got; got="$(prune_decision "$2" "$3" "$4" "$5" "$6")"
        if [ "$got" = "$1" ]; then echo "  ok   [$1] state='$2' dirty=$3 prot=$4 active=$5 at-head=$6"
        else echo "  FAIL [want $1 got $got] state='$2' dirty=$3 prot=$4 active=$5 at-head=$6"; fail=1; fi; }
    _bk() { local got; got="$(branch_decision "$2" "$3" "$4" "$5")"
        if [ "$got" = "$1" ]; then echo "  ok   [$1] state='$2' held=$3 prot=$4 at-head=$5"
        else echo "  FAIL [want $1 got $got] state='$2' held=$3 prot=$4 at-head=$5"; fail=1; fi; }
    echo "worktree-prune --selftest:"
    _ck REAP           MERGED 0 0 0 1
    _ck SKIP-dirty     MERGED 1 0 0 1
    _ck SKIP-protected MERGED 0 1 0 1
    _ck SKIP-active    MERGED 0 0 1 1
    _ck SKIP-dirty     MERGED 1 0 1 1
    _ck SKIP-moved     MERGED 0 0 0 0
    _ck SKIP-moved     MERGED 0 0 0 ""
    _ck KEEP-OPEN      OPEN   0 0 0 1
    _ck KEEP-          ""     0 0 0 1
    _bk DELETE         MERGED 0 0 1
    _bk SKIP-held      MERGED 1 0 1
    _bk SKIP-protected MERGED 0 1 1
    _bk SKIP-moved     MERGED 0 0 0
    _bk KEEP-OPEN      OPEN   0 0 1
    _bk KEEP-CLOSED    CLOSED 0 0 1
    _bk KEEP-          ""     0 0 0
    # asserts-failure: --branches must never delete a branch a worktree holds,
    # an OPEN-PR branch, a no-PR branch, or one with commits past its merged head.
    _never_fail=0
    _never() { [ "$(branch_decision "$@")" != "DELETE" ] || {
        echo "  FAIL --branches would delete state='$1' held=$2 prot=$3 at-head=$4"; _never_fail=1; fail=1; }; }
    _never MERGED 1 0 1   # held by a worktree
    _never OPEN   0 0 1
    _never ""     0 0 1   # no PR
    _never MERGED 0 0 0   # commits past the merged head
    [ "$_never_fail" -eq 0 ] && echo "  ok   held / OPEN / no-PR / moved branches are never DELETE"
    # asserts-failure: a DIRTY merged worktree must NEVER be reaped (would lose
    # uncommitted work), nor one a session touched inside the idle threshold
    # (would pull the tree out from under it). Prove both guards hold.
    if [ "$(prune_decision MERGED 1 0 0 1)" = "REAP" ]; then
        echo "  FAIL dirty merged worktree was marked REAP"; fail=1
    else echo "  ok   dirty merged worktree is never REAP"; fi
    if [ "$(prune_decision MERGED 0 0 1 1)" = "REAP" ]; then
        echo "  FAIL recently-active merged worktree was marked REAP"; fail=1
    else echo "  ok   recently-active merged worktree is never REAP"; fi
    # ...nor one whose HEAD moved past the merged PR head: `git branch -D` would
    # destroy the committed-but-unpushed follow-up commits.
    if [ "$(prune_decision MERGED 0 0 0 0)" = "REAP" ]; then
        echo "  FAIL merged worktree with commits past the PR head was marked REAP"; fail=1
    else echo "  ok   merged worktree with commits past the PR head is never REAP"; fi
    [ "$fail" -eq 0 ] && { echo "worktree-prune --selftest: PASS"; exit 0; }
    echo "worktree-prune --selftest: FAIL"; exit 1
fi

usage() { echo "usage: $0 [--branches] [--apply] [--idle-hours N] | --selftest" >&2; exit 2; }

APPLY=0
BRANCHES=0
IDLE_HOURS=24
while [ $# -gt 0 ]; do
    case "$1" in
        --apply)      APPLY=1 ;;
        --dry-run)    APPLY=0 ;;   # the default; accepted because the audit suggests it
        --branches)   BRANCHES=1 ;;
        --idle-hours) [ $# -ge 2 ] || usage; IDLE_HOURS="$2"; shift ;;
        --idle-hours=*) IDLE_HOURS="${1#*=}" ;;
        *)            usage ;;
    esac
    shift
done
case "$IDLE_HOURS" in ''|*[!0-9]*) echo "worktree-prune: --idle-hours wants a whole number of hours (got '$IDLE_HOURS')" >&2; exit 2 ;; esac

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "worktree-prune: not inside a git work tree" >&2; exit 2
}

# Protected branches: develop + main always, plus project.config.json
# `vcs.protected_branches` (an infra branch such as an asset store must never be
# reaped). Best-effort: without the config the two integration branches still hold.
PROTECTED_BRANCHES="develop main"
# shellcheck source=scripts/dev/project-config.sh
if . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project-config.sh" 2>/dev/null; then
    PROTECTED_BRANCHES="$PROTECTED_BRANCHES ${PC_VCS_PROTECTED_BRANCHES:-}"
else
    echo "worktree-prune: WARN — project.config.json unreadable; protecting develop/main only" >&2
fi
is_protected_branch() {
    case " $PROTECTED_BRANCHES " in *" $1 "*) return 0 ;; esac
    return 1
}

# Newest mtime (epoch seconds) of a worktree's HEAD, index and HEAD reflog —
# the files every checkout, commit, reset and `git add` rewrites. Read BEFORE
# the dirty check: a plain `git status` may refresh (rewrite) the index.
mtime_of() { stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null; }
last_activity() {
    local gitdir f t newest=0
    gitdir="$(git -C "$1" rev-parse --absolute-git-dir 2>/dev/null)" || return 1
    for f in "$gitdir/HEAD" "$gitdir/index" "$gitdir/logs/HEAD"; do
        [ -e "$f" ] || continue
        t="$(mtime_of "$f")" || continue
        [ "${t:-0}" -gt "$newest" ] && newest="$t"
    done
    [ "$newest" -gt 0 ] || return 1
    echo "$newest"
}

main_tree="$(git worktree list --porcelain 2>/dev/null | awk '/^worktree /{print $2; exit}')"
self_tree="$(git rev-parse --show-toplevel 2>/dev/null)"

declare -A PR_STATE PR_HEAD
if command -v gh >/dev/null 2>&1; then
    git fetch --prune origin develop >/dev/null 2>&1 || true
    while IFS=$'\t' read -r ref state oid; do
        [ -n "$ref" ] || continue
        if [ -z "${PR_STATE[$ref]:-}" ] || [ "$state" = "OPEN" ]; then
            PR_STATE["$ref"]="$state"; PR_HEAD["$ref"]="${oid:-}"
        fi
    done < <(gh pr list --state all --limit 1000 --json headRefName,state,headRefOid \
                --jq '.[] | [.headRefName, .state, .headRefOid] | @tsv' 2>/dev/null || true)
else
    echo "worktree-prune: gh not on PATH — cannot determine MERGED state; nothing to reap." >&2
    exit 0
fi

# ── --branches: local branches no worktree holds ─────────────────────────────
if [ "$BRANCHES" -eq 1 ]; then
    declare -A HELD
    while IFS= read -r line; do
        case "$line" in "branch refs/heads/"*) HELD["${line#branch refs/heads/}"]=1 ;; esac
    done < <(git worktree list --porcelain 2>/dev/null)
    [ "$APPLY" -eq 1 ] && echo "worktree-prune --branches: --apply (deleting MERGED local branches no worktree holds)" \
                        || echo "worktree-prune --branches: DRY-RUN (pass --apply to delete). Candidates:"
    deleted=0 moved=0 rc=0
    while IFS=$'\t' read -r ref tip; do
        # Full refname, not :short — short names gain a `heads/` prefix when a
        # tag of the same name exists, and would then miss every map lookup.
        branch="${ref#refs/heads/}"
        [ -n "$branch" ] || continue
        protected=0; is_protected_branch "$branch" && protected=1
        state="${PR_STATE[$branch]:-}"
        at_head=0; [ -n "${PR_HEAD[$branch]:-}" ] && [ "${PR_HEAD[$branch]}" = "$tip" ] && at_head=1
        case "$(branch_decision "$state" "${HELD[$branch]:-0}" "$protected" "$at_head")" in
            DELETE)
                if [ "$APPLY" -eq 1 ]; then
                    if git branch -D "$branch" >/dev/null 2>&1; then
                        echo "  deleted       $branch (PR MERGED)"; deleted=$((deleted+1))
                    else
                        echo "  FAILED        $branch — branch -D error" >&2; rc=1
                    fi
                else
                    echo "  would-delete  $branch (PR MERGED, no worktree)"; deleted=$((deleted+1))
                fi ;;
            SKIP-moved) echo "  skip(moved)   $branch — tip is not the merged PR head; commits since the merge?"; moved=$((moved+1)) ;;
            *) : ;;  # KEEP-* / SKIP-held / SKIP-protected — silent
        esac
    done < <(git for-each-ref --format='%(refname)%09%(objectname)' refs/heads/ 2>/dev/null)
    echo "worktree-prune --branches: $([ "$APPLY" -eq 1 ] && echo deleted || echo would-delete)=$deleted  skip-moved=$moved"
    exit $rc
fi

[ "$APPLY" -eq 1 ] && echo "worktree-prune: --apply (reaping MERGED + clean + idle worktrees)" \
                    || echo "worktree-prune: DRY-RUN (pass --apply to reap). Candidates:"

now="$(date +%s)"
idle_secs=$((IDLE_HOURS * 3600))
reaped=0 skipped=0 rc=0
wt_path=""; wt_branch=""; wt_head=""
consider() {
    [ -n "$wt_path" ] || return 0
    local path="$wt_path" branch="$wt_branch" head="$wt_head"
    # Protected: a protected branch, detached, the main integration tree, or
    # the worktree this script runs in.
    local protected=0
    [ -z "$branch" ] && protected=1
    is_protected_branch "$branch" && protected=1
    [ "$path" = "$main_tree" ] && protected=1
    [ "$path" = "$self_tree" ] && protected=1
    # Active unless proven idle: an unreadable timestamp counts as recent.
    local active=0 last age=""
    if [ "$idle_secs" -gt 0 ]; then
        if last="$(last_activity "$path")"; then
            age=$((now - last))
            [ "$age" -lt "$idle_secs" ] && active=1
        else
            active=1
        fi
    fi
    # Untracked files count as dirty: `git worktree remove` refuses them too,
    # and they are as likely to be someone's work as a modified tracked file.
    # --no-optional-locks keeps status from rewriting the index it inspects.
    local dirty=0 st
    if ! st="$(git -C "$path" --no-optional-locks status --porcelain 2>/dev/null)" || [ -n "$st" ]; then dirty=1; fi
    local state="${PR_STATE[$branch]:-}"
    # Same tip guard as --branches: only a HEAD that IS the merged PR head is
    # reapable — `git branch -D` would drop any commit made after the merge.
    local at_head=0
    [ -n "${PR_HEAD[$branch]:-}" ] && [ "${PR_HEAD[$branch]}" = "$head" ] && at_head=1
    local action; action="$(prune_decision "$state" "$dirty" "$protected" "$active" "$at_head")"
    case "$action" in
        REAP)
            if [ "$APPLY" -eq 1 ]; then
                if git worktree remove "$path" 2>/dev/null && git branch -D "$branch" >/dev/null 2>&1; then
                    echo "  reaped   $(basename "$path") [$branch] (PR MERGED)"; reaped=$((reaped+1))
                else
                    echo "  FAILED   $(basename "$path") [$branch] — remove/delete error" >&2; rc=1
                fi
            else
                echo "  would-reap  $(basename "$path") [$branch] (PR MERGED, clean, idle)"; reaped=$((reaped+1))
            fi ;;
        SKIP-dirty) echo "  skip(dirty) $(basename "$path") [$branch] — uncommitted or untracked changes"; skipped=$((skipped+1)) ;;
        SKIP-active) echo "  skip(active) $(basename "$path") [$branch] — HEAD/index touched ${age:+$((age / 3600))h }ago, inside --idle-hours $IDLE_HOURS"; skipped=$((skipped+1)) ;;
        SKIP-moved) echo "  skip(moved) $(basename "$path") [$branch] — HEAD is not the merged PR head; commits since the merge?"; skipped=$((skipped+1)) ;;
        *) : ;;  # KEEP-* / SKIP-protected — silent
    esac
}
while IFS= read -r line; do
    case "$line" in
        "worktree "*) consider; wt_path="${line#worktree }"; wt_branch=""; wt_head="" ;;
        "HEAD "*) wt_head="${line#HEAD }" ;;
        "branch refs/heads/"*) wt_branch="${line#branch refs/heads/}" ;;
        "detached") wt_branch="" ;;
    esac
done < <(git worktree list --porcelain 2>/dev/null)
consider

echo "worktree-prune: $([ "$APPLY" -eq 1 ] && echo reaped || echo would-reap)=$reaped  skipped=$skipped"
exit $rc
