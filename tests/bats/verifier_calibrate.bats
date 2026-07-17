#!/usr/bin/env bats
# tests/bats/verifier_calibrate.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/verifier-calibrate.py — the advisory→blocking
# calibration report for the verifier (Brier / ECE / AUC / hard-veto stats).
# Pure offline: fixed calibration sets, no model.
#
# Proves:
#   * the self-test dogfoods;
#   * a well-calibrated, discriminating set is eligible for blocking promotion;
#   * a too-small / miscalibrated set stays advisory;
#   * --gate turns "not eligible" into exit 1;
#   * --scores/--labels joins a sidecar aggregate with ground-truth labels;
#   * malformed input fails with exit 2.
# Docs: docs/agent-rules/verifier-sidecar.md.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    CAL="$REPO_ROOT/scripts/dev/verifier-calibrate.py"
    export CAL

    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK

    # A well-calibrated, discriminating set of 24 (scores hug outcomes).
    "$PY" - "$WORK/good.json" <<'PY'
import json, sys
cases = []
for i in range(24):
    y = 1 if i % 2 == 0 else 0
    s = 0.95 - 0.02 * (i % 3) if y == 1 else 0.05 + 0.02 * (i % 3)
    cases.append({"id": f"c{i}", "score": s, "outcome": y, "hard_veto": (y == 0 and i % 4 == 1)})
json.dump({"cases": cases}, open(sys.argv[1], "w"))
PY
}

@test "selftest passes (dogfood)" {
    run "$PY" "$CAL" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "well-calibrated set is eligible for blocking promotion" {
    run "$PY" "$CAL" "$WORK/good.json"
    [ "$status" -eq 0 ]
    OUTPUT="$output" "$PY" - <<'PY'
import json, os
d = json.loads(os.environ["OUTPUT"])
assert d["eligible_for_blocking"] is True, d["blockers"]
assert d["auc"] == 1.0, d
assert d["brier"] < 0.15 and d["ece"] < 0.10, d
assert d["hard_veto"]["precision"] == 1.0, d["hard_veto"]
PY
}

@test "too-small set stays advisory and --gate exits 1" {
    echo '{"cases":[{"id":"a","score":0.9,"outcome":1},{"id":"b","score":0.1,"outcome":0}]}' > "$WORK/tiny.json"
    run "$PY" "$CAL" "$WORK/tiny.json"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"stay-advisory"'* ]]
    run "$PY" "$CAL" "$WORK/tiny.json" --gate
    [ "$status" -eq 1 ]
}

@test "join --scores sidecar aggregate with --labels" {
    echo '{"candidates":[{"id":"fix-a","overall_score":0.9,"hard_veto":false},{"id":"fix-b","overall_score":0.2,"hard_veto":true}]}' > "$WORK/agg.json"
    echo '{"fix-a":1,"fix-b":0}' > "$WORK/labels.json"
    run "$PY" "$CAL" --scores "$WORK/agg.json" --labels "$WORK/labels.json"
    [ "$status" -eq 0 ]
    OUTPUT="$output" "$PY" - <<'PY'
import json, os
d = json.loads(os.environ["OUTPUT"])
assert d["samples"] == 2, d
assert d["hard_veto"]["precision"] == 1.0, d["hard_veto"]
PY
}

@test "malformed input fails with exit 2" {
    echo '{bad' > "$WORK/bad.json"
    run "$PY" "$CAL" "$WORK/bad.json"
    [ "$status" -eq 2 ]
    [[ "$output" == *"ERROR"* ]]

    echo '{"cases":[{"id":"a","score":0.5}]}' > "$WORK/nooutcome.json"
    run "$PY" "$CAL" "$WORK/nooutcome.json"
    [ "$status" -eq 2 ]
}
