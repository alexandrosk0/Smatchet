#!/usr/bin/env bash
# pr-status-watch.sh — emit one notify line per notable PR state-change
# (RED / MERGED / CLOSED / CR-blocked / green-+-CR-clear) for one or more PRs.
#
# WHY: the smatchet-merge-watcher is a SEPARATE daemon process. When it merges a
# PR — or parks a RED PR in BLOCKED/triage — it does NOT signal the Claude session
# that registered it (its halt prompts target SmatchetToastManager, "not back to
# this session"; ship-loops.md § post-ship option 3). So a session that registered
# a PR and committed to *reporting the outcome* is blind to a red check unless it
# polls. Run this UNDER the harness Monitor tool (each stdout line becomes a
# notification) — or run_in_background — to close that blind spot.
#
# Usage:
#   bash scripts/dev/pr-status-watch.sh <pr> [<pr> ...]      # poll until all terminal
#   bash scripts/dev/pr-status-watch.sh --once <pr> ...      # single pass (cron / test)
#   bash scripts/dev/pr-status-watch.sh --interval 60 <pr>   # poll cadence (default 90s)
#   bash scripts/dev/pr-status-watch.sh --selftest           # classifier fixtures (no gh)
#
# A RED line fires ONLY for a check that actually BLOCKS merge — mirroring the
# merge-gates gating contract so the watch stops noise-flagging non-blocking reds
# (the recurring "RED: Perf PR-fast" on an OOB-labelled PR, or "RED: <advisory>"
# on a check that never gated). A failing check is BLOCKING iff it is gating
# (REQUIRED ∈ branch_protection.required_contexts, OR allow-listed-non-advisory:
# name ~ $MERGE_GATES_BLOCK_ALLOWLIST_RE single-sourced from merge-gates.sh and
# not "advisory") AND not downgraded by a PR label (tests-out-of-band →
# "Test-delta gate" · perf-out-of-band → "Perf PR-fast*"). A non-gating or
# OOB-downgraded FAILURE is dropped from the RED line, not surfaced as noise.
#
# Emits (one line == one Monitor notification; each distinct line once):
#   PR #N RED: <checks>                                 a BLOCKING check failed
#   PR #N MERGED                                        the watcher (or anyone) merged it
#   PR #N CLOSED (not merged)
#   PR #N CI green but CodeRabbit has N actionable …    blocked on review (not a CI fail)
#   PR #N all green + CR clear — awaiting watcher merge surfaces a wedged/dead watcher
# (silent while CI checks or the CodeRabbit review are still in-flight.)
#
# Exit 0 when every PR is terminal (MERGED/CLOSED) or after one --once pass.
# Exit 2 on usage error. A transient `gh` failure is non-fatal (keeps polling).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

INTERVAL=90
ONCE=0
SELFTEST=0
PRS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --once)        ONCE=1; shift ;;
        --selftest)    SELFTEST=1; shift ;;
        --interval)    INTERVAL="${2:-90}"; shift 2 ;;
        --interval=*)  INTERVAL="${1#--interval=}"; shift ;;
        -*)            echo "usage: $0 [--once] [--interval N] [--selftest] <pr>..." >&2; exit 2 ;;
        *)             PRS+=("$1"); shift ;;
    esac
done

# Pure classifiers — the units the --selftest + bats exercise (no gh, no network).

