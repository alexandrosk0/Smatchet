#!/usr/bin/env bats
# tests/bats/verifier_sidecar.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/verifier-sidecar.py — the deterministic half of the
# LLM-as-a-Verifier pattern (advisory aggregator; no model call).
#
# Proves the AGGREGATION CONTRACT with fixed JSON fixtures (no live model):
#   * fine-grained logprob reward is an expectation over the score scale;
#   * a hard veto forces `block` and sinks the candidate in the ranking;
#   * the pivot-tournament budget is O(Nk), below round-robin;
#   * progress tracking classifies rising vs stuck-and-abandon;
#   * malformed input fails with exit 2 (the asserts-failure path).
# Plan: docs/plans/llm-verifier-sidecar.md.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SIDECAR="$REPO_ROOT/scripts/dev/verifier-sidecar.py"
    export SIDECAR

    # Pick a WORKING python 3 interpreter (mirrors agent_eval_score.bats: a bare
    # `command -v python3` matches the Windows WinStore alias that exits 49).
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK
}

@test "selftest passes (dogfood)" {
    run "$PY" "$SIDECAR" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "fine-grained reward is an expectation over the score scale" {
    # Mass peaked on A (best) -> reward near 1.0; valid_mass 1.0.
    "$PY" - <<'PY' > "$WORK/lp.json"
import json, math
print(json.dumps({"logprobs": {"A": math.log(0.9), "B": math.log(0.1)}}))
PY
    run "$PY" "$SIDECAR" reward "$WORK/lp.json"
    [ "$status" -eq 0 ]
    "$PY" - "$WORK" <<'PY'
import json, subprocess, sys, os
out = subprocess.check_output([os.environ["PY"], os.environ["SIDECAR"], "reward",
                               os.path.join(sys.argv[1], "lp.json")]).decode()
d = json.loads(out)
assert d["reward"] > 0.9, d
assert d["valid_mass"] == 1.0, d
PY
}

@test "hard veto forces block and sinks the candidate in the ranking" {
    cat > "$WORK/multi.json" <<'JSON'
{"candidates":[
 {"id":"safe","samples":[{"criteria":{"correctness":0.9,"security":0.95},"confidence":0.9}]},
 {"id":"risky","samples":[{"criteria":{"correctness":0.8,"security":0.2},"confidence":0.6,
   "hard_veto":true,"veto_reason":"injection"}]}
]}
JSON
    run "$PY" "$SIDECAR" "$WORK/multi.json"
    [ "$status" -eq 0 ]
    "$PY" - "$WORK" <<'PY'
import json, subprocess, sys, os
out = subprocess.check_output([os.environ["PY"], os.environ["SIDECAR"],
                               os.path.join(sys.argv[1], "multi.json")]).decode()
d = json.loads(out)
by_id = {c["id"]: c for c in d["candidates"]}
assert by_id["risky"]["recommendation"] == "block", by_id["risky"]
assert by_id["safe"]["recommendation"] in ("accept", "escalate"), by_id["safe"]
assert d["ranking"][0]["id"] == "safe", d["ranking"]
assert d["ranking"][-1]["id"] == "risky", d["ranking"]
PY
}

@test "pivot-tournament budget is O(Nk), below round-robin" {
    cat > "$WORK/four.json" <<'JSON'
{"candidates":[
 {"id":"a","samples":[{"overall_score":0.9}]},
 {"id":"b","samples":[{"overall_score":0.8}]},
 {"id":"c","samples":[{"overall_score":0.7}]},
 {"id":"d","samples":[{"overall_score":0.6}]}
]}
JSON
    "$PY" - "$WORK" <<'PY'
import json, subprocess, sys, os
out = subprocess.check_output([os.environ["PY"], os.environ["SIDECAR"],
                               os.path.join(sys.argv[1], "four.json")]).decode()
t = json.loads(out)["pivot_tournament"]
# N=4, k=2 -> N + k(N-k) + C(k,2) = 4 + 4 + 1 = 9 < 12 round-robin.
assert t["expected_comparisons"] == 9, t
assert len(t["ring_pairs"]) == 4, t
assert len(t["pivots"]) == 2, t
PY
}

@test "progress tracking: rising curve does not early-stop; stuck curve does" {
    echo '{"steps":[0.1,0.3,0.5,0.7,0.9]}' > "$WORK/rising.json"
    echo '{"steps":[0.05,0.04,0.03,0.05,0.02],"min_steps":4}' > "$WORK/stuck.json"
    "$PY" - "$WORK" <<'PY'
import json, subprocess, sys, os
def run(name):
    out = subprocess.check_output([os.environ["PY"], os.environ["SIDECAR"], "track",
                                   os.path.join(sys.argv[1], name)]).decode()
    return json.loads(out)
r = run("rising.json"); s = run("stuck.json")
assert r["overall_trend"] == "rising", r
assert r["early_stop_recommended"] is False, r
assert s["early_stop_recommended"] is True, s
PY
}

@test "malformed input fails with exit 2" {
    echo '{bad' > "$WORK/bad.json"
    run "$PY" "$SIDECAR" "$WORK/bad.json"
    [ "$status" -eq 2 ]
    [[ "$output" == *"ERROR"* ]]

    echo '{"candidates":[{"id":"x","samples":[]}]}' > "$WORK/empty.json"
    run "$PY" "$SIDECAR" "$WORK/empty.json"
    [ "$status" -eq 2 ]
}

@test "no input path is a usage error (exit 2)" {
    run "$PY" "$SIDECAR"
    [ "$status" -eq 2 ]
    [[ "$output" == *"ERROR"* ]]
}
