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

# parse_review — runs the review text on stdin through the real-mode runner
# against $CASE and prints findings_of for the result.
parse_review() {
    local review="$WORK/review-stdin.md"
    cat > "$review"
    make_stub_exe "$review"
    bash "$RUNNER" "$CASE" --prompt-root="$REPO_ROOT" --exe="$STUB" --out="$WORK/parsed.json" >/dev/null
    findings_of "$WORK/parsed.json"
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

# Each output shape below is tested in both directions: a review whose
# findings must all count, and one whose bullets must all be dropped.

@test "real mode: empty-section markers under severity headings yield zero findings" {
    run parse_review <<'REVIEW'
## Critical
- No critical findings.

## High
No findings.

## Medium
- None: the diff is clean.
- None
  The shift loop stays in bounds.
- None — the loop at Source/Core/include/SmatchetUI.h:410 is bounds-safe.
- No findings (checked Source/Core/include/SmatchetUI.h:415).

## Low / Nits
- Nothing to report.
- _n/a_
- No findings in this category.
- No issues identified.
- None identified.
- None at this severity.
- No high-severity findings.
- No critical or high findings.
- Nothing to flag.
- No new findings.
- None that meet the bar.
- **None**

## Verified clean
- Source/Core/src/secrets/Store.cpp:60 releases on every path.

## Outcome: applied
REVIEW
    [ "$status" -eq 0 ]
    [ "$output" = "parser=headings" ]
}

@test "real mode: cite-free findings that start like an empty-section marker still count" {
    run parse_review <<'REVIEW'
## High
- None of the three callers holds mLock before calling Flush().
- Nothing releases the DPAPI handle on the early-return path.
- No bounds check on idx before the shift loop.
- No other caller checks the return value of Protect().
- Nothing in the destructor frees the buffer.
REVIEW
    [ "$status" -eq 0 ]
    [ "$output" = "high None None
high None None
high None None
high None None
high None None
parser=headings" ]
}

@test "real mode: decorated and indented headings still open and close sections" {
    run parse_review <<'REVIEW'
## **High** (1)
- Source/Core/src/secrets/Store.cpp:120 — secret lost on re-save.
 ## Verified clean
- Source/Core/src/secrets/Store.cpp:60 releases on every path.
## Low / Nits — 1
- Source/Core/src/secrets/Store.h:12 — stale comment.
REVIEW
    [ "$status" -eq 0 ]
    [ "$output" = "high Source/Core/src/secrets/Store.cpp 120
low Source/Core/src/secrets/Store.h 12
parser=headings" ]
}

@test "real mode: a # line at a bullet's content column is wrapped text, not a heading" {
    run parse_review <<'REVIEW'
## High
- a.cpp:1 — real finding
  # shell prompt line inside a wrapped bullet
- b.cpp:2 — second
## Verified clean
- c.cpp:3
REVIEW
    [ "$status" -eq 0 ]
    [ "$output" = "high a.cpp 1
high b.cpp 2
parser=headings" ]
}

@test "real mode: no severity heading scans citations outside the evidence sections" {
    run parse_review <<'REVIEW'
## Findings
- [P1] Plaintext PAT erased before encrypt succeeds — Source_Core/src/ConfigManager.cpp:374

Finding 2 (LOW) — Source_Core/src/ConfigManager.cpp:390

### 3. Source_Core/src/ConfigManager.cpp:402 — same pattern for plane_api_key
If ProtectSecretForConfig fails, the only copy of the key is lost.

## Verified clean
- Source_Core/src/ConfigManager.cpp:380 mcp_auth_token guard is correct.

## Outcome: applied

## Self-improvement
- Checklist could name Source_Core/src/ConfigManager.cpp:374 style erase-before-encrypt paths.
REVIEW
    [ "$status" -eq 0 ]
    [ "$output" = "info Source_Core/src/ConfigManager.cpp 374
info Source_Core/src/ConfigManager.cpp 390
info Source_Core/src/ConfigManager.cpp 402
parser=citations" ]
}

@test "real mode: a clean review with no severity heading scores finding_count 1.0 on a 0-finding case" {
    # The contract's clean shape: "no findings" + ## Verified clean + ## Outcome.
    # No severity heading, so the citation scan runs, but it must skip the
    # Verified-clean evidence citations.
    clean_case="$REPO_ROOT/tests/agent-eval/code-review/cr-recentlru-false-positive.json"
    review="$WORK/clean-noseverity.md"
    cat > "$review" <<'REVIEW'
No findings: the shift loop is bounds-safe.

## Verified clean
- Bounds: Source/Core/include/SmatchetUI.h:410 stops at size() - 1.
- Ordering: Source/Core/include/SmatchetUI.h:415 moves the hit to the front.

## Outcome: applied
REVIEW
    make_stub_exe "$review"
    base="$WORK/base.json"
    head="$WORK/head.json"
    bash "$RUNNER" "$clean_case" --prompt-root="$REPO_ROOT" --fake-runner --out="$base"
    bash "$RUNNER" "$clean_case" --prompt-root="$REPO_ROOT" --exe="$STUB" --out="$head"
    run findings_of "$head"
    [ "$status" -eq 0 ]
    [ "$output" = "parser=citations" ]

    cat > "$WORK/judge.py" <<'JUDGE'
import json, sys
json.load(sys.stdin)
print(json.dumps({"score": 0.9}))
JUDGE
    jpath="$WORK/judge.py"
    if command -v cygpath >/dev/null 2>&1; then jpath="$(cygpath -m "$jpath")"; fi
    run "$PY" "$SCORER" "$base" "$head" --policy "$POLICY" --judge-cmd "$PY $jpath"
    [ "$status" -eq 0 ]
    [[ "$output" == *"| \`finding_count\` | objective | 1.000 | 1.000 |"* ]]
    [[ "$output" == *"No regressions."* ]]
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