# classify_cr <review_state> <review_body> <review_commit> <head_oid> -> clear | actionable:N | pending
# Maps the latest CodeRabbit review to the merge-gate's CR verdict: APPROVED or
# "Actionable comments posted: 0" => clear; CHANGES_REQUESTED or actionable>0 =>
# actionable; no review / COMMENTED-without-a-count => pending (in-progress).
# STALENESS GUARD: if the latest CR review is on a commit OTHER than the current
# head (a fix-push CR hasn't re-reviewed yet), the count is stale — treat as
# pending, NOT a live verdict. Without this the watch false-reports the pre-fix
# count after every CR-fix push ("don't trust a CR count without reading the cited
# commit" — AGENTS.md § Merge gates). review_commit/head_oid empty => skip the
# guard (keeps the pure 2-arg selftest calls valid).
classify_cr() {
    local s="$1" b="$2" rc="${3:-}" head="${4:-}" n
    [ -z "$s" ] && { printf 'pending'; return; }
    [ -n "$rc" ] && [ -n "$head" ] && [ "$rc" != "$head" ] && { printf 'pending'; return; }
    [ "$s" = "APPROVED" ] && { printf 'clear'; return; }
    [ "$s" = "CHANGES_REQUESTED" ] && { printf 'actionable:?'; return; }
    n="$(printf '%s' "$b" | grep -oE 'Actionable comments posted: [0-9]+' | grep -oE '[0-9]+' | head -1)"
    [ -z "$n" ] && { printf 'pending'; return; }
    if [ "$n" = "0" ]; then printf 'clear'; else printf 'actionable:%s' "$n"; fi
}

# classify_pr_line <pr> <state> <failed_csv> <ci_pending_count> <cr>
# Echoes the notify line, or nothing (silent / still in-flight). Priority:
# CI failure > CI still running (silent) > CI green + CR verdict.
classify_pr_line() {
    local pr="$1" state="$2" failed="$3" ci_pending="$4" cr="$5"
    case "$state" in
        MERGED) printf 'PR #%s MERGED\n' "$pr"; return ;;
        CLOSED) printf 'PR #%s CLOSED (not merged)\n' "$pr"; return ;;
        OPEN)   ;;
        *)      return ;;
    esac
    if [ -n "$failed" ]; then printf 'PR #%s RED: %s\n' "$pr" "$failed"; return; fi
    if [ "${ci_pending:-0}" -gt 0 ] 2>/dev/null; then return; fi   # CI still running -> silent
    case "$cr" in
        actionable:*) printf 'PR #%s CI green but CodeRabbit has %s actionable finding(s) — blocked on review\n' "$pr" "${cr#actionable:}" ;;
        clear)        printf 'PR #%s all green + CR clear — awaiting watcher merge\n' "$pr" ;;
        *)            : ;;   # CR review still pending -> silent (in-progress, like CI)
    esac
}

if [ "$SELFTEST" = 1 ]; then
    fail=0
    _t() { # $1=desc $2=expected $3=actual
        if [ "$2" = "$3" ]; then printf 'ok - %s\n' "$1"
        else printf 'NOT OK - %s (want [%s] got [%s])\n' "$1" "$2" "$3"; fail=1; fi
    }
    # selftest: asserts-failure — a failed check on an OPEN PR MUST emit RED (the whole point).
    _t "red fires"              "PR #5 RED: Windows + MSVC" "$(classify_pr_line 5 OPEN 'Windows + MSVC' 0 clear)"
    _t "red beats CI-pending"   "PR #5 RED: Coverage"       "$(classify_pr_line 5 OPEN 'Coverage' 3 pending)"
    _t "red beats CR-actionable" "PR #5 RED: X"             "$(classify_pr_line 5 OPEN 'X' 0 actionable:2)"
    _t "merged fires"           "PR #5 MERGED"              "$(classify_pr_line 5 MERGED '' 0 clear)"
    _t "closed fires"           "PR #5 CLOSED (not merged)" "$(classify_pr_line 5 CLOSED '' 0 clear)"
    _t "green+CR-clear awaits"  "PR #5 all green + CR clear — awaiting watcher merge" "$(classify_pr_line 5 OPEN '' 0 clear)"
    _t "green+CR-actionable"    "PR #5 CI green but CodeRabbit has 2 actionable finding(s) — blocked on review" "$(classify_pr_line 5 OPEN '' 0 actionable:2)"
    _t "CI-pending is silent"   ""                          "$(classify_pr_line 5 OPEN '' 3 clear)"
    _t "green+CR-pending silent" ""                         "$(classify_pr_line 5 OPEN '' 0 pending)"
    # classify_cr verdict mapping
    _t "cr approved -> clear"   "clear"        "$(classify_cr APPROVED '')"
    _t "cr 0-actionable -> clear" "clear"      "$(classify_cr COMMENTED 'Actionable comments posted: 0')"
    _t "cr N-actionable"        "actionable:2" "$(classify_cr COMMENTED 'Actionable comments posted: 2')"
    _t "cr changes-requested"   "actionable:?" "$(classify_cr CHANGES_REQUESTED '')"
    _t "cr no-review -> pending" "pending"     "$(classify_cr '' '')"
    # staleness guard: a real actionable count on the CURRENT head fires; the SAME count on a
    # stale (pre-fix-push) commit is suppressed to pending (don't false-report after a fix push).
    _t "cr actionable on head"  "actionable:2" "$(classify_cr COMMENTED 'Actionable comments posted: 2' abc123 abc123)"
    _t "cr actionable stale-commit -> pending" "pending" "$(classify_cr COMMENTED 'Actionable comments posted: 2' oldsha newsha)"
    if [ "$fail" = 0 ]; then echo "selftest: pr-status-watch classifier OK"; exit 0; else exit 1; fi
