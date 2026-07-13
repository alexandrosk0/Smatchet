#!/usr/bin/env bats
# tests/bats/agent_eval_run.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/agent-eval-run.sh — the agent eval runner.
#
# Uses --fake-runner (canned result, NO live tokens) to assert:
#   * the emitted JSON conforms to docs/agent-eval/result-schema.json;
#   * the before/after seam (--prompt-root) + trial count are honoured;
#   * usage errors exit 2;
#   * runner -> scorer integration: two fake runs score clean (exit 0).
# Plan: docs/plans/shipped/subagent-eval-harness.md § Verification (Runner test).
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    RUNNER="$REPO_ROOT/scripts/dev/agent-eval-run.sh"
    SCORER="$REPO_ROOT/scripts/dev/agent-eval-score.py"
    VALIDATOR="$REPO_ROOT/tests/agent-eval/validate_schema.py"
    POLICY="$REPO_ROOT/docs/agent-eval/scoring-policy.json"
    CASE="$REPO_ROOT/tests/agent-eval/code-review/cr-dpapi-secret-loss.json"
    export RUNNER SCORER VALIDATOR POLICY CASE

    # Exec-validate the interpreter (bats-coverage-02): a bare `command -v python3`
    # matches the Windows WinStore alias that exits 49 on run. Try each candidate
    # and skip cleanly when none works.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK
}

@test "fake-runner writes a result that validates against result-schema.json" {
    out="$WORK/r.json"
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --out="$out"
    [ "$status" -eq 0 ]
    [ -f "$out" ]
    run "$PY" "$VALIDATOR" "$REPO_ROOT/docs/agent-eval/result-schema.json" "$out"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "result records caseId, fake-runner harness, and prompt-root" {
    out="$WORK/r.json"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --out="$out"
    run "$PY" -c 'import json,sys; d=json.load(open(sys.argv[1],encoding="utf-8")); print(d["caseId"], d["harness"], d["promptRoot"])' "$out"
    [ "$status" -eq 0 ]
    [[ "$output" == *"cr-dpapi-secret-loss"* ]]
    [[ "$output" == *"fake-runner"* ]]
}

@test "--trials=3 produces exactly 3 trials" {
    out="$WORK/r.json"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --trials=3 --out="$out"
    run "$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1],encoding="utf-8"))["trials"]))' "$out"
    [ "$status" -eq 0 ]
    [ "$output" -eq 3 ]
}

@test "space-form flags (--out PATH) parse identically to --out=PATH" {
    out="$WORK/space.json"
    run bash "$RUNNER" "$CASE" --prompt-root "$REPO_ROOT" --fake-runner --out "$out"
    [ "$status" -eq 0 ]
    [ -f "$out" ]
}

@test "missing prompt-root yields exit 2" {
    run bash "$RUNNER" "$CASE" --fake-runner --out="$WORK/r.json"
    [ "$status" -eq 2 ]
    [[ "$output" == *"prompt-root"* ]]
}

@test "missing case file yields exit 2" {
    run bash "$RUNNER" --prompt-root="$REPO_ROOT" --fake-runner
    [ "$status" -eq 2 ]
}

@test "unknown flag yields exit 2" {
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"unknown flag"* ]]
}

@test "non-integer trials yields exit 2" {
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --trials=abc
    [ "$status" -eq 2 ]
}

@test "runner emits the result path on the last stdout line" {
    out="$WORK/r.json"
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --out="$out"
    [ "$status" -eq 0 ]
    last="$(printf '%s\n' "$output" | tail -n 1)"
    [ "$last" = "$out" ]
}

# ---------- runner -> scorer integration (no live tokens) ----------

@test "two fake runs of the same case score clean (exit 0)" {
    base="$WORK/base.json"
    head="$WORK/head.json"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --out="$base"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --out="$head"

    # Fake judge so judge dimensions resolve deterministically.
    cat > "$WORK/judge.py" <<'JUDGE'
import json, sys
json.load(sys.stdin)
print(json.dumps({"score": 0.9}))
JUDGE
    jpath="$WORK/judge.py"
    if command -v cygpath >/dev/null 2>&1; then jpath="$(cygpath -m "$jpath")"; fi

    run "$PY" "$SCORER" "$base" "$head" --policy "$POLICY" --judge-cmd "$PY $jpath"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No regressions."* ]]
}
