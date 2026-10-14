#!/usr/bin/env bats
# tests/bats/verifier_labels.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/verifier-labels.py — joins the <trace>.meta.json
# files pre-ship.sh records beside each verifier trace with the merge-snapshot
# ledger (by headSha) + the postmortems ledger, and emits the labelled
# calibration set verifier-calibrate.py consumes
# (tooling.md `verifier-calibration-traces-never-collected`). Pure offline.
#
# Proves:
#   * the self-test dogfoods;
#   * clean merges label 1; red-check / override-label / non-pass verdict /
#     postmortem-named merges label 0 (every #N in a heading's PR segment); a
#     prose-only or title-only postmortem mention does not;
#   * a run whose head never merged is excluded and counted, never guessed;
#   * the output is a valid verifier-calibrate.py input;
#   * malformed input / a missing traces dir fail with exit 2.
#
# Requires: bash, bats, python (3.x) on PATH.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    LABELS="$REPO_ROOT/scripts/dev/verifier-labels.py"
    CAL="$REPO_ROOT/scripts/dev/verifier-calibrate.py"
    export LABELS CAL

    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"

    WORK="$BATS_TEST_TMPDIR"
    export WORK
    mkdir -p "$WORK/traces"
    : > "$WORK/ledger.jsonl"
    printf '# Postmortems\n' > "$WORK/postmortems.md"
}

# meta <head> <score> [hard_veto] — one recorded run on <head>.
meta() {
    printf '{"branch":"feat-%s","headSha":"%s","overall_score":%s,"hard_veto":%s}\n' \
        "$1" "$1" "$2" "${3:-false}" > "$WORK/traces/feat-$1-20261004T000000Z.meta.json"
}

# row <pr> <head> <gates> <redChecks-json-array> [overrideLabels-json-array]
row() {
    printf '{"pr":%s,"mergeCommit":"m%s","headSha":"%s","gates":"%s","redChecks":%s,"overrideLabels":%s}\n' \
        "$1" "$1" "$2" "$3" "$4" "${5:-[]}" >> "$WORK/ledger.jsonl"
}

labels() {
    "$PY" "$LABELS" --traces "$WORK/traces" --ledger "$WORK/ledger.jsonl" \
        --postmortems "$WORK/postmortems.md" "$@"
}

# outcome_of <pr> — the label the calibration set gives that PR's run.
outcome_of() {
    "$PY" -c 'import json,sys
for c in json.load(open(sys.argv[1]))["cases"]:
    if c["id"].endswith("(PR #%s)" % sys.argv[2]): print(c["outcome"])' "$WORK/cal.json" "$1"
}

@test "--selftest passes" {
    run "$PY" "$LABELS" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"verifier-labels --selftest: PASS"* ]]
}

@test "clean merges label 1; a bypassed red check or a non-pass verdict labels 0" {
    meta aaa 0.9; row 101 aaa GATES_PASSED '[]'
    meta bbb 0.3; row 102 bbb GATES_PASSED '["Coverage"]'
    meta ccc 0.4; row 103 ccc GATES_INCOMPLETE '[]'
    meta ddd 0.8; row 104 ddd BACKFILLED '[]'
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    [ "$(outcome_of 101)" = "1" ]
    [ "$(outcome_of 102)" = "0" ]
    [ "$(outcome_of 103)" = "0" ]
    [ "$(outcome_of 104)" = "1" ]
}

@test "a PR named in a postmortem heading labels 0; a prose-only mention does not" {
    meta aaa 0.9; row 201 aaa GATES_PASSED '[]'
    meta bbb 0.9; row 202 bbb GATES_PASSED '[]'
    meta ccc 0.9; row 203 ccc GATES_PASSED '[]'
    meta ddd 0.9; row 204 ddd GATES_PASSED '[]'
    cat >> "$WORK/postmortems.md" <<'MD'

## 2026-10-01 · PR #900, #202 · merged past a red check
Root cause discussion mentions PR #201 in passing.

## 2026-10-02 · PR #905/#203 · reverted
MD
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    [ "$(outcome_of 201)" = "1" ]
    [ "$(outcome_of 202)" = "0" ]
    [ "$(outcome_of 203)" = "0" ]
    [ "$(outcome_of 204)" = "1" ]
}

