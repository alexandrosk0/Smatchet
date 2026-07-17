#!/usr/bin/env bats
# tests/bats/verifier_endpoint.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/verifier-endpoint.py — the dependency-free,
# OpenAI-compatible verifier stub endpoint. Unlike the other verifier suites
# (which inject transports), this one starts the server and drives the loop over
# REAL HTTP, proving the live path end to end with no model / key / GPU.
# Docs: docs/agent-rules/verifier-sidecar.md § Calibration.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    EP="$REPO_ROOT/scripts/dev/verifier-endpoint.py"
    PRODUCE="$REPO_ROOT/scripts/dev/verifier-produce.py"
    SIDECAR="$REPO_ROOT/scripts/dev/verifier-sidecar.py"
    export EP PRODUCE SIDECAR

    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK
    cat > "$WORK/job.json" <<'JSON'
{"problem":"fix the null-deref crash on shutdown",
 "criteria":["correctness","security"],
 "candidates":[{"id":"fix-a","text":"guard the pointer and add a shutdown-order test"},
               {"id":"fix-b","text":"swallow all shutdown errors"}]}
JSON
}

# Start the endpoint, capture its ephemeral URL into $URL, remember $EP_PID.
start_endpoint() {
    "$PY" "$EP" "$@" > "$WORK/ep.out" 2>"$WORK/ep.err" &
    EP_PID=$!
    local i
    for i in $(seq 1 100); do
        if grep -q '^endpoint: ' "$WORK/ep.out" 2>/dev/null; then break; fi
        if ! kill -0 "$EP_PID" 2>/dev/null; then
            echo "endpoint died on startup:" >&2; cat "$WORK/ep.err" >&2; return 1
        fi
        sleep 0.1
    done
    URL="$(sed -n 's/^endpoint: //p' "$WORK/ep.out")"
    [ -n "$URL" ] || { echo "no endpoint URL captured" >&2; return 1; }
}

teardown() {
    [ -n "${EP_PID:-}" ] && kill "$EP_PID" 2>/dev/null
    return 0
}

@test "selftest passes (dogfood)" {
    run "$PY" "$EP" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "logprobs mode: produce over HTTP pipes into the sidecar" {
    start_endpoint
    run bash -c "VERIFIER_BASE_URL='$URL' '$PY' '$PRODUCE' '$WORK/job.json' --model stub | '$PY' '$SIDECAR' aggregate -"
    [ "$status" -eq 0 ]
    OUTPUT="$output" "$PY" - <<'PY'
import json, os
d = json.loads(os.environ["OUTPUT"])
ids = {c["id"] for c in d["candidates"]}
assert ids == {"fix-a", "fix-b"}, ids
for c in d["candidates"]:
    assert 0.0 <= c["overall_score"] <= 1.0, c
assert d.get("ranking"), "expected a ranking for 2 candidates"
PY
}

@test "--record over HTTP writes a trace that replays offline" {
    start_endpoint
    run bash -c "VERIFIER_BASE_URL='$URL' '$PY' '$PRODUCE' '$WORK/job.json' --model stub --record '$WORK/trace.json' >/dev/null"
    [ "$status" -eq 0 ]
    # 2 candidates x 2 criteria x 1 repeat = 4 recorded responses.
    run "$PY" -c "import json,sys; print(len(json.load(open('$WORK/trace.json'))))"
    [ "$status" -eq 0 ]
    [ "$output" -eq 4 ]
    # The trace replays with no server.
    run bash -c "'$PY' '$PRODUCE' '$WORK/job.json' --model stub --responses '$WORK/trace.json' | '$PY' '$SIDECAR' aggregate - >/dev/null"
    [ "$status" -eq 0 ]
}

@test "--fixed A makes every candidate score near 1.0" {
    start_endpoint --fixed A
    run bash -c "VERIFIER_BASE_URL='$URL' '$PY' '$PRODUCE' '$WORK/job.json' --model stub | '$PY' '$SIDECAR' aggregate -"
    [ "$status" -eq 0 ]
    OUTPUT="$output" "$PY" - <<'PY'
import json, os
d = json.loads(os.environ["OUTPUT"])
for c in d["candidates"]:
    assert c["overall_score"] > 0.95, (c["id"], c["overall_score"])
PY
}

@test "scalar mode works over HTTP too" {
    start_endpoint
    run bash -c "VERIFIER_BASE_URL='$URL' '$PY' '$PRODUCE' '$WORK/job.json' --model stub --mode scalar | '$PY' '$SIDECAR' aggregate -"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"overall_score"'* ]]
}

@test "health endpoint responds; malformed request is a 400" {
    start_endpoint
    run "$PY" - "$URL" <<'PY'
import sys, json, urllib.request, urllib.error
u = sys.argv[1]
assert json.load(urllib.request.urlopen(u + "/health"))["status"] == "ok"
try:
    urllib.request.urlopen(urllib.request.Request(u + "/chat/completions", data=b"{bad", method="POST"))
except urllib.error.HTTPError as e:
    assert e.code == 400, e.code
else:
    raise AssertionError("expected HTTP 400 for malformed body")
print("ok")
PY
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok"* ]]
}
