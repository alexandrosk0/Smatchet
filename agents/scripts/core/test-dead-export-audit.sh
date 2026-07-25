#!/usr/bin/env bash
# test-dead-export-audit.sh — the cross-TU dead-export gate (gate-blind-spot-sweep Slice 1).
# ----------------------------------------------------------------------------
# Class of debt this makes visible: a free function DECLARED in a public
# `Source/Core/include/**` header, DEFINED in some .cpp, and called by NOBODY.
# `-Wunused-function` only sees the static-in-one-TU shape; 40-unused-config-guard.sh
# only sees config keys; check-test-list.sh only sees orphan test files. A symbol
# whose declaration, definition and zero callers are spread across three files is
# structurally invisible to every gate in the tree — this one closes that.
#
# The detector lives in agents/scripts/core/dead_export_audit.py; this is the
# test-all.sh-discoverable wrapper (auto-enrolled via the `test-*.sh` glob, so it
# runs in the "Agentic self-tests (bats)" CI lane with no workflow edit).
#
# ADVISORY (WARN-first), mirroring the DRY gate's pre-graduation posture per
# docs/adr/0015-dry-quality-pillar-duplication-gate.md: `--check` prints
# `[dead-export] WARN ...` for findings absent from the grandfathered baseline and
# still reports PASS. It cannot red the lane on a detector false positive, which is
# the point — the textual audit is a floor, not a ceiling (see the .py docstring's
# § Accepted blind spots). Graduation to blocking is a separate decision with its
# own ADR once the baseline is clean and the false-positive rate is known.
#
# Baseline: docs/high-integrity/dead-export-baseline.md (same shape + regenerate
# convention as dup-baseline.md).
#
# Modes:
#   (no args) | --check   scan the real tree; WARN on non-baselined findings; PASS.
#   --report              human-readable list of every current finding.
#   --baseline            regenerate the baseline file in place (then commit it).
#   --selftest            dogfood on a throwaway git repo: a planted uncalled export
#                         MUST be flagged and a planted CALLED export MUST NOT be.
#                         Asserts a failure case.
#
# Exit: 0 clean / advisory-WARN · 1 selftest assertion failed · 2 infra error
# (no python, no git, unreadable tree — never a silent pass).
# Goes through test-shell-lint.sh + the shellcheck -S warning fail-set.
# ----------------------------------------------------------------------------
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "test-dead-export-audit: cannot resolve repo root (git required)" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
}
cd "$ROOT" || { echo "test-dead-export-audit: cannot cd to repo root" >&2; exit 2; }

AUDIT="$ROOT/agents/scripts/core/dead_export_audit.py"
BASELINE="docs/high-integrity/dead-export-baseline.md"

# command -v alone is insufficient on Windows: the python3 Store-alias stub passes it
# but exits 49 when run. Probe each candidate; take the first that actually executes.
PY=""
for _c in python3 python py; do
    _p="$(command -v "$_c" 2>/dev/null)" || continue
    if "$_p" -c "" >/dev/null 2>&1; then PY="$_p"; break; fi
done
if [ -z "$PY" ]; then
    echo "test-dead-export-audit: no working python interpreter (python3/python/py)." >&2
    echo "  See BUILD.md § Dev-script CLI tools." >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi
if [ ! -f "$AUDIT" ]; then
    echo "test-dead-export-audit: detector missing: $AUDIT" >&2
    echo "Passed: 0  Failed: 0"
    exit 2
fi

