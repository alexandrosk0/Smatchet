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
#   UNKNOWN — the PR map could not be built (gh missing, failing — offline,
#             unauthenticated — or its list truncated at the limit), so "no PR"
#             cannot be claimed; the row is never a reap candidate
#
# --remote adds a section for pushed-then-abandoned `origin/*` branches
# (tooling 2026-05-30): no PR under that head name, not protected (develop /
# main / project.config.json `vcs.protected_branches`), nothing ahead of
# origin/develop, and a last commit older than --age-days (default 14). Report
# only — it may be a sibling's in-flight work, so nothing is ever deleted.
#
# One `gh pr list` call builds the headRefName→state map (not one call/branch),
# covering every PR in the repo (--limit GIT_LEFTOVER_PR_LIMIT, default 10000; a
# list that fills the limit is treated as truncated). gh's exit status is
# checked: a failed call leaves PR state UNKNOWN with a caveat, never "no PR".
#
# Usage:
#   bash scripts/dev/git-leftover-audit.sh [--no-fetch] [--remote [--age-days N]]
#   bash scripts/dev/git-leftover-audit.sh --selftest
#
# Exit: 0 — report emitted (READ-ONLY never fails the caller) · 2 — infra error
#       (not in a git work tree). --selftest: 0 pass / 1 fail.
#
# selftest: asserts-failure

set -uo pipefail

# Pure classifier: (pr_state, ahead_of_develop) -> bucket label. pr_state is the
# GitHub state ("MERGED"/"CLOSED"/"OPEN"), "" when no PR exists, or "?" when the
# PR map is unavailable or incomplete (unknown is NOT "no PR"). ahead is "1"
# when the branch has commits not on origin/develop, else "0".
classify_leftover() {
    local pr_state="$1" ahead="$2"
    case "$pr_state" in
        MERGED) echo "RESIDUE-merged" ;;
        CLOSED) echo "RESIDUE-closed" ;;
        OPEN)   echo "ACTIVE-open" ;;
        "")     if [ "$ahead" = "1" ]; then echo "WIP-or-orphan"; else echo "STALE-no-pr"; fi ;;
        "?")    echo "UNKNOWN-pr-state" ;;
        *)      echo "UNKNOWN-$pr_state" ;;
    esac
}

# Pure --remote filter: (has_pr, protected, ahead, age_days, min_age_days) ->
# STALE-remote (report it), CANDIDATE-pr-unknown (report it, flagged: it may
# have a PR), or the first reason it is left out. has_pr is "1"/"0"/"?" (PR map
# unavailable); protected / ahead are "1"/"0"; ahead means commits not on
# origin/develop.
classify_remote() {
    local has_pr="$1" protected="$2" ahead="$3" age="$4" min_age="$5"
    [ "$protected" = "1" ] && { echo "SKIP-protected"; return 0; }
    [ "$has_pr" = "1" ] && { echo "SKIP-has-pr"; return 0; }
    [ "$ahead" = "1" ] && { echo "SKIP-ahead"; return 0; }
    [ "$age" -gt "$min_age" ] || { echo "SKIP-recent"; return 0; }
    [ "$has_pr" = "?" ] && { echo "CANDIDATE-pr-unknown"; return 0; }
    echo "STALE-remote"
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
    _ck UNKNOWN-pr-state "?"  0
    _ck UNKNOWN-pr-state "?"  1
    # asserts-failure: a merged PR must NEVER classify as ACTIVE (the bug that
    # would keep reapable residue alive). Prove the classifier rejects it.
    if [ "$(classify_leftover MERGED 0)" = "ACTIVE-open" ]; then
        echo "  FAIL merged PR misclassified as ACTIVE"; fail=1
    else
        echo "  ok   merged PR is never ACTIVE"
    fi
    # asserts-failure: an UNKNOWN PR state (gh failed) must never read as a
    # no-PR reap candidate.
    if [ "$(classify_leftover "?" 0)" = "STALE-no-pr" ]; then
        echo "  FAIL unknown PR state misclassified as STALE-no-pr"; fail=1
    else
        echo "  ok   unknown PR state is never STALE-no-pr"
    fi
    _rk() { # _rk <want> <has_pr> <protected> <ahead> <age> <min_age>
        local got; got="$(classify_remote "$2" "$3" "$4" "$5" "$6")"
        if [ "$got" = "$1" ]; then echo "  ok   [$1] pr=$2 prot=$3 ahead=$4 age=$5d min=$6d"
        else echo "  FAIL [want $1, got $got] pr=$2 prot=$3 ahead=$4 age=$5d min=$6d"; fail=1; fi
    }
    _rk STALE-remote   0 0 0 30 14
    _rk SKIP-recent    0 0 0 14 14
    _rk SKIP-ahead     0 0 1 30 14
    _rk SKIP-has-pr    1 0 0 30 14
    _rk SKIP-protected 0 1 0 30 14
    _rk CANDIDATE-pr-unknown "?" 0 0 30 14
    _rk SKIP-ahead     "?" 0 1 30 14
    # asserts-failure: a branch with a PR, or with work not on develop, must
    # NEVER be reported as abandoned residue.
    if [ "$(classify_remote 1 0 0 99 0)" = "STALE-remote" ] || [ "$(classify_remote 0 0 1 99 0)" = "STALE-remote" ] \
       || [ "$(classify_remote "?" 0 0 99 0)" = "STALE-remote" ]; then
        echo "  FAIL a PR-backed, ahead-of-develop or PR-unknown remote branch was flagged STALE"; fail=1
    else
        echo "  ok   PR-backed / ahead-of-develop / PR-unknown remote branches are never STALE"
    fi
    [ "$fail" -eq 0 ] && { echo "git-leftover-audit --selftest: PASS"; exit 0; }
    echo "git-leftover-audit --selftest: FAIL"; exit 1
fi

usage() { echo "usage: $0 [--no-fetch] [--remote [--age-days N]] | --selftest" >&2; exit 2; }

FETCH=1
REMOTE=0
AGE_DAYS=14
while [ $# -gt 0 ]; do
    case "$1" in
        --no-fetch) FETCH=0 ;;
        --remote)   REMOTE=1 ;;
        --age-days) [ $# -ge 2 ] || usage; AGE_DAYS="$2"; shift ;;
        --age-days=*) AGE_DAYS="${1#*=}" ;;
        *)          usage ;;
    esac
    shift
