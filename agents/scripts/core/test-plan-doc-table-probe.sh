#!/bin/bash
# test-plan-doc-table-probe: bucket-A self-test for
# scripts/dev/plan-doc-table-probe.sh.
#
# Auto-enrolled by scripts/dev/test-all.sh.
#
# Cases:
#   1. Plan with no § File-level changes section -> exit 0 + no-tables-found
#   2. Plan with file-level section but no recognised columns -> exit 0
#   3. Plan with one valid symbol (real call site) -> exit 0
#   4. Plan with one missing symbol (does NOT exist in tree) -> exit 1
#   5. Plan with one CMake GLOB-populated variable -> exit 0 (resolves via grep)
#   6. Mixed plan (1 valid + 1 invalid symbol) -> exit 1 + miss line for invalid
#
# Plan: docs/plans/shipped/process-backlog-tighten-1-2-3-9-11-12.md § Slice 4

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
PROBE="$REPO_ROOT/agents/scripts/core/plan-doc-table-probe.sh"

if [ ! -x "$PROBE" ]; then
    echo "FAIL: $PROBE not executable"
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

# Run probe from REPO_ROOT (git grep + CMakeLists.txt grep need to be there).
run_probe() {
    local plan_path="$1"
    local out exit_code
    set +e
    out=$(cd "$REPO_ROOT" && bash "$PROBE" "$plan_path" 2>&1)
    exit_code=$?
    set -e
    echo "EXIT=$exit_code"
    echo "$out"
}

assert_exit() {
    local label="$1"
    local expected="$2"
    local got_output="$3"
    local actual
    actual=$(echo "$got_output" | grep -oE 'EXIT=[0-9]+' | head -1 | cut -d= -f2)
    if [ "$actual" = "$expected" ]; then
        echo "PASS: $label (exit=$actual)"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label (expected exit=$expected, got exit=$actual)"
        echo "--- output ---"
        echo "$got_output"
        echo "--- end ---"
        FAIL=$((FAIL + 1))
    fi
}

assert_contains() {
    local label="$1"
    local needle="$2"
    local hay="$3"
    if echo "$hay" | grep -qF "$needle"; then
        echo "PASS: $label (contains \"$needle\")"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label (expected substring \"$needle\")"
        FAIL=$((FAIL + 1))
    fi
}

# Test 1: plan with no § File-level changes section.
cat > "$TMP/plan-1.md" <<'EOF'
# Plan with no file-level section

## Summary
Just some prose, no table.
EOF
out=$(run_probe "$TMP/plan-1.md")
assert_exit "1. no file-level section -> exit 0" 0 "$out"
assert_contains "1b. no-tables-found logged" "no-tables-found" "$out"

# Test 2: plan with file-level section but no recognised columns.
cat > "$TMP/plan-2.md" <<'EOF'
# Plan with unrecognised columns

## File-level changes

| Random | Headers |
|--------|---------|
| foo    | bar     |
EOF
out=$(run_probe "$TMP/plan-2.md")
assert_exit "2. unrecognised columns -> exit 0" 0 "$out"

# Test 3: plan with one valid symbol (use a known one from the repo).
cat > "$TMP/plan-3.md" <<'EOF'
# Plan with one valid symbol

## File-level changes

| Symbol                        | Role        |
|-------------------------------|-------------|
| `setup_claude_code`           | (definer)   |
EOF
out=$(run_probe "$TMP/plan-3.md")
assert_exit "3. valid symbol -> exit 0" 0 "$out"

# Test 4: plan with one missing symbol. Use a runtime-generated random
# token so this test script itself doesn't accidentally contain the
# string the probe greps for (self-reference would make git grep hit
# this very file).
BOGUS_4="zz_bogus_${RANDOM}_${RANDOM}_${RANDOM}_x"
cat > "$TMP/plan-4.md" <<EOF
# Plan with one missing symbol

## File-level changes

| Symbol      | Role      |
|-------------|-----------|
| \`$BOGUS_4\` | (caller) |
EOF
out=$(run_probe "$TMP/plan-4.md")
assert_exit "4. missing symbol -> exit 1" 1 "$out"
assert_contains "4b. MISS line present" "MISS" "$out"

# Test 5: plan with one CMake variable (verify resolves via grep).
cat > "$TMP/plan-5.md" <<'EOF'
# Plan with CMake variable

## File-level changes

| CMake variable | Role       |
|----------------|------------|
| `CORE_SOURCES` | (list-add) |
EOF
out=$(run_probe "$TMP/plan-5.md")
# CORE_SOURCES exists in the tracked CMake build files (defined in
# Source/Core/CMakeLists.txt since the per-component split; the probe greps
# every tracked *CMakeLists.txt + cmake/*.cmake) — should resolve.
if git -C "$REPO_ROOT" grep -q "CORE_SOURCES" -- '*CMakeLists.txt' 'cmake/*.cmake' 2>/dev/null; then
    assert_exit "5. CMake variable CORE_SOURCES -> exit 0" 0 "$out"
else
    echo "SKIP: 5. CORE_SOURCES not in CMakeLists.txt (repo changed); skipping"
fi

# Test 6: mixed plan (1 valid + 1 invalid). Runtime-generated bogus token,
# distinct from Test 4's.
BOGUS_6="zz_bogus_${RANDOM}_${RANDOM}_${RANDOM}_y"
cat > "$TMP/plan-6.md" <<EOF
# Mixed plan

## File-level changes

| Symbol                | Role      |
|-----------------------|-----------|
| \`setup_claude_code\` | (definer) |
| \`$BOGUS_6\`          | (caller)  |
EOF
out=$(run_probe "$TMP/plan-6.md")
assert_exit "6. mixed valid+invalid -> exit 1" 1 "$out"
assert_contains "6b. MISS line for invalid symbol" "$BOGUS_6" "$out"

echo
echo "================================================================"
echo "test-plan-doc-table-probe: $PASS passed, $FAIL failed"
echo "================================================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
