#!/usr/bin/env bash
# test-ui-bulk-field-edit.sh — bucket-E driver for the grid's "Set <Field> for N rows..." bulk edit
# (tests/ui/bulk_field_edit.test.cpp, docs/plans/bulk-edit-rows.md P1). Generates a 12-issue Jira fixture with a
# priority catalog into the throwaway profile, runs `ui_test.run --name=BulkFieldEdit` against an ephemeral spawn
# and parses the JSON envelope.
#
# Covers through the real UI (right-click menu, popup, confirm button): the menu item's visibility rules, a pick that
# saves every row, the confirm step above 10 rows with uneditable / unchanged rows listed as skipped, and the offline
# queue path. Asserts the backend saw one update per row, the job summary, the summary toast and the kept selection.
#
# Spawn-warmup retry: the headless `--spawn` runner intermittently launches the ephemeral app "cold" and the whole
# queued test set fails 0-of-N (spawn_warmup_deterministic_gate.test.cpp). Retry ONLY on that wipeout signal
# (passed==0 && failed==tested), never on a partial failure.
#
# Exit codes:
#   0 — every test passed
#   1 — at least one test failed (genuine)
#   2 — binary missing OR build had SMATCHET_BUILD_UI_TESTS=OFF

set -euo pipefail

# Shared bucket-E preamble (exe staleness guard, throwaway profile, wedge-proof capture).
# shellcheck source=scripts/dev/lib/ui-test-driver.sh
. "$(dirname "$0")/lib/ui-test-driver.sh"

EXE="${SMATCHET_EXE:-build/ninja-ui-test-msvc/Smatchet.exe}"
PY="${PYTHON:-python}"
TEST_PORT="${SMATCHET_TEST_PORT:-58760}"
FILTER="${UI_TEST_FILTER:-BulkFieldEdit}"
MAX_ATTEMPTS="${UI_TEST_WARMUP_RETRIES:-8}"

if [ ! -f "$EXE" ]; then
    echo "FAIL: $EXE not found. Build with: cmake --build --preset ninja-ui-test-msvc --target SmatchetStandalone" >&2
    exit 2
fi

ui_test_require_fresh_exe "$EXE" || exit 2

# A loopback token requirement from the caller's shell would turn the CLI's own calls into 401s.
unset SMATCHET_MCP_REQUIRE_TOKEN_ON_LOOPBACK

# Seeded profile: a fresh one shows the Whisper setup banner over the grid and starts read-only.
ui_test_isolate_home --seed

# Native Windows path for both python and the exe (the throwaway dir is an MSYS path under Git Bash).
HOME_NATIVE="$(cygpath -m "$UI_TEST_HOME" 2>/dev/null || echo "$UI_TEST_HOME")"
FIXTURE="$HOME_NATIVE/bulk-edit-rows.json"

# 12 issues: odd keys High (id 2), even keys Medium (id 3). The priority catalog gives the bulk edit its option list;
# summary is a text field the bulk edit must not offer.
"$PY" - "$FIXTURE" <<'PYEOF'
import json
import sys

issues = []
for n in range(1, 13):
    prio = {"name": "High", "id": "2"} if n % 2 == 1 else {"name": "Medium", "id": "3"}
    issues.append({
        "key": "SMAT-%d" % n,
        "fields": {
            "summary": "Bulk edit issue %d" % n,
            "status": {"name": "Open", "id": "1"},
            "priority": prio,
            "assignee": {"displayName": "Alice Tester"},
            # SMAT-12 is its own type: editmeta is cached per type, so marking one issue read-only for a field must
            # not turn every other row read-only with it.
            "issuetype": {"name": "Bug" if n == 12 else "Task"},
        },
    })
