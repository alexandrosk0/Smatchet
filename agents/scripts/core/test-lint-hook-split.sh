#!/usr/bin/env bash
# test-lint-hook-split.sh — verify the deferred-lint pipeline.
#
# Covers:
#   1. Inline hook produces .lint-queue.<pid> + .tree-dirty, exits 0, fast.
#   2. Multi-edit dedup: same path written 3× still drains as 1 file.
#   3. Multi-file drain: distinct paths each visited once.
#   4. Fault injection: a real cppcheck violation surfaces from the drain.
#   5. Chunked drain across > SMATCHET_LINT_DRAIN_CHUNK files (remainder requeued).
#   6. Per-PID queue isolation: distinct .lint-queue.<pid> files both consumed.
#   7. SMATCHET_LINT_INLINE=1 escape hatch skips the queue (and runs inline).
#   8. agents/scripts/core/lint-flush.sh delegates to the drain script.
#   9. PreToolUse:Bash on `cmake --build …` clears .tree-dirty.
#  10. Lockfile serialises concurrent Stop events (via scripts/dev/lockfile.py).
#  11. SessionStart cleanup removes orphaned queue / lock / tree-dirty.
#  12. lint-syntax-both.py --selftest (PCH-drift FP classification, PR-6).
#
# Tests 4/5/6/10 were the deferred sweep filed in
# docs/self-improvement/AGENT_SELF_IMPROVEMENT.md — implemented in PR-6.
#
# Auto-enrolled by scripts/dev/test-all.sh.

# set -e: an unexpected failure (missing hook, typo) aborts early rather than
# cascading into confusing downstream assertion failures. Every command whose
# non-zero exit is EXPECTED / captured is guarded with `|| rc=$?` (rc reset to 0
# first) or `|| true`, so set -e never fires on an intentional may-fail step
# (bash-06). Pipelines whose left side may legitimately fail (a `cat` over an
# empty queue glob) are likewise guarded.
set -euo pipefail

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
skip() { echo "  SKIP  $1"; }

# Populate the global QUEUE_REAL array with the per-PID queue files, matching
# the drain's own glob: .lint-queue.lock is the serialisation lock, not queue
# content, and (unlike the pre-lockfile.py flock era) it outlives a drain.
declare -a QUEUE_REAL=()
collect_queue_files() {
    QUEUE_REAL=()
    shopt -s nullglob
    local q
    for q in "$CLAUDE_DIR"/.lint-queue.*; do
        [[ "$q" == *.lock ]] && continue
        QUEUE_REAL+=("$q")
    done
    shopt -u nullglob
}

