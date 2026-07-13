#!/usr/bin/env bats
# tests/bats/agent_eval_score.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/agent-eval-score.py — the agent eval scorer.
#
# Proves the SCORING CONTRACT with NO live agent and NO live judge (plan
# docs/plans/shipped/subagent-eval-harness.md § Approach step 3):
#   * a FAKE judge command (deterministic python script) replaces the LLM, so
#     judge-dimension scores are fixed and reproducible;
#   * fixed base/head result fixtures drive the objective checks.
# Asserts the exit-code contract (0 clean / 1 regression / 2 malformed) and the
# delta-table shape, plus schema conformance of cases + a sample result.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCORER="$REPO_ROOT/scripts/dev/agent-eval-score.py"
    POLICY="$REPO_ROOT/docs/agent-eval/scoring-policy.json"
    VALIDATOR="$REPO_ROOT/tests/agent-eval/validate_schema.py"
    export SCORER POLICY VALIDATOR

    # Pick a WORKING python 3 interpreter, exec-validating each candidate
    # (bats-coverage-02): a bare `command -v python3` matches the Windows WinStore
    # alias that exits 49 on run. Skip cleanly when none works.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"

    # -- fake judge -----------------------------------------------------------
    # Reads {dimension,output,...} on stdin, emits {"score":N}. Deterministic:
    # a canned per-dimension base score, knocked down by 0.4 when the agent
    # output carries the "DEGRADED" marker — that is how a head fixture drives a
    # JUDGE-dimension regression without any real model.
    cat > "$WORK/fake_judge.py" <<'JUDGE'
import json, sys
p = json.load(sys.stdin)
dim = p.get("dimension", "")
out = p.get("output", "")
base = {"correctness": 0.90, "actionability": 0.75}
score = base.get(dim, 0.50)
if "DEGRADED" in out:
    score = max(0.0, score - 0.40)
print(json.dumps({"score": score}))
JUDGE

    # Build a judge command the scorer's subprocess can launch. On Windows the
    # interpreter is native python, which needs a C:/... path not an MSYS
    # /c/... path — convert with cygpath -m when present.
    local jpath="$WORK/fake_judge.py"
    if command -v cygpath >/dev/null 2>&1; then jpath="$(cygpath -m "$jpath")"; fi
    JUDGE_CMD="$PY $jpath"
    export JUDGE_CMD

    # -- result fixtures ------------------------------------------------------
    # BASE: 2 well-formed findings (both file:line cited), expected count 2.
    cat > "$WORK/base.json" <<'EOF'
{
  "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
  "promptRoot": "/base", "harness": "fake-runner",
  "case": {
    "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
    "repoRef": "deadbeef", "baseBranch": "develop", "toolPosture": "read-only",
    "delegationPacket": "review the diff",
    "dimensions": [
      {"key": "correctness", "kind": "judge"},
      {"key": "cited_file_line", "kind": "objective", "check": "cited_file_line"},
      {"key": "severity_enum", "kind": "objective", "check": "severity_enum"},
      {"key": "finding_count", "kind": "objective", "check": "finding_count"}
    ],
    "referenceOutcome": {"expectedFindingCount": 2, "allowedSeverities": ["high","medium","low"]}
  },
  "trials": [
    {"index": 0, "output": "Two solid findings.", "findings": [
      {"file": "a.cpp", "line": 10, "severity": "high", "summary": "leak"},
      {"file": "b.cpp", "line": 20, "severity": "low", "summary": "typo"}
    ]}
  ]
}
EOF

    # HEAD_REGRESS: one finding lost its line (cited_file_line drops),
    # only one finding survives (finding_count drops), and the output carries
    # the DEGRADED marker (correctness judge score drops).
    cat > "$WORK/head_regress.json" <<'EOF'
{
  "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
  "promptRoot": "/head", "harness": "fake-runner",
  "case": {
    "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
    "repoRef": "deadbeef", "baseBranch": "develop", "toolPosture": "read-only",
    "delegationPacket": "review the diff",
    "dimensions": [
      {"key": "correctness", "kind": "judge"},
      {"key": "cited_file_line", "kind": "objective", "check": "cited_file_line"},
      {"key": "severity_enum", "kind": "objective", "check": "severity_enum"},
      {"key": "finding_count", "kind": "objective", "check": "finding_count"}
    ],
    "referenceOutcome": {"expectedFindingCount": 2, "allowedSeverities": ["high","medium","low"]}
  },
  "trials": [
    {"index": 0, "output": "DEGRADED — only one weak finding.", "findings": [
      {"file": "a.cpp", "line": null, "severity": "high", "summary": "leak"}
    ]}
  ]
}
EOF

    # MALFORMED: missing the required trials array.
    cat > "$WORK/no_trials.json" <<'EOF'
{ "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
  "promptRoot": "/x", "harness": "fake-runner" }
