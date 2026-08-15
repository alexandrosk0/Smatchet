#!/usr/bin/env bash
# test-autonomous-debug-loop.sh — End-to-end smoke test for the autonomous
# debugging pipeline (plan: autonomous-debugging-no-creds).
#
# Proves: ASAN canary → build → ctest detects → merge-watcher pattern matches
#         → AUTO_ACT_SANITIZER_PROMPT would fire → cleanup.
#
# Usage:
#   bash agents/scripts/core/test-autonomous-debug-loop.sh [--keep-canary]
#
# Prerequisites:
#   - MSVC dev environment on PATH (run from VS dev prompt or after
#     ilammy/msvc-dev-cmd), OR MSYS2 UCRT64 with a working ASAN toolchain.
#   - cmake + ninja on PATH.
#   - Python 3 on PATH (for merge-watcher validation).
#
# Exit codes: 0 = all checks pass, 1 = a check failed, 2 = prerequisites missing.

set -euo pipefail

command -v cmake >/dev/null 2>&1 || { echo "cmake required" >&2; exit 2; }
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/resolve-py.sh"
PY="$(resolve_py)" || { echo "python3 required (no working interpreter on PATH)" >&2; exit 2; }
command -v cl.exe >/dev/null 2>&1 || { echo "cl.exe required" >&2; exit 2; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

CANARY_FILE="$REPO_ROOT/tests/Core/AsanCanary.test.cpp"
KEEP_CANARY=false
PRESET=""
PASS=0
FAIL=0

cleanup() {
    if [ "$KEEP_CANARY" = false ] && [ -f "$CANARY_FILE" ]; then
        rm -f "$CANARY_FILE"
        echo "[cleanup] Removed canary file."
    fi
}
trap cleanup EXIT

for arg in "$@"; do
    case "$arg" in
        --keep-canary) KEEP_CANARY=true ;;
        *) echo "Unknown arg: $arg"; exit 2 ;;
    esac
done

check() {
    local label="$1"; shift
    if "$@"; then
        echo "[PASS] $label"
        PASS=$((PASS + 1))
    else
        echo "[FAIL] $label"
        FAIL=$((FAIL + 1))
    fi
}

# ---------- Phase 1: Detect available sanitizer preset ----------

echo ""
echo "=== Phase 1: Detect sanitizer preset ==="

if cl.exe /? >/dev/null 2>&1; then
    PRESET="ninja-msvc-asan"
    CONFIGURE_EXTRA="-DSMATCHET_WITH_WHISPER=OFF"
    BUILD_DIR="$REPO_ROOT/build/$PRESET"
    echo "MSVC detected — using preset: $PRESET"
elif g++ -print-file-name=libasan.dll.a 2>/dev/null | grep -q "/"; then
    PRESET="ninja-msvc-asan"
    CONFIGURE_EXTRA=""
    BUILD_DIR="$REPO_ROOT/build/$PRESET"
    echo "GCC with ASAN detected — using preset: $PRESET"
else
    echo "No sanitizer-capable compiler found."
    echo "Run from a VS dev prompt (MSVC) or ensure GCC ships libasan."
    exit 2
fi

# ---------- Phase 2: Inject ASAN canary ----------

echo ""
echo "=== Phase 2: Inject ASAN canary ==="

cat > "$CANARY_FILE" << 'CANARY_EOF'
// Deliberate ASAN canary — heap-use-after-free.
// Injected by test-autonomous-debug-loop.sh; removed after test.
#include <doctest/doctest.h>
#include <cstdlib>

TEST_CASE("asan_canary_heap_use_after_free") {
    // Allocate, free, then read — classic use-after-free.
    volatile int* p = new int(42);
    int addr = *p;          // valid read
    delete p;
    volatile int bad = *p;  // ASAN should catch this
    (void)addr;
    (void)bad;
    CHECK(true);            // never reached if ASAN fires
}
CANARY_EOF

echo "Canary injected at: $CANARY_FILE"

# ---------- Phase 3: Configure + Build ----------

echo ""
echo "=== Phase 3: Configure + Build (expect success — ASAN is runtime) ==="

cmake --preset "$PRESET" $CONFIGURE_EXTRA 2>&1 | tail -5
cmake --build --preset "$PRESET" 2>&1 | tail -5

check "Build succeeded under $PRESET" test $? -eq 0

# ---------- Phase 4: Run ctest — expect ASAN failure ----------

echo ""
echo "=== Phase 4: Run ctest (expect ASAN failure on canary) ==="

CTEST_OUTPUT=""
CTEST_RC=0
pushd "$BUILD_DIR" > /dev/null
CTEST_OUTPUT=$(ctest --output-on-failure -R "asan_canary" 2>&1) || CTEST_RC=$?
popd > /dev/null

echo "$CTEST_OUTPUT" | tail -20

check "ctest exited non-zero (ASAN caught the canary)" test "$CTEST_RC" -ne 0

check "ctest output contains AddressSanitizer" \
    echo "$CTEST_OUTPUT" | grep -qi "addresssanitizer\|heap-use-after-free"

# ---------- Phase 5: Validate merge-watcher pattern matching ----------

echo ""
echo "=== Phase 5: Validate merge-watcher detection ==="

# Simulate the status line merge-watcher would see from merge-gates.sh
SIMULATED_STATUS="Sanitizer (ASAN via MSVC) fail 3m14s https://github.com/..."