fi

[ "${#PRS[@]}" -gt 0 ] || { echo "usage: $0 [--once] [--interval N] [--selftest] <pr>..." >&2; exit 2; }
case "$INTERVAL" in ''|*[!0-9]*) INTERVAL=90 ;; esac

# External-tool preflight (SHELL_LINT_DEPS) — the polling path needs gh + jq. The
# --selftest path above is pure (no externals) and exits before this.
command -v gh >/dev/null 2>&1 || { echo "pr-status-watch: required tool 'gh' not on PATH" >&2; exit 2; }
command -v jq >/dev/null 2>&1 || { echo "pr-status-watch: required tool 'jq' not on PATH" >&2; exit 2; }

# Single-source the meant-to-block allow-list regex from merge-gates.sh (NOT a
# duplicated literal — change it there and this watch follows). The source is
# side-effect-free when sourced (its CLI entry-point is `$0`-guarded). Done in
# the polling path only, AFTER the pure --selftest early-exit, so --selftest
# stays dependency-free.
# shellcheck source=agents/scripts/core/merge-gates.sh
source "$REPO_ROOT/agents/scripts/core/merge-gates.sh"
if [ -z "${MERGE_GATES_BLOCK_ALLOWLIST_RE:-}" ]; then
    echo "pr-status-watch: merge-gates.sh did not export MERGE_GATES_BLOCK_ALLOWLIST_RE" >&2
    exit 2
fi

# read_required_contexts — newline-separated branch-protection required context
# names. Override via PR_STATUS_WATCH_REQUIRED_CONTEXTS (even ""), else read
# project.config.json (UTF-8-safe, matches merge-gates.sh / safe-admin-merge.sh).
read_required_contexts() {
    if [ -n "${PR_STATUS_WATCH_REQUIRED_CONTEXTS+x}" ]; then
        printf '%s\n' "${PR_STATUS_WATCH_REQUIRED_CONTEXTS//,/$'\n'}"
        return 0
    fi
    local config_file="${PR_STATUS_WATCH_CONFIG_FILE:-$REPO_ROOT/project.config.json}"
    if [ -f "$config_file" ]; then
        jq -r '.branch_protection.required_contexts[]? // empty' "$config_file" 2>/dev/null || true
    fi
}

# Resolve the required-context set once into a JSON array for poll_pr's jq.
REQ_JSON='[]'
_req_list="$(read_required_contexts)"
if [ -n "$_req_list" ]; then
    REQ_JSON="$(printf '%s\n' "$_req_list" | jq -R . | jq -sc 'map(select(length > 0))')" || REQ_JSON='[]'
fi

# Field separator for poll_pr's packed row. Must be NON-whitespace: read with a
# whitespace IFS (tab/space) collapses consecutive delimiters, dropping an empty
# middle field (a green PR has empty `failed`, which would shift `pending` into it
# and mis-emit "RED: <count>"). US (0x1f) never appears in a GitHub check name.
SEP=$'\037'