EOF

    # MALFORMED: not even JSON.
    printf 'this is not json {' > "$WORK/broken.json"

    # MISMATCH: same shape as base but a different caseId.
    "$PY" - "$WORK/base.json" "$WORK/other_case.json" <<'EOF'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
d["caseId"] = "demo-OTHER"
json.dump(d, open(sys.argv[2], "w", encoding="utf-8"))
EOF
    export WORK
}

score() { "$PY" "$SCORER" "$@"; }

# ---------- exit-code contract ----------

@test "clean run base-vs-base yields exit 0 and no regressions" {
    run score "$WORK/base.json" "$WORK/base.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No regressions."* ]]
}

@test "objective plus judge regression yields exit 1" {
    run score "$WORK/base.json" "$WORK/head_regress.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"REGRESSION"* ]]
    [[ "$output" == *"Regressions:"* ]]
}

@test "cited_file_line objective check drives a regression row" {
    run score "$WORK/base.json" "$WORK/head_regress.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"cited_file_line"* ]]
    [[ "$output" == *"1.000"* ]]   # base cited_file_line score
    [[ "$output" == *"0.000"* ]]   # head cited_file_line score
}

@test "judge dimension regression is surfaced (correctness drops via DEGRADED marker)" {
    run score "$WORK/base.json" "$WORK/head_regress.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"correctness (judge): regressed"* ]]
}

@test "malformed result missing trials yields exit 2" {
    run score "$WORK/no_trials.json" "$WORK/no_trials.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 2 ]
    [[ "$output" == *"trials"* ]]
}

@test "malformed result not JSON yields exit 2" {
    run score "$WORK/broken.json" "$WORK/base.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 2 ]
    [[ "$output" == *"malformed JSON"* ]]
}

@test "caseId mismatch base vs head yields exit 2" {
    run score "$WORK/base.json" "$WORK/other_case.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 2 ]
    [[ "$output" == *"caseId mismatch"* ]]
}

@test "judge dimension present but no judge-cmd yields exit 2" {
    run env -u SMATCHET_AGENT_EVAL_JUDGE_CMD "$PY" "$SCORER" "$WORK/base.json" "$WORK/base.json" --policy "$POLICY"
    [ "$status" -eq 2 ]
    [[ "$output" == *"judge"* ]]
}

@test "markdown-only downgrades a regression to advisory exit 0" {
    run score "$WORK/base.json" "$WORK/head_regress.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD" --markdown-only
    [ "$status" -eq 0 ]
    [[ "$output" == *"REGRESSION"* ]]
}

@test "judge-cmd env fallback works without the flag" {
    SMATCHET_AGENT_EVAL_JUDGE_CMD="$JUDGE_CMD" run score "$WORK/base.json" "$WORK/base.json" --policy "$POLICY"
    [ "$status" -eq 0 ]
    [[ "$output" == *"No regressions."* ]]
}

# ---------- delta-table shape ----------

@test "delta table has a header row and one row per dimension" {
    run score "$WORK/base.json" "$WORK/base.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"| dimension | kind | base | head | Δ | verdict |"* ]]
    [[ "$output" == *"| \`correctness\` | judge |"* ]]
    [[ "$output" == *"| \`cited_file_line\` | objective |"* ]]
    [[ "$output" == *"| \`severity_enum\` | objective |"* ]]
    [[ "$output" == *"| \`finding_count\` | objective |"* ]]
}

# ---------- schema conformance (stdlib validator, no third-party dep) ----------

@test "sample result validates against result-schema.json" {
    run "$PY" "$VALIDATOR" "$REPO_ROOT/docs/agent-eval/result-schema.json" "$WORK/base.json"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "all curated code-review cases validate against case-schema.json" {
    local schema="$REPO_ROOT/docs/agent-eval/case-schema.json"
    local n=0
    for c in "$REPO_ROOT"/tests/agent-eval/code-review/*.json; do
        [ -e "$c" ] || continue
        n=$((n + 1))
        run "$PY" "$VALIDATOR" "$schema" "$c"
        [ "$status" -eq 0 ] || { echo "FAILED: $c -> $output"; return 1; }
    done
    [ "$n" -ge 3 ]   # plan mandates 3-5 curated cases
}

@test "validator rejects a result missing required fields (negative control)" {
    run "$PY" "$VALIDATOR" "$REPO_ROOT/docs/agent-eval/result-schema.json" "$WORK/no_trials.json"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SCHEMA VIOLATION"* ]]
}