done
case "$AGE_DAYS" in ''|*[!0-9]*) echo "git-leftover-audit: --age-days wants a whole number of days (got '$AGE_DAYS')" >&2; exit 2 ;; esac

git rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
    echo "git-leftover-audit: not inside a git work tree" >&2; exit 2
}

if [ "$FETCH" -eq 1 ]; then
    # --remote judges every origin/* ref, so it needs them all current, not
    # just develop; --prune drops refs whose remote branch is already gone.
    fetch_spec=(develop); [ "$REMOTE" -eq 1 ] && fetch_spec=()
    git fetch --prune origin ${fetch_spec[@]+"${fetch_spec[@]}"} >/dev/null 2>&1 || \
        echo "git-leftover-audit: WARN — 'git fetch --prune' failed (offline?); report may be stale" >&2
fi

# headRefName -> state map via ONE gh call. PR_MAP_COMPLETE=1 only when gh ran,
# succeeded and returned fewer rows than the limit — then a branch missing from
# the map really has no PR. Otherwise a missing branch reads "?" (unknown).
declare -A PR_STATE
PR_MAP_COMPLETE=0
PR_UNKNOWN_WHY=""
PR_LIMIT="${GIT_LEFTOVER_PR_LIMIT:-10000}"
case "$PR_LIMIT" in ''|*[!0-9]*|0) PR_LIMIT=10000 ;; esac
if command -v gh >/dev/null 2>&1; then
    gh_err="$(mktemp)" || gh_err=/dev/null
    if pr_rows="$(gh pr list --state all --limit "$PR_LIMIT" --json headRefName,state \
                    --jq '.[] | [.headRefName, .state] | @tsv' 2>"$gh_err")"; then
        pr_count=0
        while IFS=$'\t' read -r ref state; do
            [ -n "$ref" ] || continue
            pr_count=$((pr_count + 1))
            # Keep the most decisive state if a ref appears twice (OPEN wins over
            # a stale CLOSED of a reused branch name).
            if [ -z "${PR_STATE[$ref]:-}" ] || [ "$state" = "OPEN" ]; then
                PR_STATE["$ref"]="$state"
            fi
        done <<< "$pr_rows"
        if [ "$pr_count" -ge "$PR_LIMIT" ]; then
            PR_UNKNOWN_WHY="the PR list filled --limit $PR_LIMIT and may be truncated (raise GIT_LEFTOVER_PR_LIMIT)"
        else
            PR_MAP_COMPLETE=1
        fi
    else
        PR_UNKNOWN_WHY="'gh pr list' failed ($(head -n 1 "$gh_err" 2>/dev/null | tr -d '\r')): unauthenticated or offline"
    fi
    [ "$gh_err" = /dev/null ] || rm -f "$gh_err"