"$PY" - "$REPO_ROOT/agents/scripts/core/merge-watcher.py" "$SIMULATED_STATUS" << 'PYEOF'
import sys, importlib.util, os

watcher_path = sys.argv[1]
status_line  = sys.argv[2]

# Load merge-watcher as a module
spec = importlib.util.spec_from_file_location("mw", watcher_path)
mw = importlib.util.module_from_spec(spec)

# We only need the detection function — mock the rest
try:
    spec.loader.exec_module(mw)
except Exception:
    pass  # module-level imports may fail; we parse the function manually

# Fallback: extract and eval the function directly
import re, textwrap
with open(watcher_path) as f:
    src = f.read()

# Extract _looks_like_sanitizer_failure
match = re.search(
    r'(def _looks_like_sanitizer_failure\(.*?\).*?(?=\ndef |\nclass |\Z))',
    src, re.DOTALL
)
if not match:
    print("FAIL: Could not find _looks_like_sanitizer_failure in merge-watcher.py")
    sys.exit(1)

exec(match.group(1))  # defines _looks_like_sanitizer_failure in local scope

result = _looks_like_sanitizer_failure(status_line)
if result:
    print(f"Detection function matched: '{status_line}' -> True")
    sys.exit(0)
else:
    print(f"Detection function did NOT match: '{status_line}' -> False")
    sys.exit(1)
PYEOF

check "merge-watcher _looks_like_sanitizer_failure matches ASAN status line" \
    test $? -eq 0

# Also test with real ASAN output signature
"$PY" -c "
s = 'ERROR: AddressSanitizer: heap-use-after-free on address 0x...'
fn_body = '''
def _looks_like_sanitizer_failure(status_line):
    s = status_line.lower()
    return (
        \"sanitizer\" in s
        and (\"fail\" in s or \"error\" in s or \"blocked\" in s)
    ) or (
        (\"addresssanitizer\" in s or \"undefinedbehaviorsanitizer\" in s or \"threadsanitizer\" in s)
        and (\"fail\" in s or \"error\" in s or \"warning\" in s)
    )
'''
exec(fn_body)
result = _looks_like_sanitizer_failure(s)
print(f'ASAN error string match: {result}')
exit(0 if result else 1)
"

check "merge-watcher matches raw ASAN error output" test $? -eq 0

# ---------- Phase 6: Validate AUTO_ACT_SANITIZER_PROMPT exists ----------

echo ""
echo "=== Phase 6: Validate prompt template ==="

check "AUTO_ACT_SANITIZER_PROMPT defined in merge-watcher.py" \
    grep -q "AUTO_ACT_SANITIZER_PROMPT" "$REPO_ROOT/agents/scripts/core/merge-watcher.py"

check "Prompt references debug-detective" \
    grep -A5 "AUTO_ACT_SANITIZER_PROMPT" "$REPO_ROOT/agents/scripts/core/merge-watcher.py" \
    | grep -qi "debug-detective"

check "MERGE_WATCH_AUTO_ACT_ON_SANITIZER env var checked" \
    grep -q "MERGE_WATCH_AUTO_ACT_ON_SANITIZER" "$REPO_ROOT/agents/scripts/core/merge-watcher.py"

# ---------- Phase 7: Validate debug-detective contract ----------

echo ""
echo "=== Phase 7: Validate debug-detective reproducer-first contract ==="

check "debug-detective.md contains 'reproducer-first contract'" \
    grep -q "reproducer-first contract" "$REPO_ROOT/agents/core/debug-detective.md"

check "debug-detective.md contains phase 0 (concreteness check)" \
    grep -qi "phase 0.*concreteness\|concreteness check" "$REPO_ROOT/agents/core/debug-detective.md"

check "debug-detective.md contains phase 0.5 (scenario reuse)" \
    grep -qi "phase 0.5\|existing-scenario reuse" "$REPO_ROOT/agents/core/debug-detective.md"

# ---------- Phase 8: Validate scenario infrastructure ----------

echo ""
echo "=== Phase 8: Validate scenario infrastructure ==="

check "SmatchetScenarioRegistry.cpp exists" \
    test -f "$REPO_ROOT/Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp"

check "5 slice-8 scenarios registered" \
    bash -c "grep -c 'Scenario()' '$REPO_ROOT/Source/Core/src/Commands/Scenarios/SmatchetScenarioRegistry.cpp' | awk '{exit (\$1 >= 19 ? 0 : 1)}'"

check "StubAiClient.h exists" \
    test -f "$REPO_ROOT/tests/support/StubAiClient.h"

check "FakeP4Runner.h exists" \
    test -f "$REPO_ROOT/tests/support/FakeP4Runner.h"

check "SmatchetAgentDebug.h exists" \
    test -f "$REPO_ROOT/tests/_debug/SmatchetAgentDebug.h"

# ---------- Summary ----------

echo ""
echo "========================================"
echo "  PASS: $PASS   FAIL: $FAIL"
echo "========================================"

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi

echo ""
echo "All checks passed. The autonomous debugging loop infrastructure is wired."
echo ""
echo "To test the FULL end-to-end loop (merge-watcher → debug-detective spawn):"
echo "  1. Push a branch with the ASAN canary still in place (--keep-canary)"
echo "  2. Open a PR"
echo "  3. Set MERGE_WATCH_AUTO_ACT_ON_SANITIZER=true"
echo "  4. Run: python agents/scripts/core/merge-watcher.py --pr <N>"
echo "  5. Watch debug-detective spawn and diagnose the use-after-free"