cleanup() {
    rm -f "$CLAUDE_DIR"/.lint-queue.* 2>/dev/null || true
    rm -f "$CLAUDE_DIR/.lint-queue.lock" 2>/dev/null || true
    rm -f "$CLAUDE_DIR/.tree-dirty" 2>/dev/null || true
    # Synthesised Test 3 fixture copies — canonical lint_hook_probe.cpp stays.
    rm -f "$PROJ_DIR/tests/fixtures/lint_hook_probe_b.cpp" 2>/dev/null || true
    rm -f "$PROJ_DIR/tests/fixtures/lint_hook_probe_c.cpp" 2>/dev/null || true
    # Synthesised Test 4/5/6 fault + chunk fixtures.
    rm -f "$PROJ_DIR/tests/fixtures/lint_hook_fault.cpp" 2>/dev/null || true
    rm -f "$PROJ_DIR"/tests/fixtures/lint_hook_chunk_*.cpp 2>/dev/null || true
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
inline_rc=0
echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh" || inline_rc=$?
end_ns="$(date +%s%N 2>/dev/null || date +%s)"

if [[ $inline_rc -eq 0 ]]; then
    ok "inline exit=0"
else
    nope "inline exit=$inline_rc (expected 0)"
fi

collect_queue_files
if [[ ${#QUEUE_REAL[@]} -ge 1 ]]; then
    ok "queue file appeared: ${QUEUE_REAL[0]##*/}"
else
    nope "queue file missing after inline"
fi

if [[ ${#QUEUE_REAL[@]} -gt 0 ]] && grep -q "lint_hook_probe.cpp" "${QUEUE_REAL[@]}" 2>/dev/null; then
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
for _ in 1 2 3; do echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh" || true; done
# Three appends should yield three lines pre-drain.
collect_queue_files
PRE_DRAIN_LINES=$(cat "${QUEUE_REAL[@]}" 2>/dev/null | wc -l) || true
if [[ $PRE_DRAIN_LINES -eq 3 ]]; then
    ok "queue has 3 lines pre-drain (one per inline call)"
else
    nope "queue has $PRE_DRAIN_LINES lines pre-drain (expected 3)"
fi

# After drain, queue should be empty (dedup → 1 unique path → consumed in chunk).
drain_rc=0
bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || drain_rc=$?
collect_queue_files
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
        echo '{"tool_input": {"file_path": "'"$f"'"}}' | bash "$HOOKS_DIR/lint-cpp.sh" || true
    fi
done

collect_queue_files
PRE=$(cat "${QUEUE_REAL[@]}" 2>/dev/null | wc -l) || true
if [[ $PRE -eq ${#MULTI[@]} ]]; then
    ok "queue has $PRE lines pre-drain (one per file)"
else
    nope "queue has $PRE lines pre-drain (expected ${#MULTI[@]})"
fi

drain_rc=0
bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || drain_rc=$?
collect_queue_files
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "drain consumed multi-file queue (rc=$drain_rc)"
else
    nope "drain left ${#QUEUE_REAL[@]} queue files (rc=$drain_rc)"
fi

# -------------------------------------------------------------------- Test 4
note "Test 4 — fault injection: a real cppcheck violation surfaces from the drain"
cleanup
# A first-party fixture with a genuine cppcheck defect (out-of-bounds + uninit).
# Queue it via the inline hook, then drain and assert the drain reports it.
# The drain delta-filters findings to lines changed vs origin/develop; an
# untracked fixture has no diff hunks, which would drop every finding. Force the
# delta baseline to a non-existent ref so lint_filter_delta fails OPEN (the
# documented non-git / unresolvable-baseline behaviour) and real findings pass.
FAULT_FILE="$PROJ_DIR/tests/fixtures/lint_hook_fault.cpp"
mkdir -p "$PROJ_DIR/tests/fixtures"
cat > "$FAULT_FILE" <<'CPP'
namespace smatchet_lint_fault {
int OutOfBounds() {
    int arr[3];
    return arr[5];
}
} // namespace smatchet_lint_fault
CPP

if ! command -v cppcheck >/dev/null 2>&1; then
    skip "Test 4 — cppcheck not on PATH"
else
    echo '{"tool_input": {"file_path": "'"$FAULT_FILE"'"}}' | bash "$HOOKS_DIR/lint-cpp.sh" || true
    # Drain with the delta baseline forced unresolvable (fail-open) so the
    # finding on the untracked fixture is not delta-filtered away.
    drain_rc=0
    DRAIN_OUT="$(LINT_DELTA_BASE="__no_such_ref_pr6__" bash "$HOOKS_DIR/lint-cpp-drain.sh" 2>&1)" || drain_rc=$?
    if printf '%s' "$DRAIN_OUT" | grep -qiE 'arrayIndexOutOfBounds|uninitvar|cppcheck:'; then
        ok "drain surfaced the injected cppcheck violation (rc=$drain_rc)"
    else
        nope "drain did not surface the injected violation (rc=$drain_rc): $DRAIN_OUT"
    fi
fi

# -------------------------------------------------------------------- Test 5
note "Test 5 — chunked drain across > SMATCHET_LINT_DRAIN_CHUNK files"
cleanup
# Stage 3 distinct clean fixtures and drain with a chunk size of 2: the first
# drain must consume 2 and requeue 1 (a single new queue file); the second drain
# must consume the remainder, leaving the queue empty.
declare -a CHUNKS=()
for n in 1 2 3; do
    cf="$PROJ_DIR/tests/fixtures/lint_hook_chunk_${n}.cpp"
    cp -f "$PROBE_FILE" "$cf"
    sed -i "s/smatchet_lint_probe/smatchet_lint_probe_chunk${n}/" "$cf"
    CHUNKS+=("$cf")
    echo '{"tool_input": {"file_path": "'"$cf"'"}}' | bash "$HOOKS_DIR/lint-cpp.sh" || true
done

SMATCHET_LINT_DRAIN_CHUNK=2 bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || true
collect_queue_files
# Exactly one queue file should remain, holding the 1-file remainder.
REMAIN_LINES=$(cat "${QUEUE_REAL[@]}" 2>/dev/null | wc -l | tr -d ' ') || true
if [[ ${#QUEUE_REAL[@]} -eq 1 && $REMAIN_LINES -eq 1 ]]; then
    ok "first chunked drain requeued the 1-file remainder"
else
    nope "expected 1 remainder file with 1 line; got ${#QUEUE_REAL[@]} files / $REMAIN_LINES lines"
fi

SMATCHET_LINT_DRAIN_CHUNK=2 bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || true
collect_queue_files
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "second chunked drain consumed the remainder (queue empty)"
else
    nope "second chunked drain left ${#QUEUE_REAL[@]} queue file(s)"
fi

# -------------------------------------------------------------------- Test 6
note "Test 6 — per-PID queue isolation: distinct .lint-queue.<pid> both consumed"
cleanup
# Simulate two parallel agents by hand-writing two per-PID queue files (the
# inline hook keys the queue off $PPID, so two live PIDs map to two files). The
# drain globs .lint-queue.* so it must consume BOTH regardless of owner PID.
printf '%s\n' "$PROBE_FILE" > "$CLAUDE_DIR/.lint-queue.111111"
cp -f "$PROBE_FILE" "$PROJ_DIR/tests/fixtures/lint_hook_chunk_1.cpp"
sed -i 's/smatchet_lint_probe/smatchet_lint_probe_pid6/' "$PROJ_DIR/tests/fixtures/lint_hook_chunk_1.cpp"
printf '%s\n' "$PROJ_DIR/tests/fixtures/lint_hook_chunk_1.cpp" > "$CLAUDE_DIR/.lint-queue.222222"

drain_rc=0
bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || drain_rc=$?
collect_queue_files
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "drain consumed both per-PID queue files (rc=$drain_rc)"
else
    nope "drain left ${#QUEUE_REAL[@]} per-PID queue file(s) (rc=$drain_rc)"
fi

# -------------------------------------------------------------------- Test 7
note "Test 7 — SMATCHET_LINT_INLINE=1 escape hatch skips the queue"
cleanup
rc=0
SMATCHET_LINT_INLINE=1 bash -c "echo '$PROBE_JSON' | bash '$HOOKS_DIR/lint-cpp.sh'" >/dev/null 2>&1 || rc=$?
collect_queue_files
if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
    ok "inline mode skipped the queue (rc=$rc)"
else
    nope "inline mode wrote to queue anyway (rc=$rc, files=${#QUEUE_REAL[@]})"
fi

# -------------------------------------------------------------------- Test 8
note "Test 8 — manual flush via agents/scripts/core/lint-flush.sh"
cleanup
echo "$PROBE_JSON" | bash "$HOOKS_DIR/lint-cpp.sh" || true
flush_rc=0
bash "$PROJ_DIR/agents/scripts/core/lint-flush.sh" >/dev/null 2>&1 || flush_rc=$?
collect_queue_files
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
    | bash "$HOOKS_DIR/clear-tree-dirty.sh" || true
if [[ ! -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty cleared on cmake --build invocation"
else
    nope ".tree-dirty still present after cmake --build hook"
fi

# Negative case: an unrelated command must leave .tree-dirty alone.
: > "$CLAUDE_DIR/.tree-dirty"
echo '{"tool_input": {"command": "ls -la build/"}}' \
    | bash "$HOOKS_DIR/clear-tree-dirty.sh" || true
if [[ -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty preserved on unrelated Bash command"
else
    nope ".tree-dirty wrongly cleared on unrelated command"
fi

# Env-prefix variant: `MSYS2_PATH_TYPE=inherit cmake --build …` must still match.
: > "$CLAUDE_DIR/.tree-dirty"
echo '{"tool_input": {"command": "MSYS2_PATH_TYPE=inherit cmake --build --preset ninja-test-msvc"}}' \
    | bash "$HOOKS_DIR/clear-tree-dirty.sh" || true
if [[ ! -e "$CLAUDE_DIR/.tree-dirty" ]]; then
    ok ".tree-dirty cleared with env-var prefix"
else
    nope ".tree-dirty not cleared with env-var prefix"
fi

# ------------------------------------------------------------------- Test 10
note "Test 10 — lockfile.py serialises concurrent drains"
cleanup
LOCK_PY="$PROJ_DIR/scripts/dev/lockfile.py"
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/lib/resolve-py.sh"
PY_BIN="$(resolve_py || true)"
if [[ -z "$PY_BIN" || ! -f "$LOCK_PY" ]]; then
    # The drain's serialisation runs through scripts/dev/lockfile.py; with no
    # Python the drain degrades to no locking by design (see the hook header),
    # so there is nothing to serialise-test.
    skip "Test 10 — python/lockfile.py unavailable (drain runs lock-free by design)"
else
    # Hold the lock in a background process, then fire a drain that has work
    # queued: the contending drain must take the non-blocking lock path and exit
    # 0 WITHOUT consuming the queue (the holder will, or a later Stop event).
    echo '{"tool_input": {"file_path": "'"$PROBE_FILE"'"}}' | bash "$HOOKS_DIR/lint-cpp.sh" || true
    LOCK_FILE="$CLAUDE_DIR/.lint-queue.lock"
    READY_FILE="$CLAUDE_DIR/.lint-lock-held"
    rm -f "$READY_FILE"
    # Background holder: grab the exclusive lock, then signal readiness and
    # sleep, holding it. The marker is written by the locked command, so its
    # existence PROVES the lock is held — a fixed sleep would let a slow host
    # start the drain first and make this test intermittent.
    "$PY_BIN" "$LOCK_PY" --lockfile "$LOCK_FILE" -- \
        bash -c ': > "$1"; sleep 3' _ "$READY_FILE" >/dev/null 2>&1 &
    HOLDER_PID=$!
    # Bounded poll: 100 * 0.05s = 5s ceiling, then run anyway and let the
    # assertion below report the failure rather than hanging the suite.
    for _ in $(seq 1 100); do
        [[ -e "$READY_FILE" ]] && break
        sleep 0.05
    done
    contend_rc=0
    bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || contend_rc=$?
    collect_queue_files
    # The contended drain must NOT have consumed the queued work (lock held).
    if [[ $contend_rc -eq 0 && ${#QUEUE_REAL[@]} -gt 0 ]] &&
       grep -q "lint_hook_probe.cpp" "${QUEUE_REAL[@]}" 2>/dev/null; then
        ok "contended drain exited 0 and left the queue for the lock holder"
    else
        nope "contended drain mis-handled the held lock (rc=$contend_rc, files=${#QUEUE_REAL[@]})"
    fi
    wait "$HOLDER_PID" 2>/dev/null || true
    # After the holder releases, a fresh drain must consume the queue.
    bash "$HOOKS_DIR/lint-cpp-drain.sh" >/dev/null 2>&1 || true
    collect_queue_files
    if [[ ${#QUEUE_REAL[@]} -eq 0 ]]; then
        ok "post-release drain consumed the queue"
    else
        nope "post-release drain left ${#QUEUE_REAL[@]} queue file(s)"
    fi
fi

# ------------------------------------------------------------------- Test 11
note "Test 11 — SessionStart clears orphaned queue / lock / tree-dirty"
cleanup
: > "$CLAUDE_DIR/.lint-queue.99999"
: > "$CLAUDE_DIR/.lint-queue.lock"
: > "$CLAUDE_DIR/.tree-dirty"

# clear-session-context.sh reads optional JSON on stdin. Empty stdin is fine.
echo '' | bash "$PROJ_DIR/agents/scripts/core/clear-session-context.sh" >/dev/null 2>&1 || true

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

# ------------------------------------------------------------------- Test 12
# The dual-target syntax checker (lint_run_dual_target -> lint-syntax-both.py)
# is part of this pipeline; its FP-line filter carries a --selftest. Run it here
# so the PCH version-drift FP patterns (PR-6) stay enrolled in CI.
note "Test 12 — lint-syntax-both.py --selftest (PCH-drift FP classification)"
cleanup
SYNTAX_HOOK="$PROJ_DIR/docs/harness/claude-code/hooks/lint-syntax-both.py"
if [[ -z "$PY_BIN" ]]; then
    skip "Test 12 — python not on PATH"
elif [[ ! -f "$SYNTAX_HOOK" ]]; then
    nope "lint-syntax-both.py missing at $SYNTAX_HOOK"
elif "$PY_BIN" "$SYNTAX_HOOK" --selftest >/dev/null 2>&1; then
    ok "lint-syntax-both selftest passed"
else
    nope "lint-syntax-both selftest failed"
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
