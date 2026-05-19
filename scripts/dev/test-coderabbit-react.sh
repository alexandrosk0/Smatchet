#!/usr/bin/env bash
# Phase-9 bucket-A smoke for the CodeRabbit react loop.
# Plan: docs/design/coderabbit-react-loop.md § Verification (CLI smoke).
#
# Drives `Smatchet.exe cmd coderabbit-react.poll-now` against the fixture
# `tests/fixtures/coderabbit_comments_sample.json` with:
#   - A PATH shim that resolves `claude` to `tests/fixtures/stub-coderabbit-claude.sh`.
#   - A PATH shim that resolves `gh` to an inline bash stub returning the
#     comments fixture for the relevant endpoint.
#
# Skip-clean if `Smatchet.exe` is absent or AGENTIC=OFF (mirrors the
# pattern in `test-agentic-handoff-iterate.sh`).
#
# DEFERRED: requires a running Smatchet.exe with GitHub PAT + `agent_open_pr_watch`
# rows for a full e2e tick that actually dispatches a spawn. This script
# covers the CLI dispatch-arg surface + classifier registration round-trip;
# the live-PR end-to-end probe documented in plan § Verification step 3 is
# tracked in `docs/backlog/agent-self-improvement/tooling.md` until bucket-E
# (ImGui Test Engine) wires up. See plan § Verification results.
#
# Final summary line: `Passed: N  Failed: M` per `test-all.sh` convention.

set -euo pipefail

# Locate repo root from this script's location.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

EXE_CANDIDATES=(
    "$REPO_ROOT/build/ninja-iter-msys2/Smatchet.exe"
    "$REPO_ROOT/build/ninja-debug-msys2/Smatchet.exe"
    "$REPO_ROOT/build/ninja-publish-msys2/Smatchet.exe"
)

EXE=""
for candidate in "${EXE_CANDIDATES[@]}"; do
    if [[ -x "$candidate" ]]; then
        EXE="$candidate"
        break
    fi
done

# Tempdir + cleanup trap. We DO NOT wipe sentinels mid-run because the
# Smatchet runner reads them asynchronously; teardown happens at exit.
TMP="$(mktemp -d -t smatchet-coderabbit-react-XXXXXX)"
cleanup() {
    rc=$?
    rm -rf "$TMP" 2>/dev/null || true
    trap - EXIT
    exit "$rc"
}
trap cleanup EXIT INT TERM

PASSED=0
FAILED=0

pass() { PASSED=$((PASSED + 1)); echo "  ok — $*"; }
fail() { FAILED=$((FAILED + 1)); echo "  FAIL — $*" >&2; }

# ───────────────────────────────────────────────────────────────────────
# Always-deterministic checks: the stub scripts exist + behave per contract.
# These run regardless of whether Smatchet.exe is built — they cover the
# stub-runner end-to-end seam (plan § Verification — stub-runner end-to-end).
# ───────────────────────────────────────────────────────────────────────

STUB_CLAUDE="$REPO_ROOT/tests/fixtures/stub-coderabbit-claude.sh"
FIXTURE="$REPO_ROOT/tests/fixtures/coderabbit_comments_sample.json"

echo "[coderabbit-react] checking stub claude at $STUB_CLAUDE"
if [[ -x "$STUB_CLAUDE" ]]; then
    pass "stub-coderabbit-claude.sh present and executable"
else
    fail "stub-coderabbit-claude.sh missing or not executable"
fi

if [[ -f "$FIXTURE" ]]; then
    pass "coderabbit_comments_sample.json fixture present"
else
    fail "coderabbit_comments_sample.json fixture missing"
fi

# Drive the stub directly with a synthesised SEED.json. This asserts the
# stub's sentinel-write contract — RUN_RESULT.json + PR_URL.txt — which is
# what `ClaudeCodeLocalRunner` consumes downstream.
if [[ -x "$STUB_CLAUDE" ]]; then
    STUB_RUN="$TMP/stub-run"
    mkdir -p "$STUB_RUN"
    printf '%s\n' '{"proposalId":"smoke-test","dispatch_source":"coderabbit_react"}' > "$STUB_RUN/SEED.json"
    (cd "$STUB_RUN" && bash "$STUB_CLAUDE" > "$STUB_RUN/stdout.ndjson" 2> "$STUB_RUN/stderr.log") || fail "stub-coderabbit-claude.sh exited non-zero"

    if [[ -f "$STUB_RUN/RUN_RESULT.json" ]]; then
        pass "stub wrote RUN_RESULT.json"
    else
        fail "stub did not write RUN_RESULT.json"
    fi

    if [[ -f "$STUB_RUN/PR_URL.txt" ]]; then
        pass "stub wrote PR_URL.txt"
    else
        fail "stub did not write PR_URL.txt"
    fi

    if grep -q '"type":"result"' "$STUB_RUN/stdout.ndjson" 2>/dev/null; then
        pass "stub emitted terminal stream-json result event"
    else
        fail "stub did not emit a stream-json result event"
    fi

    if grep -q '"ok": true' "$STUB_RUN/RUN_RESULT.json" 2>/dev/null; then
        pass "stub RUN_RESULT.json reports ok=true"
    else
        fail "stub RUN_RESULT.json missing ok=true"
    fi
