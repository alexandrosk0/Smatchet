#!/usr/bin/env bash
# test-config-migration.sh — bucket-A wrapper for the config schema-migration doctest cases.
#
# Runs the ConfigMigration + ConfigManager test cases from SmatchetTests.exe and asserts
# every fixture under tests/fixtures/config/v*.json + truncated.json + garbage.json loads
# cleanly (Pillar 3: never crash) and the migration markers (`migrated_inherit_issuetype_v1`,
# `layout_schema_version`) round-trip per spec.
#
# Why doctest-via-shell rather than the standalone-exe + fixture-copy loop originally sketched
# in docs/plans/shipped/test-suite-expansion-completion.md § Phase 4: ConfigManager does not consult
# any `SMATCHET_USER_DATA` env var on Load(), so a child exe cannot be isolated to a fixture
# dir without modifying production code (out of scope for Phase 4). The doctest rig achieves
# the same coverage with per-TEST_CASE isolation via TestEnvGuard.h.
#
# Final summary line "Passed: N  Failed: M" is consumed by scripts/dev/test-all.sh.
#
# Exit codes:
#   0 — every assertion passed
#   1 — at least one assertion failed
#   2 — build missing and cmake/build failed

set -euo pipefail

command -v cmake >/dev/null 2>&1 || { echo "cmake required" >&2; exit 2; }

cd "$(dirname "$0")/../.."

TESTS_EXE="${SMATCHET_TESTS_EXE:-build/ninja-test-msvc/tests/SmatchetTests.exe}"

if [ ! -x "$TESTS_EXE" ]; then
    echo "SmatchetTests.exe missing at $TESTS_EXE — configuring + building..."
    cmake --preset ninja-test-msvc >/dev/null 2>&1 || { echo "cmake preset failed"; exit 2; }
    cmake --build --preset ninja-test-msvc --target SmatchetTests 2>&1 | tail -5 || {
        echo "build failed"
        exit 2
    }
fi

if [ ! -x "$TESTS_EXE" ]; then
    echo "SmatchetTests.exe still missing after build at $TESTS_EXE"
    exit 2
fi

FIXTURES_DIR="tests/fixtures/config"
if [ ! -d "$FIXTURES_DIR" ]; then
    echo "fixtures dir missing: $FIXTURES_DIR"
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Sanity: every expected fixture file is present and non-empty.
FIXTURE_COUNT=0
for f in "$FIXTURES_DIR"/v*.json "$FIXTURES_DIR"/truncated.json "$FIXTURES_DIR"/garbage.json; do
    [ -f "$f" ] || continue
    if [ ! -s "$f" ] && [ "$(basename "$f")" != "garbage.json" ]; then
        # garbage.json is allowed to be small; others must have content.
        echo "fixture is empty: $f"
        echo "Passed: 0  Failed: 1"
        exit 1
    fi
    FIXTURE_COUNT=$((FIXTURE_COUNT + 1))
done

if [ "$FIXTURE_COUNT" -lt 4 ]; then
    echo "expected >=4 fixtures (v*.json + truncated.json + garbage.json); found $FIXTURE_COUNT"
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Tell the doctest CASE that resolves fixture paths where to look. The test already probes
# several relative-path candidates, but a deterministic env var is the cleanest in CI.
# Split declare + assign per SC2155 — `local`/`export var=$(...)` masks the command exit
# code, so a `pwd` failure here would silently set an empty path instead of erroring.
SMATCHET_TEST_FIXTURES="$(pwd)/$FIXTURES_DIR"
export SMATCHET_TEST_FIXTURES

OUT=$("$TESTS_EXE" --test-case='ConfigMigration*' --reporters=console 2>&1 || true)
echo "$OUT"

ASSERTIONS_LINE=$(echo "$OUT" | grep -E '^\[doctest\] assertions:' | tail -n 1 || true)
if [ -z "$ASSERTIONS_LINE" ]; then
    echo "Passed: 0  Failed: 1"
    echo "ERROR: no doctest assertions summary line found"
    exit 1
fi

PASSED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +passed.*/\1/')
FAILED=$(echo "$ASSERTIONS_LINE" | sed -E 's/.*\| *([0-9]+) +failed.*/\1/')

echo "Fixtures checked: $FIXTURE_COUNT"
echo "Passed: $PASSED  Failed: $FAILED"

# Zero-run floor (fail-open shape Z): a run that produces ZERO results leaves
# PASSED=FAILED=0 and would exit green - a vanished/unparsed suite passing.
if [ "$PASSED" -eq 0 ] && [ "$FAILED" -eq 0 ]; then
    echo "$(basename "$0" .sh): FAIL - the test run produced ZERO results (vanished / unparsed)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi
if [ "$FAILED" -gt 0 ]; then
    exit 1
fi
exit 0