# ---------------------------------------------------------------------------
# --selftest — plant a KNOWN-BAD tree and assert the detector flags it.
#
# selftest: asserts-failure — the first fixture below is a genuine failure case
# (an export with no caller anywhere); if the detector regresses to reporting
# nothing, this selftest fails rather than passing green.
# ---------------------------------------------------------------------------
run_selftest() {
    local tmp rc=0 out
    tmp="$(mktemp -d)" || { echo "test-dead-export-audit: mktemp failed" >&2; return 2; }
    # shellcheck disable=SC2064  # expand $tmp now: the trap must survive the local going out of scope
    trap "rm -rf '$tmp'" RETURN

    # The detector's own unit-level invariants (parser + partition rules).
    if ! out="$("$PY" "$AUDIT" --selftest 2>&1)"; then
        echo "test-dead-export-audit --selftest: FAIL — detector invariants broken:" >&2
        printf '%s\n' "$out" >&2
        return 1
    fi

    # End-to-end fixture: one dead export, one live export, in a throwaway repo.
    mkdir -p "$tmp/Source/Core/include" "$tmp/Source/Core/src" || return 2
    cat > "$tmp/Source/Core/include/Fixture.h" <<'EOF'
#pragma once
#include <string>
std::string PlantedDeadExport(const std::string& in);
std::string PlantedLiveExport(const std::string& in);
EOF
    cat > "$tmp/Source/Core/src/Fixture.cpp" <<'EOF'
#include "Fixture.h"
std::string PlantedDeadExport(const std::string& in) { return in; }
std::string PlantedLiveExport(const std::string& in) { return in; }
EOF
    cat > "$tmp/Source/Core/src/Caller.cpp" <<'EOF'
#include "Fixture.h"
std::string UseIt(const std::string& s) { return PlantedLiveExport(s); }
EOF
    git init -q "$tmp" >/dev/null 2>&1 || return 2
    git -C "$tmp" config user.email t@t.t >/dev/null 2>&1 || return 2
    git -C "$tmp" config user.name t >/dev/null 2>&1 || return 2
    git -C "$tmp" add -A >/dev/null 2>&1 || return 2

    out="$(cd "$tmp" && "$PY" "$AUDIT" --list 2>&1)" || {
        echo "test-dead-export-audit --selftest: FAIL — --list errored on the fixture:" >&2
        printf '%s\n' "$out" >&2
        return 1
    }
    if ! printf '%s\n' "$out" | grep -q 'PlantedDeadExport'; then
        echo "test-dead-export-audit --selftest: FAIL — the planted UNCALLED export was NOT flagged." >&2
        printf '  detector output: %s\n' "$out" >&2
        rc=1
    fi
    if printf '%s\n' "$out" | grep -q 'PlantedLiveExport'; then
        echo "test-dead-export-audit --selftest: FAIL — a CALLED export was flagged (false positive)." >&2
        printf '  detector output: %s\n' "$out" >&2
        rc=1
    fi

    if [ "$rc" -ne 0 ]; then
        echo "Passed: 0  Failed: 1"
        return 1
    fi
    echo "test-dead-export-audit --selftest: PASS — planted dead export flagged, called export not."
    echo "Passed: 1  Failed: 0"
    return 0
}

run_baseline() {
    local out
    if ! out="$("$PY" "$AUDIT" --baseline-md 2>&1)"; then
        echo "test-dead-export-audit: FAIL — --baseline-md errored:" >&2
        printf '%s\n' "$out" >&2
        return 2
    fi
    mkdir -p "$(dirname "$BASELINE")" || return 2
    printf '%s\n' "$out" > "$BASELINE" || return 2
    echo "test-dead-export-audit: regenerated $BASELINE — review + commit it."
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
            echo "test-dead-export-audit: infra error (exit $RC) — failing closed." >&2
            echo "Passed: 0  Failed: 0"
            exit 2
        fi
        # ADVISORY: WARN lines are informational and never fail the lane. Only an
        # infra error (above) or a broken selftest can red this gate today.
        WARNS=$(printf '%s\n' "$OUT" | grep -c '^\[dead-export\] WARN' || true)
        if [ "$WARNS" -gt 0 ]; then
            echo "test-dead-export-audit: ADVISORY — $WARNS non-baselined dead export(s) above."
            echo "  Fix by deleting the symbol, or grandfather it:"
            echo "    bash agents/scripts/core/test-dead-export-audit.sh --baseline"
        fi
        echo "Passed: 1  Failed: 0"
        exit 0
        ;;
    *)
        echo "test-dead-export-audit: unknown argument '$1'" >&2
        echo "  usage: test-dead-export-audit.sh [--check|--report|--baseline|--selftest]" >&2
        echo "Passed: 0  Failed: 0"
        exit 2
        ;;
esac
