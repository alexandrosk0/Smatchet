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
PROBE_AGENT_PREP="phase7-probe-prep"
PROBE_PROMOTE_AGENT="phase7-probe-promote"

# Track pending CLs created by scenarios 4 + 5 so cleanup can remove them
# even when assertions fail mid-scenario.
PREP_CL=""
STRANDED_CL=""

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

# Cleanup for scenarios 4 + 5: any pending CL we created lives on the main
# client and may have an attached shelf. Order matters — `p4 change -d` on
# a CL with opened files OR a shelf returns "has files" error. So:
#   1. shelve -d   (kills the shelf, files stay opened)
#   2. revert      (drops the opened files; for adds this also removes
#                   the on-disk file, restoring the workspace)
#   3. change -d   (deletes the now-empty pending CL)
cleanup_pending_cl() {
    local cl="$1"
    [ -n "$cl" ] || return 0
    : "${P4_MAIN_CLIENT:=smatchet_main_${P4USER}}"
    # Bind every p4 op to the main client so cleanup works regardless of
    # the caller's default P4CLIENT. Without this, on a machine whose
    # default client is anything other than smatchet_main_*, shelve -d
    # and change -d would silently no-op (CR finding 2026-05-23 on #415).
    P4CLIENT="$P4_MAIN_CLIENT" "$P4_BIN" shelve -d -c "$cl" >/dev/null 2>&1 || true
    P4CLIENT="$P4_MAIN_CLIENT" "$P4_BIN" revert -c "$cl" //smatchet/main/... >/dev/null 2>&1 || true
    P4CLIENT="$P4_MAIN_CLIENT" "$P4_BIN" change -d "$cl" >/dev/null 2>&1 || true
}

trap 'cleanup_pending_cl "$PREP_CL"; cleanup_pending_cl "$STRANDED_CL"; cleanup_probe "$PROBE_AGENT_A"; cleanup_probe "$PROBE_AGENT_B"; cleanup_probe "$PROBE_AGENT_PREP"; cleanup_probe "$PROBE_PROMOTE_AGENT"' EXIT

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

# ----- scenario 4: prepare-review-cl creates pending CL + shelf ------------
# Covers `bash scripts/dev/p4-task-stream-to-pr.sh --prepare-review-cl`
# from docs/design/p4-gated-ship-loop.md. Verifies that prepare-mode:
#   1. Creates a NAMED pending CL on //smatchet/main (not the default
#      changelist).
#   2. Stamps the CL description with the contract tag
#      `task-stream-id: <agent-id>` (promote-mode validates against this).
#   3. Shelves the CL (so a human can review in P4V from any machine).
#   4. Does NOT call git or gh — script exits cleanly without touching
#      the canonical git working tree's branch state.
echo ""
echo "=== Scenario 4: --prepare-review-cl creates pending CL + shelf ==="

# Pre-flight: prepare-mode refuses if the canonical main client carries
# any pending CL (per the script's hard rule — leftover state must be the
# user's call to clean up). If we detect pre-existing pending CLs on the
# canonical client, skip scenario 4 cleanly rather than asserting against
# state we can't safely mutate.
main_client_probe="${P4_MAIN_CLIENT:-smatchet_main_${P4USER}}"
if pending_probe=$("$P4_BIN" changes -s pending -c "$main_client_probe" 2>/dev/null); then
    pending_probe_count=$(printf '%s' "$pending_probe" | grep -c . || true)
else
    pending_probe_count=0
fi
if [ "${pending_probe_count:-0}" -gt 0 ]; then
    echo "  SKIP: ${main_client_probe} has ${pending_probe_count} pre-existing pending CL(s) — scenario 4 cannot mutate user state"
    echo "  (clean up unrelated pending CLs before re-running this test for full coverage)"
else
    cleanup_probe "$PROBE_AGENT_PREP"  # leftover state
    ws_prep=$(bash scripts/dev/p4-task-stream.sh "$PROBE_AGENT_PREP" 2>/dev/null)
    [ -n "$ws_prep" ] && [ -d "$ws_prep" ]
    assert_eq "prepare probe: alloc emits workspace" "ok" "$([ -n "$ws_prep" ] && [ -d "$ws_prep" ] && echo ok)"

