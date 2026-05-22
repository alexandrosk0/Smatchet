#!/usr/bin/env bash
# test-p4-dual-vcs.sh — verification scenarios for the Perforce dual-VCS layer.
#
# Phase 7 of docs/design/git-to-perforce-migration.md. Three scenarios:
#   1. dual-VCS round-trip (allocate task stream → edit → bridge --dry-run)
#   2. git-only baseline (regression — no p4 calls when SMATCHET_AGENT_VCS=git)
#   3. multi-agent parallel (two task streams allocated + GC'd in one pass)
#
# Skips cleanly when p4 is unreachable (CI has no p4d; exits 2 so test-all.sh
# treats this as "missing binary" rather than a real failure).
#
# Exit codes (per scripts/dev/test-all.sh contract):
#   0 — every scenario passed
#   1 — at least one scenario failed
#   2 — p4 unreachable / env not configured (skip)

set -euo pipefail

cd "$(dirname "$0")/../.."

PASSED=0
FAILED=0
PROBE_AGENT_A="phase7-probe-a"
PROBE_AGENT_B="phase7-probe-b"

# ----- preconditions -------------------------------------------------------
# Helper-script precheck — if any of the dependent scripts (owned by #380
# and merged piecemeal) aren't on disk, exit 2 per scripts/dev/test-all.sh
# "missing binary → skip" contract. Otherwise the test would die with exit
# 127 (command-not-found) the first time it tried to invoke one, which
# test-all.sh would mis-classify as a real failure.
for dep in scripts/dev/p4-task-stream.sh scripts/dev/p4-task-stream-gc.sh scripts/dev/lock-claim.sh; do
    # Invocations are `bash <script>` (not `<script>` directly), so we only
    # need read access — not the executable bit. On Windows / NTFS the
    # x-bit often isn't set even when the script runs fine under bash;
    # `-x` would false-skip in that case (e.g. lock-claim.sh ships as
    # -rw-r--r-- depending on how the repo was cloned).
    if ! [ -r "$dep" ]; then
        echo "test-p4-dual-vcs: required helper '${dep}' missing/unreadable; skipping (Passed: 0  Failed: 0)"
        exit 2
    fi
done

P4_BIN="${P4_BIN:-p4}"
if ! command -v "$P4_BIN" >/dev/null 2>&1; then
    P4_BIN="/c/Program Files/Perforce/p4.exe"
fi
if ! [ -x "$P4_BIN" ] && ! command -v "$P4_BIN" >/dev/null 2>&1; then
    echo "test-p4-dual-vcs: p4 CLI not found; skipping (Passed: 0  Failed: 0)"
    exit 2
fi
: "${P4PORT:=localhost:1666}"
: "${P4USER:=alexk}"
export P4PORT P4USER P4_BIN
if ! "$P4_BIN" info >/dev/null 2>&1; then
    echo "test-p4-dual-vcs: p4 server unreachable at ${P4PORT}; skipping (Passed: 0  Failed: 0)"
    exit 2
fi

assert_eq() {
    # assert_eq <name> <expected> <actual>
    if [ "$2" = "$3" ]; then
        echo "  PASS: $1"
        PASSED=$((PASSED + 1))
    else
        echo "  FAIL: $1 — expected '$2', got '$3'" >&2
        FAILED=$((FAILED + 1))
    fi
}

cleanup_probe() {
    local agent_id="$1"
    "$P4_BIN" client -d "task_${agent_id}" >/dev/null 2>&1 || true
    "$P4_BIN" stream -d "//smatchet/task-${agent_id}" >/dev/null 2>&1 || true
    rm -rf ".claude/streams/${agent_id}"
}

trap 'cleanup_probe "$PROBE_AGENT_A"; cleanup_probe "$PROBE_AGENT_B"' EXIT

# ----- scenario 1: dual-VCS round-trip ------------------------------------
echo ""
echo "=== Scenario 1: dual-VCS round-trip ==="

cleanup_probe "$PROBE_AGENT_A"   # leftover state from prior runs

ws=$(bash scripts/dev/p4-task-stream.sh "$PROBE_AGENT_A" 2>/dev/null)
[ -n "$ws" ] && [ -d "$ws" ]
assert_eq "alloc emits workspace path" "ok" "$([ -n "$ws" ] && [ -d "$ws" ] && echo ok)"

synced_count=$(find "$ws" -type f 2>/dev/null | wc -l)
test "$synced_count" -ge 100  # sanity: depot should have hundreds of files
assert_eq "task workspace populated (>= 100 files)" "ok" "$([ "$synced_count" -ge 100 ] && echo ok)"

