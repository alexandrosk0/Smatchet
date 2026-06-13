#!/usr/bin/env bash
# pr-status-watch.sh — emit one notify line per notable PR state-change
# (RED / MERGED / CLOSED / green-but-stuck) for one or more PRs.
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
# Emits (one line == one Monitor notification; each distinct line once):
#   PR #N RED: <checks>                              a required/blocking check failed
#   PR #N MERGED                                     the watcher (or anyone) merged it
#   PR #N CLOSED (not merged)
#   PR #N all checks green — awaiting watcher merge  (surfaces a wedged/dead watcher)
#
# Exit 0 when every PR is terminal (MERGED/CLOSED) or after one --once pass.
# Exit 2 on usage error. A transient `gh` failure is non-fatal (keeps polling).
set -euo pipefail

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

# Pure classifier — the unit the --selftest + bats exercise (no gh, no network).
# Echoes the notify line for this PR's state, or nothing (silent / still pending).
classify_pr_line() { # $1=pr $2=state $3=failed_csv $4=pending_count
    local pr="$1" state="$2" failed="$3" pending="$4"
    case "$state" in
        MERGED) printf 'PR #%s MERGED\n' "$pr" ;;
        CLOSED) printf 'PR #%s CLOSED (not merged)\n' "$pr" ;;
        OPEN)
            if [ -n "$failed" ]; then
                printf 'PR #%s RED: %s\n' "$pr" "$failed"
            elif [ "$pending" = "0" ]; then
                printf 'PR #%s all checks green — awaiting watcher merge\n' "$pr"
            fi ;;
    esac
}

if [ "$SELFTEST" = 1 ]; then
    fail=0
    _t() { # $1=desc $2=expected $3=actual
        if [ "$2" = "$3" ]; then printf 'ok - %s\n' "$1"
        else printf 'NOT OK - %s (want [%s] got [%s])\n' "$1" "$2" "$3"; fail=1; fi
    }
    # selftest: asserts-failure — a failed check on an OPEN PR MUST emit RED (the whole point).
    _t "red fires"             "PR #5 RED: Windows + MSVC" "$(classify_pr_line 5 OPEN 'Windows + MSVC' 2)"
    _t "red beats pending"     "PR #5 RED: Coverage"       "$(classify_pr_line 5 OPEN 'Coverage' 3)"
    _t "merged fires"          "PR #5 MERGED"              "$(classify_pr_line 5 MERGED '' 0)"
    _t "closed fires"          "PR #5 CLOSED (not merged)" "$(classify_pr_line 5 CLOSED '' 0)"
    _t "green-stuck fires"     "PR #5 all checks green — awaiting watcher merge" "$(classify_pr_line 5 OPEN '' 0)"
    _t "open+pending is silent" ""                         "$(classify_pr_line 5 OPEN '' 3)"
    if [ "$fail" = 0 ]; then echo "selftest: pr-status-watch classifier OK"; exit 0; else exit 1; fi
fi

[ "${#PRS[@]}" -gt 0 ] || { echo "usage: $0 [--once] [--interval N] [--selftest] <pr>..." >&2; exit 2; }
case "$INTERVAL" in ''|*[!0-9]*) INTERVAL=90 ;; esac

# External-tool preflight (SHELL_LINT_DEPS) — the polling path needs gh + jq. The
# --selftest path above is pure (no externals) and exits before this.
command -v gh >/dev/null 2>&1 || { echo "pr-status-watch: required tool 'gh' not on PATH" >&2; exit 2; }
command -v jq >/dev/null 2>&1 || { echo "pr-status-watch: required tool 'jq' not on PATH" >&2; exit 2; }

# Field separator for poll_pr's packed row. Must be NON-whitespace: read with a
# whitespace IFS (tab/space) collapses consecutive delimiters, dropping an empty
# middle field (a green PR has empty `failed`, which would shift `pending` into it
# and mis-emit "RED: <count>"). US (0x1f) never appears in a GitHub check name.
SEP=$'\037'

# Poll one PR via gh; echo "state<SEP>failed_csv<SEP>pending" or return 1 on gh error.
poll_pr() { # $1=pr
    local pr="$1" j st failed pending
    j="$(gh pr view "$pr" --json state,statusCheckRollup 2>/dev/null)" || return 1
    st="$(printf '%s' "$j" | jq -r '.state')"
    failed="$(printf '%s' "$j" | jq -r '[.statusCheckRollup[]? | select(.conclusion=="FAILURE" or .conclusion=="TIMED_OUT" or .conclusion=="STARTUP_FAILURE") | (.name // .context)] | join(", ")')"
    pending="$(printf '%s' "$j" | jq -r '[.statusCheckRollup[]? | select(.status!="COMPLETED")] | length')"
    printf '%s%s%s%s%s' "$st" "$SEP" "$failed" "$SEP" "$pending"
}

declare -A seen
while true; do
    open=0
    for pr in "${PRS[@]}"; do
        row="$(poll_pr "$pr")" || { open=1; continue; }   # gh error -> assume in-flight, keep watching
        IFS="$SEP" read -r st failed pending <<< "$row" || true
        [ "$st" = OPEN ] && open=1
        line="$(classify_pr_line "$pr" "$st" "$failed" "$pending")"
        [ -n "$line" ] || continue
        key="$pr:$line"
        if [ -z "${seen[$key]:-}" ]; then echo "$line"; seen["$key"]=1; fi
    done
    [ "$ONCE" = 1 ] && break
    [ "$open" = 0 ] && { echo "all PRs terminal — watch ending"; break; }
    sleep "$INTERVAL"
done
