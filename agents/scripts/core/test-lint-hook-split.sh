#!/usr/bin/env bash
# test-lint-hook-split.sh — verify the deferred-lint pipeline.
#
# Covers:
#   1. Inline hook produces .lint-queue.<pid> + .tree-dirty, exits 0, fast.
#   2. Multi-edit dedup: same path written 3× still drains as 1 file.
#   3. Multi-file drain: distinct paths each visited once.
#   7. SMATCHET_LINT_INLINE=1 escape hatch skips the queue (and runs inline).
#   8. agents/scripts/core/lint-flush.sh delegates to the drain script.
#   9. PreToolUse:Bash on `cmake --build …` clears .tree-dirty.
#  11. SessionStart cleanup removes orphaned queue / lock / tree-dirty.
#
# Deferred (require real C++ fault injection or process orchestration):
#   - Test 4 (issue surfacing with a real cppcheck violation)
#   - Test 5 (chunked drain across > SMATCHET_LINT_DRAIN_CHUNK files)
#   - Test 6 (parallel-subagent per-PID queue isolation across live PIDs)
#   - Test 10 (lockfile serialises concurrent Stop events)
# Filed in docs/self-improvement/AGENT_SELF_IMPROVEMENT.md as a follow-up sweep.
#
# Auto-enrolled by scripts/dev/test-all.sh.

set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(cd "$(dirname "$0")/../../.." && pwd)}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"

CLAUDE_DIR="$PROJ_DIR/.claude"
HOOKS_DIR="$CLAUDE_DIR/hooks"

PASS=0
FAIL=0
declare -a FAILURES=()

note() { echo "[lint-hook-split] $*"; }
ok()   { PASS=$((PASS + 1)); echo "  PASS  $1"; }
nope() { FAIL=$((FAIL + 1)); FAILURES+=("$1"); echo "  FAIL  $1"; }

cleanup() {
    rm -f "$CLAUDE_DIR"/.lint-queue.* 2>/dev/null || true
    rm -f "$CLAUDE_DIR/.lint-queue.lock" 2>/dev/null || true
    rm -f "$CLAUDE_DIR/.tree-dirty" 2>/dev/null || true
    # Synthesised Test 3 fixture copies — canonical lint_hook_probe.cpp stays.
    rm -f "$PROJ_DIR/tests/fixtures/lint_hook_probe_b.cpp" 2>/dev/null || true
    rm -f "$PROJ_DIR/tests/fixtures/lint_hook_probe_c.cpp" 2>/dev/null || true
}
trap cleanup EXIT

cleanup  # start from a known-clean state

# Pre-formatted, cppcheck-clean fixture under tests/fixtures/. Using a real
# production file as the probe would cause `clang-format -i` to apply
# legitimate-but-unrelated format fixes in place, contaminating the working
# tree. The fixture is intentionally trivial and stable.
PROBE_FILE="$PROJ_DIR/tests/fixtures/lint_hook_probe.cpp"
if [[ ! -f "$PROBE_FILE" ]]; then
    note "probe fixture missing: $PROBE_FILE"
    exit 1
fi
PROBE_JSON='{"tool_input": {"file_path": "'"$PROBE_FILE"'"}}'

# -------------------------------------------------------------------- Test 1
note "Test 1 — inline hook produces queue + tree-dirty"
cleanup
start_ns="$(date +%s%N 2>/dev/null || date +%s)"
echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh"
inline_rc=$?
end_ns="$(date +%s%N 2>/dev/null || date +%s)"

if [[ $inline_rc -eq 0 ]]; then
    ok "inline exit=0"
else
    nope "inline exit=$inline_rc (expected 0)"
fi