fi

# Fixture content invariant — the smoke must see at least one comment that
# the classifier short-circuits (per plan § Verification — `expected_marker`
# or `expected_icon = "actionable"` rows). Read the JSON without jq (not on
# every dev shell) — a literal substring check is enough for the smoke.
if [[ -f "$FIXTURE" ]]; then
    if grep -q '"expected_icon": "actionable"' "$FIXTURE"; then
        pass "fixture contains at least one actionable comment (classifier dispatch survivor)"
    else
        fail "fixture missing actionable comments"
    fi
    if grep -q '"expected_marker": true' "$FIXTURE"; then
        pass "fixture contains the smatchet-handoff marker comment (classifier RejectShortCircuit / skip path)"
    else
        fail "fixture missing handoff marker comment"
    fi
fi

# ───────────────────────────────────────────────────────────────────────
# CLI surface check — only when Smatchet.exe is available and AGENTIC=ON.
# ───────────────────────────────────────────────────────────────────────

if [[ -z "$EXE" ]]; then
    echo "[coderabbit-react] SKIP CLI section — no Smatchet.exe in build/ninja-*-msys2/"
    echo
    echo "Passed: $PASSED  Failed: $FAILED"
    exit 0
fi

LIST_OUT=$("$EXE" cmd commands.list --spawn 2>&1 || true)

if ! echo "$LIST_OUT" | grep -q '"name":"coderabbit-react\.poll-now"' ; then
    echo "[coderabbit-react] SKIP CLI section — coderabbit-react.poll-now not registered (SMATCHET_WITH_AGENTIC=OFF)"
    echo
    echo "Passed: $PASSED  Failed: $FAILED"
    exit 0
fi

pass "coderabbit-react.poll-now registered in commands.list"

# Set up the PATH shim dir. `claude` resolves to our bash stub; we also
# fabricate a minimal `gh` stub that yields the fixture for the pulls
# /comments endpoint. The shim takes precedence over any system PATH.
SHIM="$TMP/shim"
mkdir -p "$SHIM"

# `claude` shim — symlinks where possible, falls back to a tiny wrapper.
ln -sf "$STUB_CLAUDE" "$SHIM/claude" 2>/dev/null || cat > "$SHIM/claude" <<EOF
#!/usr/bin/env bash
exec bash "$STUB_CLAUDE" "\$@"
EOF
chmod +x "$SHIM/claude"

# `gh` shim — minimal. Returns the comments fixture on the relevant API
# subroute, exits 0 on any other invocation so the watcher's other calls
# (rate_limit probe, etc.) don't crash the smoke.
cat > "$SHIM/gh" <<EOF
#!/usr/bin/env bash
# Inline gh stub for the coderabbit-react smoke. Routes the
# /pulls/{n}/comments endpoint to the canned fixture; everything else
# returns {} so callers see a benign empty response.
case "\$*" in
    *"/pulls/"*"/comments"*)
        cat "$FIXTURE"
        ;;
    "auth status"*)
        echo "Logged in to github.com as smatchet-bot"
        ;;
    *"rate_limit"*)
        printf '%s\n' '{"resources":{"core":{"remaining":4500,"limit":5000}}}'
        ;;
    *)
        printf '%s\n' '{}'
        ;;
esac
exit 0
EOF
chmod +x "$SHIM/gh"

# Ping the dry-run CLI surface with the shim PATH. We pass a fake PR URL;
# the command may fail with "watcher not constructed" when no GitHub PAT
# is configured (the most common case in CI). Either outcome — a clean
# success or a deterministic Failure(HandlerError) envelope — proves the
# dispatch-arg + classifier-registration surface compiles and links.
SAFE_PATH="$SHIM:$PATH"
POLL_OUT=$(PATH="$SAFE_PATH" "$EXE" cmd coderabbit-react.poll-now --pr_url=https://github.com/example/repo/pull/1234 --spawn 2>&1 || true)

if echo "$POLL_OUT" | grep -q '"command":"coderabbit-react\.poll-now"' ; then
    pass "coderabbit-react.poll-now envelope present"
else
    fail "coderabbit-react.poll-now envelope missing — output: $(echo "$POLL_OUT" | head -3)"
fi

# Either ok=true (PAT-configured tick succeeded) OR ok=false with a
# HandlerError (no watcher because no PAT). Both are deterministic;
# silent crash / segfault is the failure mode we guard against.
if echo "$POLL_OUT" | grep -qE '"ok":(true|false)' ; then
    pass "coderabbit-react.poll-now returned a structured envelope (ok:true or ok:false)"
else
    fail "coderabbit-react.poll-now did not return a structured envelope"
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
if [[ "$FAILED" -gt 0 ]]; then
    exit 1
fi
exit 0