fixture = {
    "name": "bulk-edit-rows",
    "reachability": {"kind": "AuthenticatedReachable", "diagnostic": ""},
    "catalog": {
        "fields": [
            {"id": "priority", "name": "Priority", "family": "SelectSingle", "options": [
                {"id": "1", "value": "Critical"},
                {"id": "2", "value": "High"},
                {"id": "3", "value": "Medium"},
                {"id": "4", "value": "Low"},
                {"id": "5", "value": "Trivial"},
            ]},
            {"id": "summary", "name": "Summary", "family": "Text", "options": []},
        ],
        "components": [],
        "issueTypeMeta": [],
        "users": [],
    },
    "fetches": [{
        "fullSyncCompleted": True,
        "warning": "",
        "fetchError": "",
        "selectedFields": ["summary", "status", "priority", "assignee", "issuetype"],
        "jiraSearchPages": [{"issues": issues, "isLast": True}],
    }],
    "mutations": {"updateIssueFields": [], "createIssue": []},
}
with open(sys.argv[1], "w", encoding="utf-8") as f:
    json.dump(fixture, f, indent=2)
PYEOF

field() {
    # field <json> <key> — extract data.<key>, default "?".
    echo "$1" | "$PY" -c "import sys,json; d=json.load(sys.stdin); print(d.get('data',{}).get('$2','?'))" 2>/dev/null || echo "?"
}

PASSED="?"; FAILED="?"; TESTED="?"; LOG="?"
attempt=1
while [ "$attempt" -le "$MAX_ATTEMPTS" ]; do
    PORT=$((TEST_PORT + attempt - 1))
    echo "[test-ui-bulk-field-edit] attempt $attempt/$MAX_ATTEMPTS — launching ephemeral Smatchet (port $PORT)..."
    echo "  fixture: $FIXTURE"
    RAW_OUTPUT="$(SMATCHET_TEST_JIRA_BACKEND_FIXTURE="$FIXTURE" \
        "$EXE" cmd ui_test.run --name="$FILTER" --spawn --yes \
        --mcp-port="$PORT" ${UI_TEST_OUTLOG:+--outLog="$UI_TEST_OUTLOG"} 2>&1 || true)"
    echo "$RAW_OUTPUT" | tail -12

    JSON_LINE="$(echo "$RAW_OUTPUT" | grep -oE '\{.*\}' | tail -1 || true)"
    if [ -z "$JSON_LINE" ]; then
        echo "  (no JSON envelope this attempt)"
        attempt=$((attempt + 1)); continue
    fi

    PASSED="$(field "$JSON_LINE" passed)"
    FAILED="$(field "$JSON_LINE" failed)"
    TESTED="$(field "$JSON_LINE" tested)"
    LOG="$(field "$JSON_LINE" log)"

    if [ "$LOG" = "build had SMATCHET_BUILD_UI_TESTS=OFF" ]; then
        echo "FAIL: build had SMATCHET_BUILD_UI_TESTS=OFF — rebuild with ninja-ui-test-msvc." >&2
        echo "Passed: 0  Failed: 0"
        exit 2
    fi

    # Whole-suite wipeout (passed==0 && failed==tested && tested>0) == cold-spawn warmup miss → retry.
    if [ "$PASSED" = "0" ] && [ "$FAILED" = "$TESTED" ] && [ "$TESTED" != "0" ] && [ "$TESTED" != "?" ]; then
        echo "  cold-spawn wipeout (0/$TESTED) — retrying warmup..."
        attempt=$((attempt + 1)); continue
    fi
    break
done

echo
echo "Result: tested=$TESTED passed=$PASSED failed=$FAILED log=$LOG"

if [ "$PASSED" = "?" ] || [ "$FAILED" = "?" ]; then
    echo "FAIL: could not extract pass/fail counts after $MAX_ATTEMPTS attempts (spawn-warmup flake)." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

# Zero-run floor: a run that produces ZERO results must not exit green (filter typo / dropped registration).
if [ "$PASSED" = "0" ] && [ "$FAILED" = "0" ]; then
    echo "FAIL: ui_test.run matched 0 tests for filter '$FILTER' — registration or filter problem." >&2
    echo "Passed: 0  Failed: 1"
    exit 1
fi

echo "Passed: $PASSED  Failed: $FAILED"
if [ "$FAILED" != "0" ]; then
    exit 1
fi
exit 0