# Poll one PR via gh; echo "state<SEP>failed<SEP>ci_pending<SEP>cr" or return 1 on gh error.
# ci_pending EXCLUDES CodeRabbit-type checks (they gate via the review parse, not
# the rollup) so "CI green" can be reached while the CR check is still pending.
poll_pr() { # $1=pr
    local pr="$1" j st failed ci_pending head crstate crbody crcommit cr
    j="$(gh pr view "$pr" --json state,statusCheckRollup,reviews,headRefOid,labels 2>/dev/null)" || return 1
    st="$(printf '%s' "$j" | jq -r '.state')"
    # failed = BLOCKING failures only — a FAILURE/TIMED_OUT/STARTUP_FAILURE row
    # that is gating (required OR allow-listed-non-advisory) and not downgraded
    # by a *-out-of-band PR label. Mirrors the merge-gates $failing contract so a
    # non-gating / OOB-waived red is silent, not noise.
    # KNOWN DIVERGENCE (advisory tool; poller is authoritative): the poller's
    # stale-override guard (merge-pipeline-01) additionally REFUSES a tests-/
    # perf-out-of-band downgrade when the failing run completed before the
    # label was applied — this watch has no label-event timestamps (`gh pr
    # view` carries no timeline) and waives unconditionally, so during that
    # race window it can show clean while poll_merge_gates blocks with its
    # stale-override WARN. Trust the poller, not this display, for merge
    # decisions.
    failed="$(printf '%s' "$j" | jq -r \
        --argjson req "$REQ_JSON" \
        --arg allow "$MERGE_GATES_BLOCK_ALLOWLIST_RE" '
        ([.labels[]?.name] // []) as $labels
        | ($labels | any(. == "tests-out-of-band")) as $oobTests
        | ($labels | any(. == "perf-out-of-band")) as $oobPerf
        | [ .statusCheckRollup[]?
            | select(.conclusion=="FAILURE" or .conclusion=="TIMED_OUT" or .conclusion=="STARTUP_FAILURE")
            | (.name // .context) as $name
            | select(($req | any(. == $name))
                     or (($name | test($allow; "i"))
                         and (($name | ascii_downcase | contains("advisory")) | not)))
            | select(($oobTests and $name == "Test-delta gate") | not)
            | select(($oobPerf and ($name | startswith("Perf PR-fast"))) | not)
            | $name ]
        | join(", ")')"
    ci_pending="$(printf '%s' "$j" | jq -r '[.statusCheckRollup[]? | select(.status!="COMPLETED") | (.name // .context) | select(test("coderabbit|CR finding";"i") | not)] | length')"
    head="$(printf '%s' "$j" | jq -r '.headRefOid // ""')"
    crstate="$(printf '%s' "$j" | jq -r '[.reviews[]? | select(.author.login | test("coderabbit";"i"))] | last | .state // ""')"
    crbody="$(printf '%s' "$j" | jq -r '[.reviews[]? | select(.author.login | test("coderabbit";"i"))] | last | .body // ""')"
    crcommit="$(printf '%s' "$j" | jq -r '[.reviews[]? | select(.author.login | test("coderabbit";"i"))] | last | .commit.oid // ""')"
    cr="$(classify_cr "$crstate" "$crbody" "$crcommit" "$head")"
    printf '%s%s%s%s%s%s%s' "$st" "$SEP" "$failed" "$SEP" "$ci_pending" "$SEP" "$cr"
}

declare -A seen
while true; do
    open=0
    for pr in "${PRS[@]}"; do
        row="$(poll_pr "$pr")" || { open=1; continue; }   # gh error -> assume in-flight, keep watching
        IFS="$SEP" read -r st failed ci_pending cr <<< "$row" || true
        [ "$st" = OPEN ] && open=1
        line="$(classify_pr_line "$pr" "$st" "$failed" "$ci_pending" "$cr")"
        [ -n "$line" ] || continue
        key="$pr:$line"
        if [ -z "${seen[$key]:-}" ]; then echo "$line"; seen["$key"]=1; fi
    done
    [ "$ONCE" = 1 ] && break
    [ "$open" = 0 ] && { echo "all PRs terminal — watch ending"; break; }
    sleep "$INTERVAL"
done
