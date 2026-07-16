#!/usr/bin/env bats
# tests/bats/verifier_produce.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/verifier-produce.py — the model-calling half of the
# verifier sidecar. Runs with NO network: a recorded --responses fixture replays
# chat-completion bodies in call order, so the producer's logprob extraction and
# job assembly are exercised deterministically.
#
# Proves:
#   * the self-test dogfoods;
#   * produce() with a replay fixture emits sidecar-shaped {candidates:[samples]}
#     with per-criterion {logprobs};
#   * the output pipes straight into verifier-sidecar.py aggregate - (stdin);
#   * scalar mode emits plain floats;
#   * malformed input / missing backend fail with exit 2.
# Docs: docs/agent-rules/verifier-sidecar.md.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    PRODUCE="$REPO_ROOT/scripts/dev/verifier-produce.py"
    SIDECAR="$REPO_ROOT/scripts/dev/verifier-sidecar.py"
    export PRODUCE SIDECAR

    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK

    cat > "$WORK/job.json" <<'JSON'
{"problem":"fix the search crash","criteria":["security","correctness"],
 "candidates":[{"id":"fix-a","text":"use a parameterized query"},
               {"id":"fix-b","text":"build SQL by string concatenation"}]}
JSON
    # 4 calls in order: fix-a/security, fix-a/correctness, fix-b/security, fix-b/correctness
    "$PY" - "$WORK/responses.json" <<'PY'
import json, math, sys
def r(letter, alt):
    return {"choices":[{"message":{"content":letter},
      "logprobs":{"content":[{"token":letter,"logprob":math.log(0.85),
        "top_logprobs":[{"token":letter,"logprob":math.log(0.85)},
                        {"token":alt,"logprob":math.log(0.15)}]}]}}]}
json.dump([r("A","B"), r("B","A"), r("R","P"), r("D","B")], open(sys.argv[1],"w"))
PY
}

@test "selftest passes (dogfood)" {
    run "$PY" "$PRODUCE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "produce (replay) emits sidecar-shaped criteria logprobs" {
    run "$PY" "$PRODUCE" "$WORK/job.json" --responses "$WORK/responses.json"
    [ "$status" -eq 0 ]
    "$PY" - <<PY
import json
d = json.loads('''$output''')
ids = [c["id"] for c in d["candidates"]]
assert ids == ["fix-a", "fix-b"], ids
crit = d["candidates"][0]["samples"][0]["criteria"]
assert set(crit) == {"security", "correctness"}, crit
assert "logprobs" in crit["security"], crit
assert "A" in crit["security"]["logprobs"], crit
PY
}

@test "producer output pipes into the sidecar over stdin" {
    run bash -c "'$PY' '$PRODUCE' '$WORK/job.json' --responses '$WORK/responses.json' | '$PY' '$SIDECAR' aggregate -"
    [ "$status" -eq 0 ]
    "$PY" - <<PY
import json
d = json.loads('''$output''')
by = {c["id"]: c for c in d["candidates"]}
# fix-a scored high (A/B), fix-b low (R/D) -> fix-a leads and is not blocked.
assert by["fix-a"]["overall_score"] > by["fix-b"]["overall_score"], d
assert d["ranking"][0]["id"] == "fix-a", d["ranking"]
PY
}

@test "scalar mode emits plain floats" {
    printf '[{"choices":[{"message":{"content":"A"}}]},{"choices":[{"message":{"content":"F"}}]},{"choices":[{"message":{"content":"T"}}]},{"choices":[{"message":{"content":"C"}}]}]' > "$WORK/scalar.json"
    run "$PY" "$PRODUCE" "$WORK/job.json" --responses "$WORK/scalar.json" --mode scalar
    [ "$status" -eq 0 ]
    "$PY" - <<PY
import json
d = json.loads('''$output''')
sc = d["candidates"][0]["samples"][0]["criteria"]
assert sc["security"] == 1.0, sc
assert isinstance(sc["correctness"], float), sc
PY
}

@test "malformed job fails with exit 2" {
    echo '{bad' > "$WORK/bad.json"
    run "$PY" "$PRODUCE" "$WORK/bad.json" --responses "$WORK/responses.json"
    [ "$status" -eq 2 ]
    [[ "$output" == *"ERROR"* ]]
}

@test "missing backend (no --responses, no base url) fails with exit 2" {
    run env -u VERIFIER_BASE_URL "$PY" "$PRODUCE" "$WORK/job.json" --model stub
    [ "$status" -eq 2 ]
    [[ "$output" == *"ERROR"* ]]
}
