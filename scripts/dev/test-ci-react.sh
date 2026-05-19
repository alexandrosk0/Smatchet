#!/usr/bin/env bash
# Phase-9 bucket-A smoke for the CI react loop.
# Plan: docs/design/coderabbit-react-loop.md § Verification (CLI smoke).
#
# Drives `Smatchet.exe cmd ci-react.poll-now` against the fixture
# `tests/fixtures/check_runs_failed_sample.json` with PATH shims for
# `claude` (→ tests/fixtures/stub-ci-claude.sh) and `gh` (inline).
#
# Skip-clean if `Smatchet.exe` is absent or AGENTIC=OFF.
#
# Per plan § Phased rollout phase 9, fixture lines up with five buckets:
#   - CmakeConfigError    → build-doctor spawn
#   - CtestFailure        → debug-detective (default flag-off → no spawn)
#   - SanitizerHit        → debug-detective (flag-gated)
#   - TransientFlake      → workflow rerun (no spawn)
#   - CoverageGate        → test-rig spawn
#   - Unknown             → skip + Self-improvement log
#
# DEFERRED: a real Smatchet.exe driving the dispatch is required for the
# spawn-vs-skip count assertions. This script covers the dispatch-arg +
# classifier surface deterministically; the bucket-count probe documented
# in plan § Verification step 4 lands once bucket-E (ImGui Test Engine)
# is wired (see `docs/backlog/agent-self-improvement/tooling.md`).
#
# Final summary line: `Passed: N  Failed: M`.

set -euo pipefail

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

TMP="$(mktemp -d -t smatchet-ci-react-XXXXXX)"
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
# Deterministic stub + fixture checks.
# ───────────────────────────────────────────────────────────────────────

STUB_CLAUDE="$REPO_ROOT/tests/fixtures/stub-ci-claude.sh"
FIXTURE="$REPO_ROOT/tests/fixtures/check_runs_failed_sample.json"

echo "[ci-react] checking stub claude at $STUB_CLAUDE"
if [[ -x "$STUB_CLAUDE" ]]; then
    pass "stub-ci-claude.sh present and executable"
else
    fail "stub-ci-claude.sh missing or not executable"
fi

if [[ -f "$FIXTURE" ]]; then
    pass "check_runs_failed_sample.json fixture present"
else
    fail "check_runs_failed_sample.json fixture missing"
fi

if [[ -x "$STUB_CLAUDE" ]]; then
    STUB_RUN="$TMP/stub-run"
    mkdir -p "$STUB_RUN"
    printf '%s\n' '{"proposalId":"smoke-test","dispatch_source":"ci_react"}' > "$STUB_RUN/SEED.json"
    # Phase-6 invariant: ci-react dispatch writes CHECK_RUN.json next to
    # SEED.json. Synthesise a minimal payload so the stub exercises its
    # CHECK_RUN-aware branch.
    cat > "$STUB_RUN/CHECK_RUN.json" <<'JSON'
{"id":100001,"name":"Windows + MSYS2 UCRT64","conclusion":"failure","expected_category":"CmakeOrCtest","expected_kind":"CmakeConfigError"}
JSON
    (cd "$STUB_RUN" && bash "$STUB_CLAUDE" > "$STUB_RUN/stdout.ndjson" 2> "$STUB_RUN/stderr.log") || fail "stub-ci-claude.sh exited non-zero"

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

    # The build-doctor fix simulation should mention `cmake --build`.
    if grep -q 'cmake --build' "$STUB_RUN/stdout.ndjson" 2>/dev/null; then
        pass "stub emitted a synthetic build-doctor cmake --build line"
    else
        fail "stub did not emit a cmake --build line"
    fi
fi

# Fixture content invariant — the five buckets must all be present so the
# classifier sees one of each on a real run.
if [[ -f "$FIXTURE" ]]; then
    for kind in CmakeConfigError CtestFailure SanitizerHit TransientFlake Unknown ; do
        if grep -q "\"expected_kind\": \"$kind\"" "$FIXTURE"; then
            pass "fixture contains expected_kind=$kind"
        else
            fail "fixture missing expected_kind=$kind"
        fi
    done
    if grep -q '"expected_category": "CoverageGate"' "$FIXTURE"; then
        pass "fixture contains coverage-gate category (test-rig dispatch path)"
    else
        fail "fixture missing coverage-gate category"
    fi
fi

# ───────────────────────────────────────────────────────────────────────
# CLI surface check — only when Smatchet.exe is available and AGENTIC=ON.
# ───────────────────────────────────────────────────────────────────────

if [[ -z "$EXE" ]]; then
    echo "[ci-react] SKIP CLI section — no Smatchet.exe in build/ninja-*-msys2/"
    echo
    echo "Passed: $PASSED  Failed: $FAILED"
    exit 0
fi

LIST_OUT=$("$EXE" cmd commands.list --spawn 2>&1 || true)

if ! echo "$LIST_OUT" | grep -q '"name":"ci-react\.poll-now"' ; then
    echo "[ci-react] SKIP CLI section — ci-react.poll-now not registered (SMATCHET_WITH_AGENTIC=OFF)"
    echo
    echo "Passed: $PASSED  Failed: $FAILED"
    exit 0
fi

pass "ci-react.poll-now registered in commands.list"

# Sibling commands must also be present (phase 8 surface).
for cmd in ci-react.start ci-react.stop ci-react.status ci-react.rerun ; do
    if echo "$LIST_OUT" | grep -q "\"name\":\"${cmd}\"" ; then
        pass "$cmd registered"
    else
        fail "$cmd not registered after phase 8"
    fi
done

# Set up the PATH shim dir with `claude` → stub-ci-claude.sh + `gh` inline.
SHIM="$TMP/shim"
mkdir -p "$SHIM"
ln -sf "$STUB_CLAUDE" "$SHIM/claude" 2>/dev/null || cat > "$SHIM/claude" <<EOF
#!/usr/bin/env bash
exec bash "$STUB_CLAUDE" "\$@"
EOF
chmod +x "$SHIM/claude"

cat > "$SHIM/gh" <<EOF
#!/usr/bin/env bash
# Inline gh stub for the ci-react smoke. Routes /check-runs queries to the
# canned fixture; everything else returns {} so callers see benign empties.
case "\$*" in
    *"/check-runs"*)
        cat "$FIXTURE"
        ;;
    "workflow run"*)
        # Simulate a successful workflow rerun call.
        printf '%s\n' '{"ok":true}'
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

SAFE_PATH="$SHIM:$PATH"
POLL_OUT=$(PATH="$SAFE_PATH" "$EXE" cmd ci-react.poll-now --pr_url=https://github.com/example/repo/pull/1234 --spawn 2>&1 || true)

if echo "$POLL_OUT" | grep -q '"command":"ci-react\.poll-now"' ; then
    pass "ci-react.poll-now envelope present"
else
    fail "ci-react.poll-now envelope missing — output: $(echo "$POLL_OUT" | head -3)"
fi

if echo "$POLL_OUT" | grep -qE '"ok":(true|false)' ; then
    pass "ci-react.poll-now returned a structured envelope"
else
    fail "ci-react.poll-now did not return a structured envelope"
fi

echo
echo "Passed: $PASSED  Failed: $FAILED"
if [[ "$FAILED" -gt 0 ]]; then
    exit 1
fi
exit 0
