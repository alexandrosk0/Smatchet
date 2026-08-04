#!/usr/bin/env bats
# tests/bats/agent_eval_calibrate.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/agent-eval-calibrate.py — the judge-vs-human
# calibration GATE (PR-23 / subagent-eval-calibration backlog).
#
# Proves the CALIBRATION CONTRACT with NO live model (mirrors agent_eval_score.bats):
#   * a FAKE judge command (deterministic python) replaces the LLM, so judge
#     scores are fixed + reproducible;
#   * fixed labels + recorded-result fixtures drive the divergence math.
# Asserts the exit-code contract (0 calibrated / 1 BLOCK / 2 malformed), the
# both-gates logic (mean + single divergence), --markdown-only advisory downgrade,
# and that the COMMITTED real fixtures calibrate against a faithful judge.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    CAL="$REPO_ROOT/scripts/dev/agent-eval-calibrate.py"
    POLICY="$REPO_ROOT/docs/agent-eval/calibration-policy.json"
    export CAL POLICY

    # Exec-validate the interpreter: a bare `command -v python3` matches the
    # Windows WinStore alias stub, which resolves on PATH but exits non-zero on
    # run — every `"$PY" …` invocation then fails with a Store-install banner.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"

    # -- fake judge -----------------------------------------------------------
    # Emits a canned per-dimension score; knocked down 0.5 when the recorded
    # output carries "OFFBASE" — drives an over-threshold (BLOCK) divergence.
    cat > "$WORK/fake_judge.py" <<'JUDGE'
import json, sys
p = json.load(sys.stdin)
base = {"correctness": 0.92}
s = base.get(p.get("dimension", ""), 0.5)
if "OFFBASE" in p.get("output", ""):
    s = max(0.0, s - 0.5)
print(json.dumps({"score": s}))
JUDGE
    local jpath="$WORK/fake_judge.py"
    if command -v cygpath >/dev/null 2>&1; then jpath="$(cygpath -m "$jpath")"; fi
    JUDGE_CMD="$PY $jpath"
    export JUDGE_CMD

    # -- recorded-result fixtures (the judge INPUT) ---------------------------
    cat > "$WORK/aligned_result.json" <<'EOF'
{ "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
  "promptRoot": ".", "harness": "fake-runner",
  "case": {"caseId": "demo-1", "referenceOutcome": {}},
  "trials": [{"index": 0, "output": "a clean strong run", "findings": []}] }
EOF

    cat > "$WORK/offbase_result.json" <<'EOF'
{ "schemaVersion": 1, "caseId": "demo-1", "agent": "code-review",
  "promptRoot": ".", "harness": "fake-runner",
  "case": {"caseId": "demo-1", "referenceOutcome": {}},
  "trials": [{"index": 0, "output": "OFFBASE weak run", "findings": []}] }
EOF

    # -- labels fixtures ------------------------------------------------------
    # ALIGNED: judge 0.92 vs human 0.95 -> divergence 0.03, within policy.
    cat > "$WORK/aligned_labels.json" <<EOF
{ "schemaVersion": 1, "agent": "code-review", "entries": [
  { "label": "aligned", "result": "$WORK/aligned_result.json",
    "trialIndex": 0, "human": {"correctness": 0.95} } ] }
EOF

    # OFFBASE: judge 0.42 vs human 0.95 -> divergence 0.53 > both thresholds.
    cat > "$WORK/offbase_labels.json" <<EOF
{ "schemaVersion": 1, "agent": "code-review", "entries": [
  { "label": "offbase", "result": "$WORK/offbase_result.json",
    "trialIndex": 0, "human": {"correctness": 0.95} } ] }
EOF

    # EMPTY: no entries.
    cat > "$WORK/empty_labels.json" <<'EOF'
{ "schemaVersion": 1, "agent": "code-review", "entries": [] }
EOF

    # BAD-HUMAN: human score out of [0,1].
    cat > "$WORK/bad_human_labels.json" <<EOF
{ "schemaVersion": 1, "agent": "code-review", "entries": [
  { "label": "bad", "result": "$WORK/aligned_result.json",
    "trialIndex": 0, "human": {"correctness": 1.7} } ] }