else
    PR_UNKNOWN_WHY="gh not on PATH"
fi
[ "$PR_MAP_COMPLETE" -eq 1 ] || \
    echo "git-leftover-audit: WARN — PR state unknown: $PR_UNKNOWN_WHY. Branches missing from the PR map are UNKNOWN, never 'no PR'." >&2

# pr_state_of <branch> — the branch's PR state, "" (no PR) or "?" (unknown).
pr_state_of() {
    local st="${PR_STATE[$1]:-}"
    if [ -z "$st" ] && [ "$PR_MAP_COMPLETE" -ne 1 ]; then st="?"; fi
    printf '%s' "$st"
}

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
    local state; state="$(pr_state_of "$branch")"
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
    [ "$state" = "?" ] && notes="${notes:+$notes; }PR state unknown — verify before reaping"
    local shown="${state:-none}"; [ "$state" = "?" ] && shown="unknown"
    printf '%-42s %-22s %-16s %s\n' "$base [$branch]" "$shown" "$bucket" "$notes"
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
echo "Merged local branches no worktree holds — run: bash scripts/dev/worktree-prune.sh --branches"

# ── --remote: pushed-then-abandoned origin/* branches (report only) ──────────
if [ "$REMOTE" -eq 1 ]; then
    echo
    if ! git rev-parse --verify --quiet refs/remotes/origin/develop >/dev/null 2>&1; then
        echo "git-leftover-audit --remote: origin/develop not found — cannot judge what is merged; section skipped." >&2
        exit 0
    fi
    # Protected: develop + main, plus project.config.json vcs.protected_branches
    # (an infra branch such as an asset store is never residue).
    protected_branches="develop main"
    # shellcheck source=scripts/dev/project-config.sh
    if . "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/project-config.sh" 2>/dev/null; then
        protected_branches="$protected_branches ${PC_VCS_PROTECTED_BRANCHES:-}"
    else
        echo "git-leftover-audit: WARN — project.config.json unreadable; protecting develop/main only" >&2
    fi
    echo "STALE REMOTE BRANCHES — origin/*, no PR, nothing ahead of origin/develop, last commit > ${AGE_DAYS}d (report only; never deleted)"
    [ "$PR_MAP_COMPLETE" -eq 1 ] || echo "  (PR state unknown — $PR_UNKNOWN_WHY; rows marked [PR?] may have a PR: verify each)"
    printf '%-52s %-8s %s\n' "REMOTE BRANCH" "AGE" "LAST COMMIT"
    now="$(date +%s)"
    stale=0 unknown=0
    while IFS=$'\t' read -r ref ts subject; do
        name="${ref#refs/remotes/origin/}"
        [ "$name" = "HEAD" ] && continue
        case "$(pr_state_of "$name")" in "") has_pr=0 ;; "?") has_pr="?" ;; *) has_pr=1 ;; esac
        protected=0; case " $protected_branches " in *" $name "*) protected=1 ;; esac
        n="$(git rev-list --count "refs/remotes/origin/develop..$ref" 2>/dev/null || echo 1)"
        ahead=0; [ "${n:-1}" -gt 0 ] && ahead=1
        age=$(( (now - ${ts:-$now}) / 86400 ))
        case "$(classify_remote "$has_pr" "$protected" "$ahead" "$age" "$AGE_DAYS")" in
            STALE-remote)
                printf '%-52s %-8s %s\n' "origin/$name" "${age}d" "$subject"
                stale=$((stale+1)) ;;
            CANDIDATE-pr-unknown)
                printf '%-52s %-8s %s\n' "origin/$name [PR?]" "${age}d" "$subject"
                unknown=$((unknown+1)) ;;
        esac
    done < <(git for-each-ref --format='%(refname)%09%(committerdate:unix)%09%(subject)' refs/remotes/origin/ 2>/dev/null)
    echo "git-leftover-audit --remote: $stale stale remote branch(es)$([ "$unknown" -gt 0 ] && echo ", $unknown more with PR state unknown [PR?]"). Review each (a sibling's parked work looks the same); delete by hand with 'git push origin --delete <branch>'."
fi
exit 0
