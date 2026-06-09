#!/usr/bin/env bash
# finding-already-fixed.sh — dedup pre-check before dispatching a fix for a
# historical-review finding (or any Issue). Catches the no-op-dispatch class:
# a finding that was ALREADY fixed + merged (and whose original Issue is CLOSED)
# while a DUPLICATE Issue was filed later by the historical-review sweep —
# e.g. finding #948 was fixed by PR #959 (Issue #954 CLOSED), yet the sweep
# filed #1002 for the same finding; dispatching a fix agent against #1002 was a
# pure no-op that burned a full agent run.
#
# Two signals, EITHER of which means "likely already fixed — confirm before you
# spawn a fix agent":
#   (1) a CLOSED Issue references the finding number, AND/OR
#   (2) a fix-identifying token (CR-id, function name, the exact buggy line) is
#       already present in origin/develop's history (`git log -S`).
#
# Usage:
#   finding-already-fixed.sh <finding-ref> [<fix-grep-token>]
#     <finding-ref>      e.g. 948  — the historical-review finding / PR number
#                        searched across ALL issues (open + closed).
#     <fix-grep-token>   optional — a string the FIX introduced (e.g. a CR-id
#                        comment "CR-948-1", a new symbol, a guard name). Checked
#                        with `git log -S <token> origin/develop`.
#   finding-already-fixed.sh --selftest
#
# Exit: 0 = an already-fixed signal was found (STOP — verify, don't dispatch).
#       1 = no already-fixed signal (proceed to dispatch).
#       2 = infra (no gh / no git / bad args).
#
# This is advisory tooling for the § Fixing an Issue elevation flow in
# docs/agent-rules/issue-triage.md — not a CI gate. It still ships a --selftest.
set -uo pipefail

# --- pure verdict core (no gh/git) — testable; shared by run + --selftest ------
# verdict_from_signals <closed-issue-refs> <develop-token-hits>
#   <closed-issue-refs>   newline list of "<num> CLOSED <title>" for issues that
#                         reference the finding and are CLOSED ("" if none).
#   <develop-token-hits>  the `git log -S` oneline output ("" if none / no token).
# Prints the human verdict to stdout; returns 0 if EITHER signal is non-empty.
verdict_from_signals() {
    local closed="$1" hits="$2"
    local found=1
    if [ -n "$closed" ]; then
        echo "ALREADY-FIXED signal: a CLOSED issue references this finding —"
        printf '  %s\n' "$closed"
        found=0
    fi
    if [ -n "$hits" ]; then
        echo "ALREADY-FIXED signal: the fix token is present on origin/develop —"
        printf '  %s\n' "$hits"
        found=0
    fi
    if [ "$found" -ne 0 ]; then
        echo "No already-fixed signal — proceed to dispatch the fix (still confirm the cited file:line is actually unfixed)."
    else
        echo "STOP: verify the finding is genuinely unfixed before spawning a fix agent (it may be a duplicate of an already-shipped fix)."
    fi
    return "$found"
}

run_check() {
    local ref="$1" token="${2:-}"
    command -v gh  >/dev/null 2>&1 || { echo "finding-already-fixed: gh not on PATH" >&2; return 2; }
    command -v git >/dev/null 2>&1 || { echo "finding-already-fixed: git not on PATH" >&2; return 2; }
    # CLOSED issues that mention the finding ref (title or body). gh search is
    # best-effort; a network/auth failure must not crash the caller.
    local closed=""
    closed="$(gh issue list --state closed --search "$ref" --json number,title \
                --jq '.[] | "#\(.number) CLOSED \(.title)"' 2>/dev/null || true)"
    local hits=""
    if [ -n "$token" ]; then
        hits="$(git log -S "$token" --oneline origin/develop 2>/dev/null | head -5 || true)"
    fi
    verdict_from_signals "$closed" "$hits"
}

run_selftest() {
    local rc=0
    # selftest: asserts-failure — the NO-signal case must return 1 (proceed),
    # i.e. the verdict core must NOT claim already-fixed when both signals are empty.
    if verdict_from_signals "" "" >/dev/null 2>&1; then
        echo "finding-already-fixed selftest: FAIL — empty signals wrongly reported already-fixed" >&2
        rc=1
    fi
    # Positive: a CLOSED-issue signal must return 0 (stop).
    if ! verdict_from_signals "#954 CLOSED tickets_v2 migration key" "" >/dev/null 2>&1; then
        echo "finding-already-fixed selftest: FAIL — a closed-issue signal was not detected" >&2
        rc=1
    fi
    # Positive: a develop-token signal must return 0 (stop).
    if ! verdict_from_signals "" "0a73c249 fix: tickets_v2 resolved key" >/dev/null 2>&1; then
        echo "finding-already-fixed selftest: FAIL — a develop-token signal was not detected" >&2
        rc=1
    fi
    [ "$rc" -eq 0 ] && echo "finding-already-fixed --selftest: PASS"
    return "$rc"
}

case "${1:-}" in
    --selftest) run_selftest ;;
    -h|--help|"") echo "usage: finding-already-fixed.sh <finding-ref> [<fix-grep-token>] | --selftest" >&2; exit 2 ;;
    *) run_check "$1" "${2:-}" ;;
esac