EOF

    export WORK
}

cal() { "$PY" "$CAL" "$@"; }

# ---------- exit-code contract ----------

@test "aligned judge/human yields exit 0 and reports calibrated" {
    run cal "$WORK/aligned_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Judge calibrated within policy."* ]]
}

@test "over-threshold divergence BLOCKS with exit 1" {
    run cal "$WORK/offbase_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"BLOCK: judge not calibrated"* ]]
}

@test "single-divergence violation names the entry/dimension" {
    run cal "$WORK/offbase_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"offbase / correctness"* ]]
    [[ "$output" == *"exceeds max_single"* ]]
}

@test "mean-divergence gate also fires on the over-threshold corpus" {
    run cal "$WORK/offbase_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 1 ]
    [[ "$output" == *"mean divergence"* ]]
    [[ "$output" == *"exceeds max_mean"* ]]
}

@test "empty corpus yields exit 2" {
    run cal "$WORK/empty_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 2 ]
    [[ "$output" == *"entries"* ]]
}

@test "human score out of range yields exit 2" {
    run cal "$WORK/bad_human_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 2 ]
    [[ "$output" == *"not in [0,1]"* ]]
}

@test "missing judge-cmd yields exit 2" {
    run env -u SMATCHET_AGENT_EVAL_JUDGE_CMD "$PY" "$CAL" "$WORK/aligned_labels.json" --policy "$POLICY"
    [ "$status" -eq 2 ]
    [[ "$output" == *"judge"* ]]
}

@test "judge-cmd env fallback works without the flag" {
    SMATCHET_AGENT_EVAL_JUDGE_CMD="$JUDGE_CMD" run cal "$WORK/aligned_labels.json" --policy "$POLICY"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Judge calibrated within policy."* ]]
}

@test "markdown-only downgrades a BLOCK to advisory exit 0" {
    run cal "$WORK/offbase_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD" --markdown-only
    [ "$status" -eq 0 ]
    [[ "$output" == *"BLOCK: judge not calibrated"* ]]
}

# ---------- report shape ----------

@test "report has a header row and one row per pair" {
    run cal "$WORK/aligned_labels.json" --policy "$POLICY" --judge-cmd "$JUDGE_CMD"
    [ "$status" -eq 0 ]
    [[ "$output" == *"| entry | dimension | human | judge |"* ]]
    [[ "$output" == *"| aligned | \`correctness\` |"* ]]
}

# ---------- selftest is self-checking ----------

@test "the script --selftest passes" {
    run "$PY" "$CAL" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"agent-eval-calibrate --selftest: PASS"* ]]
}

# ---------- committed real fixtures calibrate against a faithful judge --------

@test "committed live-smoke + labels calibrate with a faithful judge" {
    # A judge that reads the same signal the human did (cites file:line +
    # contrasts the mcp_auth_token guard) lands near the human label.
    cat > "$WORK/faithful_judge.py" <<'JUDGE'
import json, sys
p = json.load(sys.stdin)
out = p.get("output", "")
s = 0.5
if "ConfigManager.cpp:374" in out: s += 0.25
if "mcp_auth_token" in out: s += 0.18
print(json.dumps({"score": min(1.0, s)}))
JUDGE
    local fj="$WORK/faithful_judge.py"
    if command -v cygpath >/dev/null 2>&1; then fj="$(cygpath -m "$fj")"; fi
    run cal "$REPO_ROOT/docs/agent-eval/calibration/code-review/labels.json" \
        --policy "$POLICY" --judge-cmd "$PY $fj"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Judge calibrated within policy."* ]]
}

@test "committed live-smoke result validates against result-schema.json" {
    run "$PY" "$REPO_ROOT/tests/agent-eval/validate_schema.py" \
        "$REPO_ROOT/docs/agent-eval/result-schema.json" \
        "$REPO_ROOT/docs/agent-eval/calibration/code-review/cr-dpapi-secret-loss.smoke.json"
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}
