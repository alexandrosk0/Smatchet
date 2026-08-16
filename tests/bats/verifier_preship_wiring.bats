#!/usr/bin/env bats
# verifier_preship_wiring.bats — slice 2 of
# docs/plans/verifier-scored-code-review-gate.md: scripts/dev/pre-ship.sh driving the
# INDEPENDENT verifier at --ack-review time and attaching the verdict to the ack.
#
# Drives the REAL pipeline (verifier-produce.py | verifier-sidecar.py aggregate) over
# HTTP against scripts/dev/verifier-endpoint.py — a dependency-free canned
# OpenAI-compatible server — so the live path is exercised with no model, no API key
# and no network egress. The endpoint is NOT a model: it proves the WIRING, never
# scoring quality.
#
# The three invariants under test, in priority order:
#   1. No independent backend configured => NO verdict is recorded. The gate must never
#      fall back to the reviewing model scoring itself; a presence-only ack is the
#      correct, honest degrade.
#   2. Backend configured but failing => presence-only ack + a WARN, never a blocked
#      push. A gate that goes down when an API goes down is worse than no gate.
#   3. Backend healthy => verdict attached; the score is advisory, only hard_veto blocks.

setup() {
    ROOT="$(git rev-parse --show-toplevel)"
    export ROOT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    git init --quiet -b base "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    mkdir -p "$REPO_TMP/scripts/dev" \
        "$REPO_TMP/agents/scripts/core/lib" \
        "$REPO_TMP/Source/Core/src/Sync"
    cp "$ROOT/scripts/dev/pre-ship.sh" "$REPO_TMP/scripts/dev/pre-ship.sh"
    cp "$ROOT/scripts/dev/verifier-produce.py" "$REPO_TMP/scripts/dev/verifier-produce.py"
    cp "$ROOT/scripts/dev/verifier-sidecar.py" "$REPO_TMP/scripts/dev/verifier-sidecar.py"
    cp "$ROOT/agents/scripts/core/lib/review-ack.sh" "$REPO_TMP/agents/scripts/core/lib/review-ack.sh"
    printf '{"lint":{"zones":{"strict":["Source/Core/src/Sync/"]}}}\n' > "$REPO_TMP/project.config.json"
    echo "// base" > "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m base
    git -C "$REPO_TMP" checkout --quiet -b feature
    printf '// edit\nint touched() { return 1; }\n' >> "$REPO_TMP/Source/Core/src/Sync/S.cpp"
    git -C "$REPO_TMP" commit --quiet -am edit
    # The gate refuses an ack for a substantive diff that no review artifact covers.
    # These tests are about the VERDICT half, not artifact presence, so satisfy that
    # precondition once here; preship_review_artifact.bats owns the artifact rules.
    printf '{"fingerprint":"%s"}\n' "$(diff_fingerprint)" > "$REPO_TMP/.review-findings.json"
    ENDPOINT_PID=""
}

# diff_fingerprint — the sha256 pre-ship.sh pins an ack/artifact to, over the same
# first-party C++ path set ra_fingerprint uses.
diff_fingerprint() {
    (cd "$REPO_TMP" && git diff base...HEAD -- 'Source/Core/*.cpp' 'Source/Core/*.h' \
        'Source/Plugins/*.cpp' 'Source/Plugins/*.h' 'Source/Standalone/*.cpp' \
        'Source/Standalone/*.h' 'tests/*.cpp' 'tests/*.h' | sha256sum | cut -d' ' -f1)
}

teardown() {
    [ -n "${ENDPOINT_PID:-}" ] && kill "$ENDPOINT_PID" 2>/dev/null
    rm -rf "${REPO_TMP:-}"
}