@test "a merge carrying an override label labels 0 even with clean gates and no red check" {
    meta aaa 0.9; row 211 aaa GATES_PASSED '[]' '["tests-out-of-band"]'
    meta bbb 0.9; row 212 bbb GATES_PASSED '[]' '[]'
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    [ "$(outcome_of 211)" = "0" ]
    [ "$(outcome_of 212)" = "1" ]
}

@test "every PR in a heading's PR segment labels 0 (real postmortems.md shapes); title refs do not" {
    local pr h
    for pr in 221 222 223 224 225 226 227 228 229; do
        h="h$pr"; meta "$h" 0.9; row "$pr" "$h" GATES_PASSED '[]'
    done
    cat >> "$WORK/postmortems.md" <<'MD'

## 2026-08-19 · PR #220 (+ #221, #222, #223) · merged past a permanently-pending check
## 2026-06-28 · PR #990 (introducer), #224, #225 (rode past) · red non-required check merged
## 2026-06-14 · PR #991 (+ #226 …) · red-check merged while IN_PROGRESS (same #227 class)
## 2026-06-07 · coverage.yml (since #228 graduation), fixed by PR #992 · prose-promise gate
#229 touched the file — a body line, not a heading
MD
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    for pr in 221 222 223 224 225 226; do [ "$(outcome_of "$pr")" = "0" ]; done
    for pr in 227 228 229; do [ "$(outcome_of "$pr")" = "1" ]; done
}

@test "a run whose head never merged is excluded and counted, never labelled" {
    meta aaa 0.9; row 301 aaa GATES_PASSED '[]'
    meta zzz 0.1
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    [[ "$output" == *"1 labelled run(s), 1 unmatched"* ]]
    run "$PY" -c 'import json,sys;d=json.load(open(sys.argv[1]));print(d["matched"], d["unmatched"], len(d["cases"]))' "$WORK/cal.json"
    [ "$output" = "1 1 1" ]
}

@test "the latest run on a head wins over an earlier one" {
    printf '{"branch":"f","headSha":"aaa","overall_score":0.2,"hard_veto":false}\n' > "$WORK/traces/f-20261001T000000Z.meta.json"
    printf '{"branch":"f","headSha":"aaa","overall_score":0.7,"hard_veto":true}\n' > "$WORK/traces/f-20261002T000000Z.meta.json"
    row 401 aaa GATES_PASSED '[]'
    run labels --out "$WORK/cal.json"
    [ "$status" -eq 0 ]
    run "$PY" -c 'import json,sys;c=json.load(open(sys.argv[1]))["cases"];print(len(c), c[0]["score"], c[0]["hard_veto"])' "$WORK/cal.json"
    [ "$output" = "1 0.7 True" ]
}

@test "the output is a valid verifier-calibrate.py calibration set" {
    meta aaa 0.9; row 501 aaa GATES_PASSED '[]'
    meta bbb 0.2; row 502 bbb GATES_PASSED '["Coverage"]'
    labels --out "$WORK/cal.json" 2>/dev/null
    run "$PY" "$CAL" "$WORK/cal.json"
    [ "$status" -eq 0 ]
    # Two samples are far below min_samples: --gate must refuse promotion.
    run "$PY" "$CAL" "$WORK/cal.json" --gate
    [ "$status" -eq 1 ]
}

@test "stdout carries the set when --out is not given" {
    meta aaa 0.9; row 601 aaa GATES_PASSED '[]'
    run "$PY" "$LABELS" --traces "$WORK/traces" --ledger "$WORK/ledger.jsonl" --postmortems "$WORK/postmortems.md"
    [ "$status" -eq 0 ]
    [[ "$output" == *'"outcome": 1'* ]]
}

@test "a malformed ledger or a missing traces dir fails with exit 2" {
    meta aaa 0.9
    echo 'not json' > "$WORK/ledger.jsonl"
    run labels
    [ "$status" -eq 2 ]
    [[ "$output" == *"malformed ledger line 1"* ]]
    run "$PY" "$LABELS" --traces "$WORK/nope" --ledger "$WORK/ledger.jsonl"
    [ "$status" -eq 2 ]
}
