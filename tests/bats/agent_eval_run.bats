#!/usr/bin/env bats
# tests/bats/agent_eval_run.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/agent-eval-run.sh — the agent eval runner.
#
# Uses --fake-runner (canned result, NO live tokens) to assert:
#   * the emitted JSON conforms to docs/agent-eval/result-schema.json;
#   * the before/after seam (--prompt-root) + trial count are honoured;
#   * usage errors exit 2;
#   * runner -> scorer integration: two fake runs score clean (exit 0);
#   * the real-mode findings parser (severity headings, citation fallback),
#     driven through a stub harness exe.
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

# ---------- real-mode findings parser (stub harness exe, no live tokens) ----------
# The claude-print adapter runs "$EXE" -p "<packet>"; a stub exe that prints a
# canned review drives the real-mode parser without a model.

# make_stub_exe <review-file> — writes $WORK/stub-claude that prints the file.
make_stub_exe() {
    local review="$1"
    STUB="$WORK/stub-claude"
    printf '#!/usr/bin/env bash\ncat %q\n' "$review" > "$STUB"
    chmod +x "$STUB"
}

# findings_of <result.json> — one "severity file line" row per trial-0 finding,
# then "parser=<runMeta.parserByTrial[0]>". Fails on a summary over 200 chars.
findings_of() {
    "$PY" - "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
for f in d["trials"][0]["findings"]:
    assert len(f["summary"]) <= 200, f["summary"]
    print(f.get("severity"), f.get("file"), f.get("line"))
print("parser=%s" % d["runMeta"]["parserByTrial"][0])
PY
}

@test "real mode: severity headings parse one finding per bullet, evidence and clean sections skipped" {
    review="$WORK/review.md"
    cat > "$review" <<'REVIEW'
Reviewed the diff against the checklist.

## Critical

## High
- Source/Core/src/secrets/Store.cpp:120 — DPAPI blob is dropped on re-save, so the stored secret is lost.
  Evidence: Source/Core/src/secrets/Store.cpp:98 reads the blob, Source/Core/src/secrets/Store.cpp:131
  writes an empty one.
  - Caller: Source/Core/src/secrets/Settings.cpp:44
  ```cpp
  // Source/Core/src/secrets/Store.cpp:131
  ## not a heading
  ```

## Medium
- None

## Low / Nits
* Source/Core/src/secrets/Store.h:12 — comment names the old function.

## Verified clean
- Ownership: Source/Core/src/secrets/Store.cpp:60 releases on every path.
- Threading: Source/Core/src/secrets/Store.cpp:75 holds the lock.
```text
## High
- Source/Core/src/fake.cpp:1 inside a fence is not a finding
```

## Outcome: applied

## Self-improvement
- Checklist could name Source/Core/src/secrets/Store.cpp:120 style re-save paths.
REVIEW
    make_stub_exe "$review"
    out="$WORK/r.json"
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --exe="$STUB" --out="$out"
    [ "$status" -eq 0 ]
    run "$PY" "$VALIDATOR" "$REPO_ROOT/docs/agent-eval/result-schema.json" "$out"
    [ "$status" -eq 0 ]
    run findings_of "$out"
    [ "$status" -eq 0 ]
    [ "$output" = "high Source/Core/src/secrets/Store.cpp 120
low Source/Core/src/secrets/Store.h 12
parser=headings" ]
}

@test "real mode: a severity heading with no bullets yields zero findings, not a citation scan" {
    review="$WORK/clean.md"
    cat > "$review" <<'REVIEW'
## High
No findings.

## Verified clean
- Source/Core/src/secrets/Store.cpp:60 releases on every path.

## Outcome: applied
REVIEW
    make_stub_exe "$review"
    out="$WORK/r.json"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --exe="$STUB" --out="$out"
    run findings_of "$out"
    [ "$status" -eq 0 ]
    [ "$output" = "parser=headings" ]
}

@test "real mode: heading-less output falls back to the per-line citation scan" {
    review="$WORK/plain.txt"
    cat > "$review" <<'REVIEW'
The secret is lost at Source/Core/src/secrets/Store.cpp:120 on re-save.
Nothing else stood out.
See also Source/Core/src/secrets/Store.h:12.
REVIEW
    make_stub_exe "$review"
    out="$WORK/r.json"
    run bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --exe="$STUB" --out="$out"
    [ "$status" -eq 0 ]
    run "$PY" "$VALIDATOR" "$REPO_ROOT/docs/agent-eval/result-schema.json" "$out"
    [ "$status" -eq 0 ]
    run findings_of "$out"
    [ "$status" -eq 0 ]
    [ "$output" = "info Source/Core/src/secrets/Store.cpp 120
info Source/Core/src/secrets/Store.h 12
parser=citations" ]
}

@test "fake-runner records the reference parser for every trial" {
    out="$WORK/r.json"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --fake-runner --trials=2 --out="$out"
    run "$PY" -c 'import json,sys; print(json.load(open(sys.argv[1],encoding="utf-8"))["runMeta"]["parserByTrial"])' "$out"
    [ "$status" -eq 0 ]
    [ "$output" = "['reference', 'reference']" ]
}