# start_endpoint [--fixed <letter>] — boot the canned endpoint, export VERIFIER_BASE_URL.
start_endpoint() {
    local port out
    port=$(python3 -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()')
    python3 "$ROOT/scripts/dev/verifier-endpoint.py" --port "$port" "$@" >/dev/null 2>&1 &
    ENDPOINT_PID=$!
    export VERIFIER_BASE_URL="http://127.0.0.1:$port"
    export VERIFIER_MODEL=stub
    # Wait for the listener rather than sleeping a fixed interval.
    for _ in $(seq 1 50); do
        if python3 -c "
import socket,sys
s=socket.socket(); s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',$port))==0 else 1)"; then return 0; fi
        python3 -c 'import time;time.sleep(0.1)'
    done
    out="endpoint did not come up on $port"
    echo "$out" >&2
    return 1
}

ack() {
    (cd "$REPO_TMP" && SMATCHET_PRESHIP_GATE_ONLY=1 bash scripts/dev/pre-ship.sh --ack-review base 2>&1)
}

marker_fields() {
    awk -F'\t' '$1 == "branch" { print NF }' "$REPO_TMP/.review-ack"
}

@test "no backend configured: presence-only ack, never a self-scored verdict" {
    run env -u VERIFIER_BASE_URL -u VERIFIER_MODEL bash -c "cd '$REPO_TMP' && SMATCHET_PRESHIP_GATE_ONLY=1 bash scripts/dev/pre-ship.sh --ack-review base 2>&1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"review ACK recorded"* ]]
    [[ "$output" != *"verdict"* ]]
    # 2-field record — byte-identical to pre-slice-2 behaviour.
    [ "$(marker_fields)" = "2" ]
}

@test "backend configured but unreachable: WARN + presence-only ack, push not blocked" {
    export VERIFIER_BASE_URL="http://127.0.0.1:1"   # nothing listening
    export VERIFIER_MODEL=stub
    run ack
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"no usable verdict"* ]]
    [ "$(marker_fields)" = "2" ]
}

@test "healthy backend: verdict attached to the ack and reported as advisory" {
    start_endpoint
    run ack
    [ "$status" -eq 0 ]
    [[ "$output" == *"review ACK + verifier verdict recorded"* ]]
    [[ "$output" == *"ADVISORY"* ]]
    # 6-field record: fingerprint + score/recommendation/hard_veto/reason.
    [ "$(marker_fields)" = "6" ]
}

@test "a calibration trace is recorded for every produced verdict" {
    # Without traces, verifier-calibrate.py can never justify promoting the score from
    # advisory to blocking — the metric would be permanently un-promotable.
    start_endpoint
    run ack
    [ "$status" -eq 0 ]
    [[ "$output" == *".verifier-traces/"* ]]
    run bash -c "ls '$REPO_TMP'/.verifier-traces/*.json | wc -l"
    [ "$output" -ge 1 ]
}

@test "the recorded verdict is the sidecar's, not an invented one" {
    start_endpoint --fixed A
    ack
    # --fixed A pins every criterion to the best letter, so the aggregate score must be
    # high. This asserts the number came from the real pipeline rather than a default.
    run bash -c "awk -F'\t' '\$1 == \"branch\" { print \$3 }' '$REPO_TMP/.review-ack'"
    [ -n "$output" ]
    run python3 -c "import sys; sys.exit(0 if float('$output') > 0.9 else 1)"
    [ "$status" -eq 0 ]
}

@test "an empty diff produces no verdict rather than scoring nothing" {
    start_endpoint
    git -C "$REPO_TMP" checkout --quiet -b empty base
    run ack
    [ "$status" -eq 0 ]
    [ "$(marker_fields)" = "2" ]
}

@test "a recorded hard_veto blocks EVERY run, not just the ack invocation" {
    # Regression for the bypass found in adversarial review: --ack-review writes the ack
    # and THEN exits on the veto, so before this fix a plain re-run saw a matching
    # fingerprint and reported "ack current -> safe to push". The veto blocked one
    # invocation, never the gate.
    printf 'branch\t%s\t0.4\tblock\ttrue\tsecret written to the log\n' "$(diff_fingerprint)" \
        > "$REPO_TMP/.review-ack"
    run bash -c "cd '$REPO_TMP' && SMATCHET_PRESHIP_GATE_ONLY=1 bash scripts/dev/pre-ship.sh base 2>&1"
    [ "$status" -eq 1 ]
    [[ "$output" == *"HARD VETO"* ]]
    [[ "$output" == *"secret written to the log"* ]]
    [[ "$output" != *"Safe to push"* ]]
}

@test "a clean recorded verdict reports the score on the gate path without blocking" {
    printf 'branch\t%s\t0.88\taccept\tfalse\t\n' "$(diff_fingerprint)" \
        > "$REPO_TMP/.review-ack"
    run bash -c "cd '$REPO_TMP' && SMATCHET_PRESHIP_GATE_ONLY=1 bash scripts/dev/pre-ship.sh base 2>&1"
    [ "$status" -eq 0 ]
    [[ "$output" == *"score=0.88"* ]]
    [[ "$output" == *"ADVISORY"* ]]
}

@test "the produced verdict alone can never veto - worst-case score still passes" {
    # verifier-produce.py emits criteria only and never hard_veto, so even every criterion
    # pinned to the worst letter yields hard_veto=false. Pins the documented limitation so
    # nobody later claims the producer pipeline provides a veto safety net.
    start_endpoint --fixed T
    run ack
    [ "$status" -eq 0 ]
    [[ "$output" == *"hard_veto=false"* ]]
    [[ "$output" == *"ADVISORY"* ]]
}
