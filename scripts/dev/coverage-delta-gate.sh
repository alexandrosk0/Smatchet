#!/usr/bin/env bash
# coverage-delta-gate.sh — refuse Source/Core/ diffs without matching test deltas.
#
# Per-PR enforcement: if the current branch's diff against `develop` (or the
# configured base) touches any production file under Source/Core/src/ AND
# touches zero test files under tests/, the gate exits 1 with a diagnostic.
#
# The CI workflow checks for the `tests-out-of-band` PR label and dismisses
# this gate when present; the bash script itself is label-unaware (label
# inspection requires the GitHub event payload, which is workflow-level).
#
# Override mechanism for local runs:
#   SMATCHET_COVERAGE_GATE_BASE   base ref to diff against. Default: origin/develop
#                                 (falls back to develop, then HEAD~1).
#   SMATCHET_COVERAGE_GATE_BYPASS set to 1 to short-circuit (advisory mode).
#
# Exit codes:
#   0 — gate satisfied (no Source/Core change, or test files also changed)
#   1 — gate failed (Source/Core changed without test delta)

set -euo pipefail

cd "$(dirname "$0")/../.."

if [ "${SMATCHET_COVERAGE_GATE_BYPASS:-0}" = "1" ]; then
    echo "[coverage-delta-gate] BYPASS active (SMATCHET_COVERAGE_GATE_BYPASS=1)"
    exit 0
fi

# Resolve the base ref. Prefer the merge-base against the named remote/develop
# so the diff reflects only the PR's contribution, not develop drift since
# branching.
BASE_REF="${SMATCHET_COVERAGE_GATE_BASE:-}"
if [ -z "$BASE_REF" ]; then
    for candidate in origin/develop develop HEAD~1; do
        if git rev-parse --verify --quiet "$candidate" >/dev/null 2>&1; then
            BASE_REF="$candidate"
            break
        fi
    done
fi
if [ -z "$BASE_REF" ]; then
    echo "[coverage-delta-gate] no usable base ref; skipping gate" >&2
    exit 0
fi

MERGE_BASE=$(git merge-base "$BASE_REF" HEAD 2>/dev/null || echo "$BASE_REF")

# Compute the diff once. --name-only --diff-filter=ACMR keeps adds, copies,
# modifies, renames (the cases that actually change content). Deletes intentionally
# excluded — removing a production file shouldn't require a new test.
mapfile -t CHANGED < <(git diff --name-only --diff-filter=ACMR "$MERGE_BASE"...HEAD 2>/dev/null || true)

if [ "${#CHANGED[@]}" -eq 0 ]; then
    echo "[coverage-delta-gate] no changed files vs $BASE_REF; gate passes"
    exit 0
fi

PROD_CHANGES=()
TEST_CHANGES=()

for f in "${CHANGED[@]}"; do
    case "$f" in
        # Production surface that the gate cares about. Source/Core/src/*.cpp is
        # the core enforcement target; *.h headers under Source/Core/include/ are
        # treated as docs-or-API-shape (different review surface) so they don't
        # require a paired test delta on their own.
        Source/Core/src/*.cpp)
            PROD_CHANGES+=("$f") ;;
        # Test surface — only actual test TUs count toward a delta. tests/support/*.h
        # (shared fixtures / helpers) was previously included but is trivially
        # dismissable (add an empty header to "satisfy" the gate). Restrict to the
        # per-test-file delta the gate was designed to enforce.
        tests/Core/*.test.cpp|tests/Lua/*.test.cpp|tests/Plugins/*.test.cpp|tests/Plugins/Mcp/*.test.cpp)
            TEST_CHANGES+=("$f") ;;
    esac
done

echo "[coverage-delta-gate] base ref:     $BASE_REF (merge-base $MERGE_BASE)"
echo "[coverage-delta-gate] prod changes: ${#PROD_CHANGES[@]}"
echo "[coverage-delta-gate] test changes: ${#TEST_CHANGES[@]}"

if [ "${#PROD_CHANGES[@]}" -eq 0 ]; then
    echo "[coverage-delta-gate] PASS — no production Source/Core/src/*.cpp changes"
    exit 0
fi

if [ "${#TEST_CHANGES[@]}" -gt 0 ]; then
    echo "[coverage-delta-gate] PASS — production + test files both changed"
    exit 0
fi

# Production-only change with no test delta. The workflow may still dismiss
# via the tests-out-of-band label; the script's job is to signal the condition.
echo
echo "FAIL: Source/Core/ changes without test deltas."
echo
echo "Changed production files:"
for f in "${PROD_CHANGES[@]}"; do
    echo "  - $f"
done
echo
echo "Add tests under tests/Core/ (or tests/Lua/, tests/Plugins/) for the"
echo "changed units, or apply the 'tests-out-of-band' PR label to dismiss this"
echo "gate for legitimate non-behavioural changes (docs-only, build flag flips,"
echo "include-shape fixes, sanitizer toggles)."
exit 1
