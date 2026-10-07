#!/usr/bin/env bats
# tests/bats/perf_compare_p99.bats
# ----------------------------------------------------------------------------
# The p99 gates of scripts/dev/perf-compare.py (infra.md 2026-06-18
# perf-gate-absolute-p99-hard-ceiling-false-positives-on-job-correlated-runner-noise):
#   * RELATIVE p99 gate — fires only when a scope's p99 regresses by more than
#     p99_rel_pct AND the absolute delta exceeds p99_min_abs_delta_ms (the
#     noise floor); p99_rel_pct null = disabled.
#   * ABSOLUTE p99 ceiling — p99_abs_ceiling_ms hard-fails on the fresh number
#     alone, regardless of the baseline.
#
# Hermetic: each test writes a synthetic baseline (top-level `.rows`, the
# docs/perf/baselines shape), a fresh scenario.run snapshot (`.data.rows`), and
# an explicit policy file (docs/perf/regression-policy.json shape: `default` +
# `perScenario`), so a later re-calibration of the real policy cannot move these
# verdicts. Every row keeps lastTotalMs / maxMs / avgPerCallMs flat and well
# under budget, so only the p99 knobs under test can produce a regression.
#
# Exit contract under test: 0 = within policy, 1 = regression, 2 = bad input.
# Requires: bash, bats, python3.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    COMPARE="$REPO_ROOT/scripts/dev/perf-compare.py"
    tmp="$(mktemp -d)"
}

teardown() {
    [ -n "${tmp:-}" ] && rm -rf "$tmp"
    return 0
}

# First python that resolves AND runs (the Windows Store-alias stub resolves but
# exits non-zero, so a resolve-only probe would hand back a broken interpreter).
_resolve_py() {
    local c
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then
            printf '%s\n' "$c"; return 0
        fi
    done
    return 1
}

# write_policy <p99_rel_pct|null> — p99 ceiling 10 ms, relative floor 1.5 ms.
write_policy() {
    cat > "$tmp/policy.json" <<EOF
{"default": {"mean_delta_pct": 10.0, "p99_abs_ceiling_ms": 10.0, "max_abs_ceiling_ms": 50.0,
             "min_baseline_calls": 10, "mean_abs_ceiling_ms": 6.94, "mean_min_abs_delta_ms": 0.05,
             "p99_rel_pct": $1, "p99_min_abs_delta_ms": 1.5},
 "perScenario": {}}
EOF
}

# write_pair <baseline-p99> <fresh-p99> — one scope, only p99 differs.
write_pair() {
    cat > "$tmp/baseline.json" <<EOF
{"scenarioId": "synthetic-scroll", "captureCommit": "0000000", "captureHost": "ci", "captureDate": "2026-01-01",
 "rows": [{"name": "SmatchetUI::Draw", "calls": 600, "lastTotalMs": 0.5, "avgPerCallMs": 0.4,
           "p99Ms": $1, "maxMs": 20.0}]}
EOF
    cat > "$tmp/fresh.json" <<EOF
{"ok": true, "data": {"rows": [{"name": "SmatchetUI::Draw", "calls": 600, "lastTotalMs": 0.5,
                                "avgPerCallMs": 0.4, "p99Ms": $2, "maxMs": 20.0}]}}
EOF
}

compare() {
    PY="$(_resolve_py)" || skip "no working python interpreter"
    run "$PY" "$COMPARE" "$tmp/baseline.json" "$tmp/fresh.json" --policy "$tmp/policy.json"
}

@test "perf-compare p99: within pct AND floor of baseline does not regress" {
    write_policy 75.0
    write_pair 1.0 1.5   # +50 % (< 75 %), delta 0.5 ms (< 1.5 ms)
    compare
    [ "$status" -eq 0 ]
    [[ "$output" == *"No regressions."* ]]
}

@test "perf-compare p99: over BOTH pct and floor regresses with p99Ms regressed" {
    write_policy 75.0
    write_pair 1.0 3.0   # +200 % (> 75 %), delta 2.0 ms (> 1.5 ms), still under 10 ms
    compare
    [ "$status" -eq 1 ]
    [[ "$output" == *"SmatchetUI::Draw: p99Ms regressed +200.0%"* ]]
    [[ "$output" != *"exceeds Pillar 1 ceiling"* ]]
}

@test "perf-compare p99: pct exceeded but delta under the noise floor does not regress" {
    write_policy 75.0
    write_pair 0.2 1.0   # +400 % (> 75 %) but delta 0.8 ms (< 1.5 ms floor)
    compare
    [ "$status" -eq 0 ]
    [[ "$output" != *"p99Ms regressed"* ]]
}

@test "perf-compare p99: over the absolute ceiling fails even within the relative band" {
    write_policy 75.0
    write_pair 11.5 12.0   # +4.3 % relative (quiet) but 12.0 > 10.0 ms ceiling
    compare
    [ "$status" -eq 1 ]
    [[ "$output" == *"p99Ms 12.000 exceeds Pillar 1 ceiling 10.000"* ]]
    [[ "$output" != *"p99Ms regressed"* ]]
}

@test "perf-compare p99: p99_rel_pct null disables the relative gate" {
    write_policy null
    write_pair 1.0 3.0   # would regress under an armed relative gate
    compare
    [ "$status" -eq 0 ]
    [[ "$output" != *"p99Ms regressed"* ]]
    [[ "$output" != *"p99 Δ ≤"* ]]
}

@test "perf-compare p99: a perScenario override re-arms the relative gate for that scenario" {
    write_policy null
    PY="$(_resolve_py)" || skip "no working python interpreter"
    "$PY" - "$tmp/policy.json" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))
p["perScenario"]["synthetic-scroll"] = {"p99_rel_pct": 50.0}
json.dump(p, open(sys.argv[1], "w"))
PY
    write_pair 1.0 3.0
    compare
    [ "$status" -eq 1 ]
    [[ "$output" == *"p99Ms regressed"* ]]
}
