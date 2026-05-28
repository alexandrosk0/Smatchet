#!/usr/bin/env bats
# tests/bats/lint_rules.bats
# ----------------------------------------------------------------------------
# Bats coverage for scripts/dev/test-lint-rules.sh (high-integrity C++ gate).
# Plan: docs/design/high-integrity-cpp-enforcement.md.
#
# Covers: each grep rule via --scan-file, SMATCHET_DEVIATION suppression +
# overdue detection, --selftest (AGENTS.md zone sync), --catalog format +
# determinism, and the --diff delta gate via a stubbed baseline triple-set.
#
# Requires: bash, bats. (narrowing-conversions is opt-in / clang-only — not
# exercised here; it has no deterministic fixture without a clang compile db.)
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export LINT="$REPO_ROOT/scripts/dev/test-lint-rules.sh"
    export FIX="$REPO_ROOT/tests/fixtures/lint_rules"
}

# ---------- per-rule detection (--scan-file) ----------

@test "no-printf-stderr fires on unexempted std::printf" {
    run bash "$LINT" --scan-file "$FIX/known-bad-printf.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"no-printf-stderr"* ]]
}

@test "no-raw-new fires on raw new Type" {
    run bash "$LINT" --scan-file "$FIX/known-bad-raw-new.cpp"
    [[ "$output" == *"no-raw-new"* ]]
}

@test "define-imgui fires on #define ImGui macro-alias" {
    run bash "$LINT" --scan-file "$FIX/known-bad-define-imgui.cpp"
    [[ "$output" == *"define-imgui"* ]]
}

# ---------- SMATCHET_DEVIATION ----------

@test "deviation-overdue fires when calendar revisit has passed" {
    run bash "$LINT" --scan-file "$FIX/deviation-overdue.cpp"
    [[ "$output" == *"deviation-overdue"* ]]
}

@test "deviation suppresses its rule on the next line + overdue not flagged when current" {
    run bash "$LINT" --scan-file "$FIX/deviation-current.cpp"
    # rule=no-raw-new suppresses the new Thing(); revisit=2099 is not overdue.
    [[ "$output" != *"no-raw-new"* ]]
    [[ "$output" != *"deviation-overdue"* ]]
}

@test "deviation-overdue fixture suppresses no-raw-new (only overdue fires)" {
    run bash "$LINT" --scan-file "$FIX/deviation-overdue.cpp"
    [[ "$output" != *"no-raw-new"* ]]
}

# ---------- known-good ----------

@test "known-good fixture produces no findings" {
    run bash "$LINT" --scan-file "$FIX/known-good.cpp"
    [ -z "$output" ]
}

# ---------- selftest: AGENTS.md zone-glob sync ----------

@test "--selftest passes (scanner zone globs match AGENTS.md)" {
    run bash "$LINT" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"in sync"* ]]
}

# ---------- catalog: format + determinism ----------

@test "--catalog emits the rule-id sections + Totals" {
    run bash "$LINT" --catalog
    [ "$status" -eq 0 ]
    [[ "$output" == *"strict zone × no-printf-stderr"* ]]
    [[ "$output" == *"strict zone × deviation-overdue"* ]]
    [[ "$output" == *"## Totals"* ]]
}

@test "--catalog is byte-deterministic across runs (no timestamp drift)" {
    run bash -c "bash '$LINT' --catalog > /tmp/lr_c1 2>/dev/null; bash '$LINT' --catalog > /tmp/lr_c2 2>/dev/null; diff -q /tmp/lr_c1 /tmp/lr_c2"
    [ "$status" -eq 0 ]
}

# ---------- diff delta gate via stubbed baseline ----------

@test "--diff PASSes when HEAD triples are a subset of the baseline" {
    # Stub a baseline that already contains every current strict-zone triple.
    head="$(bash "$LINT" --full 2>/dev/null | sed -n 's/^  //p')"
    printf '%s\n' "$head" > /tmp/lr_base_all
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_all run bash "$LINT" --diff
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "--diff FAILs when HEAD has a triple absent from the baseline" {
    # Empty baseline -> every current strict-zone violation is 'new'.
    : > /tmp/lr_base_empty
    SMATCHET_LINT_BASELINE_SET=/tmp/lr_base_empty run bash "$LINT" --diff
    [ "$status" -eq 1 ]
    [[ "$output" == *"new strict-zone"* ]]
}