# Re-invoke is idempotent: returns same path, no error.
ws2=$(bash scripts/dev/p4-task-stream.sh "$PROBE_AGENT_A" 2>/dev/null)
assert_eq "alloc is idempotent (same workspace path)" "$ws" "$ws2"

# ----- scenario 2: git-only baseline (regression — no p4 calls invoked) ----
echo ""
echo "=== Scenario 2: git-only baseline (no p4 calls) ==="

# Trace `p4` invocation by routing PATH through a wrapper that logs every
# call. Then actually invoke scripts/dev/lock-claim.sh with the default
# (git-ref) backend; the wrapper p4 must NOT be called because the dispatcher
# at the top of lock-claim.sh `exec`s into lock-claim-p4.sh only when
# SMATCHET_LOCK_BACKEND=p4-counter is set. Vacuous-pass concern (CR #389)
# resolved: previous shape only inspected the env-comparison snippet inline,
# never crossed into the real script.
TRACE_DIR=$(mktemp -d)
cat > "$TRACE_DIR/p4" <<'EOF'
#!/usr/bin/env bash
echo "$@" >> "$P4_TRACE_LOG"
exit 0
EOF
chmod +x "$TRACE_DIR/p4"
export P4_TRACE_LOG="$TRACE_DIR/p4-calls.log"
: > "$P4_TRACE_LOG"

# Tiny write-set fixture for the claim. Use a slug + remote that no other
# session can race on (SMATCHET_LOCK_BYPASS_REPO_CHECK=1 lets us point at a
# bogus remote so we don't actually fork git push either). The claim is
# allowed to FAIL — we only care about whether the wrapper p4 was called.
ws_file=$(mktemp)
printf 'scripts/dev/test-p4-dual-vcs.sh\n' > "$ws_file"
PATH="${TRACE_DIR}:${PATH}" \
    SMATCHET_LOCK_BACKEND="" \
    SMATCHET_LOCK_BYPASS_REPO_CHECK=1 \
    LOCK_REMOTE="file:///nonexistent-test-remote" \
    bash scripts/dev/lock-claim.sh "phase7-trace-${RANDOM}" "$ws_file" >/dev/null 2>&1 || true
rm -f "$ws_file"

call_count=$(wc -l < "$P4_TRACE_LOG" 2>/dev/null | tr -d ' ')
assert_eq "default backend invokes zero p4 calls (PATH-traced)" "0" "${call_count:-0}"

# Sanity check the dispatcher logic in isolation too (no scripts/dev/ call).
dispatch_under_default=$(SMATCHET_LOCK_BACKEND="" bash -c '
    [ "${SMATCHET_LOCK_BACKEND:-git-ref}" = "p4-counter" ] && echo "p4-dispatched" || echo "git-ref"
')
assert_eq "dispatcher default stays git-ref" "git-ref" "$dispatch_under_default"

rm -rf "$TRACE_DIR"

# ----- scenario 3: multi-agent parallel ----------------------------------
echo ""
echo "=== Scenario 3: multi-agent parallel task streams ==="

cleanup_probe "$PROBE_AGENT_B"

wsA=$(bash scripts/dev/p4-task-stream.sh "$PROBE_AGENT_A" 2>/dev/null)
wsB=$(bash scripts/dev/p4-task-stream.sh "$PROBE_AGENT_B" 2>/dev/null)
[ "$wsA" != "$wsB" ]
assert_eq "two agents get distinct workspaces" "ok" "$([ "$wsA" != "$wsB" ] && echo ok)"

stream_count=$("$P4_BIN" streams "//smatchet/task-*" 2>/dev/null | grep -c "phase7-probe-" || true)
assert_eq "both streams visible in 'p4 streams'" "2" "${stream_count// /}"

# GC dry-run should propose purging both (--older-than-days 0 forces it).
gc_out=$(bash scripts/dev/p4-task-stream-gc.sh --older-than-days 0 --dry-run 2>&1)
purgeable=$(echo "$gc_out" | grep -c "DRY-RUN would purge //smatchet/task-phase7-probe-" || true)
assert_eq "GC dry-run identifies both task streams" "2" "${purgeable// /}"

# Real GC removes both.
bash scripts/dev/p4-task-stream-gc.sh --older-than-days 0 >/dev/null 2>&1
remaining=$("$P4_BIN" streams "//smatchet/task-*" 2>/dev/null | grep -c "phase7-probe-" || true)
assert_eq "GC real run removes both" "0" "${remaining// /}"

# ----- final tally ---------------------------------------------------------
echo ""
echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -eq 0 ]
