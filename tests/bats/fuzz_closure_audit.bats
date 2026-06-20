#!/usr/bin/env bats
# tests/bats/fuzz_closure_audit.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/fuzz_closure_audit.py — the WARN-first
# fuzz-closure include-resolution gate. Statically resolves each fuzz target's
# closure .cpp first-hop quote-includes against that target's declared INCLUDES
# (tests/fuzz/CMakeLists.txt), catching the INVISIBLE class where a closure .cpp
# include breaks ONLY the advisory nightly ninja-fuzzer-linux build.
#
# Covers: --selftest (synthetic bad/clean targets); --check passes against the
# real tree (WARN-first → exit 0); --strict still exits 0 when the real tree is
# clean; --list parses every real target.
#
# There is no bats on the Windows CI runner, so these run at the pre-push gate
# (match include_cycle_audit.bats / merge_gates.bats structure).
#
# Requires: bash, bats, a working python interpreter (python3/python/py).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/fuzz_closure_audit.py"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
    [ -f "$AUD" ] || skip "audit script missing: $AUD"
}

@test "fuzz_closure_audit: --selftest passes (flags bad closure include, clean target clean)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"--selftest: PASS"* ]]
}

@test "fuzz_closure_audit: --check passes on the real tree (WARN-first -> exit 0)" {
    cd "$REPO_ROOT" || return 1
    run env CLAUDE_PROJECT_DIR="$REPO_ROOT" "$PY" "$AUD" --check
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "fuzz_closure_audit: --strict on the (clean) real tree exits 0" {
    cd "$REPO_ROOT" || return 1
    run env CLAUDE_PROJECT_DIR="$REPO_ROOT" "$PY" "$AUD" --strict
    [ "$status" -eq 0 ]
}

@test "fuzz_closure_audit: --list parses every real fuzz target" {
    cd "$REPO_ROOT" || return 1
    run env CLAUDE_PROJECT_DIR="$REPO_ROOT" "$PY" "$AUD" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"fuzz_image_dims"* ]]
    [[ "$output" == *"fuzz_ai_ndjson"* ]]
}

@test "fuzz_closure_audit: unknown flag is an infra error (exit 2)" {
    run "$PY" "$AUD" --bogus
    [ "$status" -eq 2 ]
}
