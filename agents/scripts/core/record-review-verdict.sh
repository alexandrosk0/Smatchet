#!/usr/bin/env bash
# record-review-verdict.sh — stamp the pre-push review-verdict marker for HEAD
# and print the head-bound PR-body verdict line.
#
# The review step of the [pre-first-push gate] leaves no trace, and a recorded
# verdict that names no commit survives pushes it never covered
# (review-verdict-not-bound-to-head, process 2026-08-13: one verdict rode
# through six review-fix pushes). This script is the single stamping point for
# both enforcement layers:
#   1. Prints `adversarial-code-review: <tail> (head=<sha>)` for the PR body —
#      CI (doc-validation.yml `Intent section`) rejects a body whose head= does
#      not match the PR head, so every push invalidates the prior verdict.
#   2. Writes `$GIT_DIR/review-verdict-<head-sha>` — the pre-push hook
#      (scripts/git-hooks/pre-push, section E) refuses to push a feature branch
#      whose HEAD carries no marker.
# Neither layer can prove the review RAN; they prove a claim was recorded for
# THIS commit, which makes a skipped or stale review visible instead of silent.
#
# Usage:
#   bash record-review-verdict.sh "N findings, <disposition>"   # e.g. "4 findings, all fixed"
#   bash record-review-verdict.sh "n/a — <reason>"              # e.g. "n/a — one-line doc fix"
#   bash record-review-verdict.sh --selftest
#
# Exit: 0 = tail validated + marker written + line printed; 1 = tail rejected
# by the verdict grammar (unfilled placeholders included); 2 = usage / not a
# git repo / no HEAD.
#
# selftest: asserts-failure
set -euo pipefail

SELF_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SELF="$SELF_DIR/$(basename "${BASH_SOURCE[0]}")"
CHECKER="$SELF_DIR/check-pr-intent.sh"

_record() {
    local tail_text="$1"
    local gitdir head line body out
    gitdir="$(git rev-parse --git-dir 2>/dev/null)" \
        || { echo "record-review-verdict: not inside a git repository" >&2; return 2; }
    head="$(git rev-parse --verify HEAD 2>/dev/null)" \
        || { echo "record-review-verdict: no HEAD commit to bind the verdict to" >&2; return 2; }
    line="adversarial-code-review: ${tail_text} (head=${head:0:12})"
    # Validate through the REAL checker (never a second copy of the grammar —
    # the CI copy already drifted once; see --check-workflow-sync there), so an
    # unfilled placeholder tail ("<disposition>", "<reason …>") is rejected here
    # at recording time, not later on the PR. The synthetic body exists only to
    # satisfy the checker's Intent requirement; PR_HEAD_SHA exercises the head
    # binding against the sha we just stamped.
    body="## Intent"$'\n\n'"(record-review-verdict validation shim)"$'\n\n'"$line"
    if ! out="$(printf '%s' "$body" | PR_HEAD_SHA="$head" bash "$CHECKER" 2>&1)"; then
        echo "record-review-verdict: verdict tail rejected by the verdict grammar:" >&2
        printf '%s\n' "$out" | sed 's/^/    /' >&2
        echo "record-review-verdict: expected \"N findings, <disposition>\" or \"n/a — <reason>\"" >&2
        echo "  with the placeholders FILLED (ship-loops.md § [pre-first-push gate] item 5)." >&2
        return 1
    fi
    printf '%s\n' "$line" > "$gitdir/review-verdict-$head"
    printf '%s\n' "$line"
}

run_selftest() {
    local tmpd out head marker
    tmpd="$(mktemp -d)" \
        || { echo "record-review-verdict --selftest: FAIL — mktemp failed" >&2; return 1; }
    if ! git -C "$tmpd" init -q \
        || ! git -C "$tmpd" -c user.email=selftest@local -c user.name=selftest \
               -c commit.gpgsign=false commit -q --allow-empty -m seed; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — could not build the temp repo" >&2
        return 1
    fi
    head="$(git -C "$tmpd" rev-parse HEAD)"
    marker="$(git -C "$tmpd" rev-parse --git-dir)"
    case "$marker" in /*) ;; *) marker="$tmpd/$marker" ;; esac
    marker="$marker/review-verdict-$head"
    # Rejection cases run FIRST — they must leave NO marker behind, and the
    # ordering makes that assertable (the pass case below stamps this same path).
    # An unfilled findings-form placeholder MUST be rejected, for the grammar
    # reason, with nothing stamped.
    out="$(cd "$tmpd" && bash "$SELF" "0 findings, <disposition>" 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an unfilled findings placeholder" >&2
        return 1
    }
    case "$out" in
        *"rejected by the verdict grammar"*) ;;
        *) rm -rf "$tmpd"
           echo "record-review-verdict --selftest: FAIL — placeholder tail rejected for the wrong reason:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ -e "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — marker stamped despite a rejected tail" >&2
        return 1
    fi
    # The unfilled n/a placeholder MUST be rejected too.
    out="$(cd "$tmpd" && bash "$SELF" "n/a — <reason the diff is trivial>" 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an unfilled n/a placeholder" >&2
        return 1
    }
    if [ -e "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — marker stamped despite a rejected n/a tail" >&2
        return 1
    fi
    # An empty tail is a usage error, not a stamp.
    out="$(cd "$tmpd" && bash "$SELF" "" 2>&1)" && {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — accepted an empty tail" >&2
        return 1
    }
    # A valid tail MUST stamp the marker and print the head-bound line.
    out="$(cd "$tmpd" && bash "$SELF" "0 findings")" || {
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — rejected a valid bare-count tail" >&2
        return 1
    }
    case "$out" in
        *"(head=${head:0:12})"*) ;;
        *) rm -rf "$tmpd"
           echo "record-review-verdict --selftest: FAIL — printed line is not bound to HEAD:" >&2
           printf '%s\n' "$out" | sed 's/^/    /' >&2
           return 1 ;;
    esac
    if [ ! -f "$marker" ]; then
        rm -rf "$tmpd"
        echo "record-review-verdict --selftest: FAIL — valid tail left no marker at $marker" >&2
        return 1
    fi
    rm -rf "$tmpd"
    echo "record-review-verdict --selftest: PASS"
}

case "${1:-}" in
    --selftest) run_selftest; exit $? ;;
    --help | -h) sed -n '2,27p' "$0"; exit 0 ;;
    "")
        echo "record-review-verdict: missing verdict tail." >&2
        echo "  usage: bash record-review-verdict.sh \"N findings, <disposition>\" | \"n/a — <reason>\"" >&2
        exit 2
        ;;
    *) _record "$1" ;;
esac
