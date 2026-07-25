#!/usr/bin/env bash
# test-small-helper-audit.sh — the sub-floor helper-clone census (gate-blind-spot-sweep Slice 2).
# ----------------------------------------------------------------------------
# Class of debt this makes visible: the SAME small named helper re-rolled body-for-body
# in three or more TUs. dup_audit.py's MIN_CLONE_TOKENS = 70 (~8 lines) is what makes
# the blocking DRY gate viable — lowering it would flood Pillar 5 with every
# `if (!x) return false;` in the tree — so a 3-line helper is invisible to the DRY
# gate BY CONSTRUCTION and the grandfathered 449-clone baseline can never surface it.
#
# The deliberate non-choice: this does NOT lower MIN_CLONE_TOKENS. It is a separate,
# narrower detector that asks a different question (exact body identity of a NAMED
# free function, >= 3 TUs) in the band below dup_audit's floor. The band ceiling is
# IMPORTED from dup_audit.py, so the two can never drift apart or leave a gap.
#
# The detector lives in agents/scripts/core/small_helper_audit.py; this is the
# test-all.sh-discoverable wrapper (auto-enrolled via the `test-*.sh` glob, so it runs
# in the "Agentic self-tests (bats)" CI lane with no workflow edit).
#
# ADVISORY (WARN-first), same posture as test-dead-export-audit.sh and for the same
# reason (ADR-0015's WARN-first -> blocking graduation path): a census that fires on a
# legitimately-repeated idiom must not be able to red the lane. Graduation is a
# separate decision with its own ADR.
#
# Baseline: docs/high-integrity/small-helper-baseline.md.
#
# Modes:
#   (no args) | --check   scan the real tree; WARN on non-baselined groups; PASS.
#   --report              human-readable list of every current group + member.
#   --baseline            regenerate the baseline file in place (then commit it).
#   --selftest            dogfood on synthetic sources: a copy-then-rename MUST collide,
#                         a genuinely different body MUST NOT, and an `if (...) {` MUST
#                         NOT be extracted as a function. Asserts a failure case.
#
# Exit: 0 clean / advisory-WARN · 1 selftest assertion failed · 2 infra error.
# Goes through test-shell-lint.sh + the shellcheck -S warning fail-set.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "test-small-helper-audit: cannot resolve repo root (git required)" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
}
cd "$ROOT" || { echo "test-small-helper-audit: cannot cd to repo root" >&2; exit 2; }

AUDIT="$ROOT/agents/scripts/core/small_helper_audit.py"
BASELINE="docs/high-integrity/small-helper-baseline.md"

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
if [ -z "$PY" ]; then
    echo "test-small-helper-audit: no working python interpreter (python3/python/py)." >&2
    echo "  See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi
if [ ! -f "$AUDIT" ]; then
    echo "test-small-helper-audit: detector missing: $AUDIT" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

# selftest: asserts-failure — small_helper_audit.py --selftest feeds the extractor
# known-bad inputs (an `if (...) {` head, an out-of-line member definition, a body at
# or above dup_audit's floor, a sub-band getter) and FAILS if any is accepted.
run_selftest() {
    local out
    if ! out="$("$PY" "$AUDIT" --selftest 2>&1)"; then
        echo "test-small-helper-audit --selftest: FAIL — detector invariants broken:" >&2
        printf '%s\n' "$out" >&2
        echo "Passed: 0  Failed: 1"
        return 1
    fi
    printf '%s\n' "$out"
    echo "Passed: 1  Failed: 0"
    return 0
}

run_baseline() {
    local out
    if ! out="$("$PY" "$AUDIT" --baseline-md 2>&1)"; then
        echo "test-small-helper-audit: FAIL — --baseline-md errored:" >&2
        printf '%s\n' "$out" >&2
        return 2
    fi
    mkdir -p "$(dirname "$BASELINE")" || return 2
    printf '%s\n' "$out" > "$BASELINE" || return 2
    echo "test-small-helper-audit: regenerated $BASELINE — review + commit it."
    return 0
}

case "${1:---check}" in
    --selftest)
        run_selftest
        exit $?
        ;;
    --baseline)
        run_baseline
        exit $?
        ;;
    --report)
        "$PY" "$AUDIT"
        exit $?
        ;;
    --check|"")
        OUT="$("$PY" "$AUDIT" --check 2>&1)"
        RC=$?
        printf '%s\n' "$OUT"
        if [ "$RC" -ge 2 ]; then
            echo "test-small-helper-audit: infra error (exit $RC) — failing closed." >&2
            echo "Passed: 0  Failed: 0"
            exit 2
        fi
        # ADVISORY: WARN lines are informational and never fail the lane.
        WARNS=$(printf '%s\n' "$OUT" | grep -c '^\[small-helper\] WARN' || true)
        if [ "$WARNS" -gt 0 ]; then
            echo "test-small-helper-audit: ADVISORY — $WARNS non-baselined helper group(s) above."
            echo "  Fix by migrating the call sites onto one shared helper, or grandfather it:"
            echo "    bash agents/scripts/core/test-small-helper-audit.sh --baseline"
        fi
        echo "Passed: 1  Failed: 0"
        exit 0
        ;;
    *)
        echo "test-small-helper-audit: unknown argument '$1'" >&2
        echo "  usage: test-small-helper-audit.sh [--check|--report|--baseline|--selftest]" >&2
        echo "Passed: 0  Failed: 0"
        exit 2
        ;;
esac