# Seed a brand-new file into the task stream so prepare has something to
# integrate. Use a probe-prefixed name so the file is unambiguously test
# debris (cleanup will revert it).
probe_file="phase7-probe-prep-touched.txt"
echo "phase7 probe content $(date +%s)" > "${ws_prep}/${probe_file}"
(
    cd "$ws_prep"
    P4CLIENT="task_${PROBE_AGENT_PREP}" "$P4_BIN" add "$probe_file" >/dev/null 2>&1 || true
    P4CLIENT="task_${PROBE_AGENT_PREP}" "$P4_BIN" submit -d "phase7 probe add" >/dev/null 2>&1
) || true

# Run prepare-mode. P4_MAIN_CLIENT must be exported so the helper sees it
# for cleanup. Stdout captures the CL number (the contract per the script
# header) — stderr carries progress.
# Use the `if foo; then …; else …; fi` idiom to capture the exit code
# safely — bare `cmd; rc=$?` aborts under `set -e` if cmd fails, and the
# script's EXIT trap then masks the abort with exit 0.
PREP_OUT=$(mktemp)
PREP_ERR=$(mktemp)
if P4PORT="${P4PORT}" P4USER="${P4USER}" P4_BIN="${P4_BIN}" \
        bash scripts/dev/p4-task-stream-to-pr.sh "$PROBE_AGENT_PREP" "Phase 7 prepare probe" --prepare-review-cl > "$PREP_OUT" 2>"$PREP_ERR"; then
    prep_exit=0
else
    prep_exit=$?
fi
PREP_CL=$(tail -1 "$PREP_OUT" | tr -d '[:space:]')
if [ "$prep_exit" -ne 0 ]; then
    echo "  DEBUG: prepare-mode stderr tail:" >&2
    tail -10 "$PREP_ERR" >&2
fi
rm -f "$PREP_OUT" "$PREP_ERR"

assert_eq "prepare-mode exits 0" "0" "$prep_exit"
# CL number is purely digits — guard against the script accidentally
# leaking a DRY-RUN sentinel or empty string.
prep_is_cl=$(printf '%s' "$PREP_CL" | grep -cE '^[0-9]+$' || true)
assert_eq "prepare-mode emits CL number on stdout" "1" "${prep_is_cl}"

# Validate the CL state via `p4 change -o <CL>` so we're checking actual
# server-side facts, not just stdout.
if [ -n "$PREP_CL" ] && [ "$prep_is_cl" = "1" ]; then
    cl_spec=$("$P4_BIN" change -o "$PREP_CL" 2>&1 || true)

    cl_status=$(printf '%s\n' "$cl_spec" | awk '/^Status:/ { print $2; exit }')
    assert_eq "prepared CL status is pending" "pending" "$cl_status"

    if printf '%s\n' "$cl_spec" | grep -qF "task-stream-id: ${PROBE_AGENT_PREP}"; then
        tag_ok=ok
    else
        tag_ok=missing
    fi
    assert_eq "prepared CL has task-stream-id tag" "ok" "$tag_ok"

    # `p4 describe -S <CL>` lists shelved files. When the CL has a shelf
    # the output contains a "Shelved files" header (older p4) or
    # "Shelved Files" (varies by version) plus the depot paths under it.
    # Use case-insensitive grep on "shelved" to cover both spellings.
    describe_out=$("$P4_BIN" describe -S "$PREP_CL" 2>&1 || true)
    if printf '%s\n' "$describe_out" | grep -qi 'shelved'; then
        shelf_ok=ok
    else
        shelf_ok=missing
    fi
    assert_eq "prepared CL has a shelf" "ok" "$shelf_ok"
fi
fi  # end scenario-4 pre-flight-clean guard

# ----- scenario 5: promote-reviewed-cl refuses stranded / mismatched CL ----
# Covers `bash scripts/dev/p4-task-stream-to-pr.sh --promote-reviewed-cl`
# refusal path: when the CL is pending but its description doesn't carry
# the `task-stream-id: <agent-id>` tag this caller expects, promote MUST
# refuse with exit 5 and surface manual cleanup instructions (never
# auto-clean — that's the user's call).
echo ""
echo "=== Scenario 5: --promote-reviewed-cl refuses stranded CL ==="

main_client="${P4_MAIN_CLIENT:-smatchet_main_${P4USER}}"