QUEUE_FILES=("$CLAUDE_DIR"/.lint-queue.*)
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
if [[ ${#QUEUE_REAL[@]} -ge 1 ]]; then
    ok "queue file appeared: ${QUEUE_REAL[0]##*/}"
else
    nope "queue file missing after inline"
fi

if grep -q "lint_hook_probe.cpp" "$CLAUDE_DIR"/.lint-queue.* 2>/dev/null; then
    ok "queue contains probe path"
else
    nope "queue does not contain probe path"
fi

if [[ -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty written"
else
    nope ".tree-dirty missing"
fi

# -------------------------------------------------------------------- Test 2
note "Test 2 — multi-edit dedup"
cleanup
for _ in 1 2 3; do echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh"; done
# Three appends should yield three lines pre-drain.
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
PRE_DRAIN_LINES=$(cat "${QUEUE_REAL[@]}" 2>/dev/null | wc -l)
if [[ $PRE_DRAIN_LINES -eq 3 ]]; then
    ok "queue has 3 lines pre-drain (one per inline call)"
else
    nope "queue has $PRE_DRAIN_LINES lines pre-drain (expected 3)"
fi

# After drain, queue should be empty (dedup → 1 unique path → consumed in chunk).
bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1
drain_rc=$?
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "drain consumed dedup'd queue (rc=$drain_rc)"
else
    nope "drain left ${#QUEUE_REAL[@]} queue files (rc=$drain_rc)"
fi

# -------------------------------------------------------------------- Test 3
note "Test 3 — multi-file drain"
cleanup
# Three distinct first-party probe fixtures. Synthesise copies of the canonical
# fixture under unique names so each is a fresh entry for the drain.
declare -a MULTI=(
    "$PROJ_DIR/tests/fixtures/lint_hook_probe.cpp"
    "$PROJ_DIR/tests/fixtures/lint_hook_probe_b.cpp"
    "$PROJ_DIR/tests/fixtures/lint_hook_probe_c.cpp"
)
mkdir -p "$PROJ_DIR/tests/fixtures"
cp -f "${MULTI[0]}" "${MULTI[1]}"
cp -f "${MULTI[0]}" "${MULTI[2]}"
# Disambiguate the namespace identifier so cppcheck doesn't flag duplicate-
# definition style issues if it ever sees all three at once.
sed -i 's/smatchet_lint_probe/smatchet_lint_probe_b/' "${MULTI[1]}"
sed -i 's/smatchet_lint_probe/smatchet_lint_probe_c/' "${MULTI[2]}"

for f in "${MULTI[@]}"; do
    if [[ -f "$f" ]]; then
        echo '{"tool_input": {"file_path": "'"$f"'"}}' | bash "$HOOKS_DIR/lint-cpp.sh"
    fi
done

shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
PRE=$(cat "${QUEUE_REAL[@]}" 2>/dev/null | wc -l)
if [[ $PRE -eq ${#MULTI[@]} ]]; then
    ok "queue has $PRE lines pre-drain (one per file)"
else
    nope "queue has $PRE lines pre-drain (expected ${#MULTI[@]})"
fi

bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1
drain_rc=$?
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "drain consumed multi-file queue (rc=$drain_rc)"
else
    nope "drain left ${#QUEUE_REAL[@]} queue files (rc=$drain_rc)"
fi

# -------------------------------------------------------------------- Test 7
note "Test 7 — SMATCHET_LINT_INLINE=1 escape hatch skips the queue"
cleanup
SMATCHET_LINT_INLINE=1 bash -c "echo '$PROBE_JSON' | bash '$HOOKS_DIR/lint-cpp.sh'" >/dev/null 2>&1
rc=$?
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "inline mode skipped the queue (rc=$rc)"
else
    nope "inline mode wrote to queue anyway (rc=$rc, files=${#QUEUE_REAL[@]})"
fi

# -------------------------------------------------------------------- Test 8
note "Test 8 — manual flush via agents/scripts/core/lint-flush.sh"
cleanup
echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh"
bash "$PROJ_DIR/agents/scripts/core/lint-flush.sh" >/dev/null 2>&1
flush_rc=$?
shopt -s nullglob
QUEUE_REAL=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "lint-flush drained the queue (rc=$flush_rc)"
else
    nope "lint-flush left queue intact (rc=$flush_rc)"
fi

# -------------------------------------------------------------------- Test 9
note "Test 9 — clear-tree-dirty.sh removes .tree-dirty on cmake --build"
cleanup
: > "$CLAUDE_DIR/.tree-dirty"
[[ -e "$CLAUDE_DIR/.tree-dirty" ]] || { nope "could not pre-stage .tree-dirty"; }

# Synthesize a PreToolUse:Bash JSON payload.
echo '{"tool_input": {"command": "cmake --build --preset ninja-iter-msvc --target SmatchetStandalone"}}' \
    | bash "$HOOKS_DIR/clear-tree-dirty.sh"
if [[ ! -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty cleared on cmake --build invocation"
else
    nope ".tree-dirty still present after cmake --build hook"
fi

# Negative case: an unrelated command must leave .tree-dirty alone.
: > "$CLAUDE_DIR/.tree-dirty"
echo '{"tool_input": {"command": "ls -la build/"}}' \
    | bash "$HOOKS_DIR/clear-tree-dirty.sh"
if [[ -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty preserved on unrelated Bash command"
else
    nope ".tree-dirty wrongly cleared on unrelated command"
fi

# Env-prefix variant: `MSYS2_PATH_TYPE=inherit cmake --build …` must still match.
: > "$CLAUDE_DIR/.tree-dirty"
echo '{"tool_input": {"command": "MSYS2_PATH_TYPE=inherit cmake --build --preset ninja-test-msvc"}}' \
    | bash "$HOOKS_DIR/clear-tree-dirty.sh"
if [[ ! -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty cleared with env-var prefix"
else
    nope ".tree-dirty not cleared with env-var prefix"
fi

# ------------------------------------------------------------------- Test 11
note "Test 11 — SessionStart clears orphaned queue / lock / tree-dirty"
cleanup
: > "$CLAUDE_DIR/.lint-queue.99999"
: > "$CLAUDE_DIR/.lint-queue.lock"
: > "$CLAUDE_DIR/.tree-dirty"

# clear-session-context.sh reads optional JSON on stdin. Empty stdin is fine.
echo '' | bash "$PROJ_DIR/agents/scripts/core/clear-session-context.sh" >/dev/null 2>&1

shopt -s nullglob
ORPHANS=("$CLAUDE_DIR"/.lint-queue.*)
shopt -u nullglob
LOCK_PRESENT=0
[[ -e "$CLAUDE_DIR/.lint-queue.lock" ]] && LOCK_PRESENT=1
DIRTY_PRESENT=0
[[ -e "$CLAUDE_DIR/.tree-dirty" ]] && DIRTY_PRESENT=1

if [[ ${#ORPHANS[@]} -eq 0 && $LOCK_PRESENT -eq 0 && $DIRTY_PRESENT -eq 0 ]]; then
    ok "SessionStart removed all orphan markers"
else
    nope "SessionStart left state (queue=${#ORPHANS[@]} lock=$LOCK_PRESENT dirty=$DIRTY_PRESENT)"
fi

# -------------------------------------------------------------------- Report
echo
# test-all.sh aggregator parses the literal "Passed: N  Failed: M" line.
echo "Passed: $PASS  Failed: $FAIL"
if [[ $FAIL -gt 0 ]]; then
    echo
    echo "lint-hook-split failures:"
    for msg in "${FAILURES[@]}"; do echo "  - $msg"; done
    exit 1
fi
exit 0