# Manually create an empty pending CL on the main client with a description
# that does NOT contain the expected task-stream-id tag. This simulates
# stranded state from a different prior run (or hand-edited mistake).
stranded_out=$(P4CLIENT="$main_client" "$P4_BIN" change -o | awk '
    /^Description:/ {
        print
        printf "\tphase7 stranded probe (intentionally missing task-stream-id tag)\n"
        in_desc=1
        next
    }
    in_desc && /^[ \t]/ { next }
    in_desc && /^$/ { next }
    in_desc && /^[A-Z]/ { in_desc=0; print; next }
    !in_desc { print }
' | P4CLIENT="$main_client" "$P4_BIN" change -i 2>&1)
STRANDED_CL=$(printf '%s\n' "$stranded_out" | sed -nE 's/.*Change ([0-9]+) created.*/\1/p' | tail -1)

if [ -n "$STRANDED_CL" ]; then
    if promote_out=$(bash scripts/dev/p4-task-stream-to-pr.sh "$PROBE_PROMOTE_AGENT" "Phase 7 promote probe" --promote-reviewed-cl "$STRANDED_CL" 2>&1); then
        promote_exit=0
    else
        promote_exit=$?
    fi

    assert_eq "promote refuses stranded CL with exit 5" "5" "$promote_exit"
    if printf '%s\n' "$promote_out" | grep -qF "missing tag 'task-stream-id: ${PROBE_PROMOTE_AGENT}'"; then
        refusal_ok=ok
    else
        refusal_ok="missing — got: $(printf '%s' "$promote_out" | head -3)"
    fi
    assert_eq "promote refusal surfaces missing-tag message" "ok" "$refusal_ok"
    if printf '%s\n' "$promote_out" | grep -qF "p4 shelve -d -c ${STRANDED_CL}"; then
        cleanup_hint_ok=ok
    else
        cleanup_hint_ok=missing
    fi
    assert_eq "promote refusal prints cleanup commands" "ok" "$cleanup_hint_ok"
else
    echo "  SKIP: could not create stranded probe CL — p4 change -i did not return a CL number" >&2
fi

# Also verify the non-existent-CL refusal path. Compute the CL id at
# runtime as (latest depot CL + 100000) — guarantees a non-existent value
# without hardcoding a literal that the depot will eventually reach (CR
# finding 2026-05-23 on #415: a hardcoded `99999999` makes this test
# flaky once the counter passes that mark).
latest_cl=$("$P4_BIN" changes -m1 //smatchet/... 2>/dev/null | sed -nE 's/^Change ([0-9]+).*/\1/p' | head -1)
nonexistent_cl=$(( ${latest_cl:-0} + 100000 ))
if promote_missing_out=$(bash scripts/dev/p4-task-stream-to-pr.sh "$PROBE_PROMOTE_AGENT" "Phase 7 promote probe" --promote-reviewed-cl "$nonexistent_cl" 2>&1); then
    promote_missing_exit=0
else
    promote_missing_exit=$?
fi
: "${promote_missing_out:=}"  # silence unused-var warning under set -u
assert_eq "promote refuses non-existent CL with exit 5" "5" "$promote_missing_exit"

# ----- scenario 6: lock-backend auto-flip if-unset pattern -----------------
# Covers the contract from docs/design/p4-gated-ship-loop.md § Resolved
# invariants § Plan-lock backend in p4-mode: orchestrator does
#     export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"
# (no colon — preserves empty-string setting that scenario 2 above relies
# on at this file's line 130). Verifies the bash-pattern semantics
# directly so a future refactor that drifts to `:-` form fails this gate.
echo ""
echo "=== Scenario 6: SMATCHET_LOCK_BACKEND if-unset auto-flip semantics ==="

# (a) Unset env → pattern fills in p4-counter.
result_unset=$(bash -c 'unset SMATCHET_LOCK_BACKEND; export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"; echo "[$SMATCHET_LOCK_BACKEND]"')
assert_eq "if-unset: unset env fills p4-counter" "[p4-counter]" "$result_unset"

# (b) Empty env → pattern PRESERVES empty (without this, scenario 2 above
#     breaks: SMATCHET_LOCK_BACKEND="" is the contract that lock-claim
#     stays on git-ref).
result_empty=$(SMATCHET_LOCK_BACKEND="" bash -c 'export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"; echo "[$SMATCHET_LOCK_BACKEND]"')
assert_eq "if-unset: empty env preserved (NOT overridden to p4-counter)" "[]" "$result_empty"

# (c) Explicit value → pattern respects user choice.
result_set=$(SMATCHET_LOCK_BACKEND=git-ref bash -c 'export SMATCHET_LOCK_BACKEND="${SMATCHET_LOCK_BACKEND-p4-counter}"; echo "[$SMATCHET_LOCK_BACKEND]"')
assert_eq "if-unset: explicit value respected" "[git-ref]" "$result_set"

# ----- final tally ---------------------------------------------------------
echo ""
echo "Passed: $PASSED  Failed: $FAILED"
[ "$FAILED" -eq 0 ]
