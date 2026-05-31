#!/usr/bin/env bats
# tests/bats/function_size.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/function_size_audit.py — the function-size
# delta gate (decompose-top-20-monoliths Slice 0), wired into test-lint-rules.sh.
#
# Covers: per-rule detection via --scan-file (long / branchy / clean negatives),
# parser robustness (aggregate init + lambda not misdetected), and the --diff
# delta gate (new function fails, grandfathered function passes, SMATCHET_DEVIATION
# suppresses) via throwaway git repos.
#
# Requires: bash, bats, a working python interpreter (python3/python/py).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/function_size_audit.py"
    export FIX="$REPO_ROOT/tests/fixtures/function_size"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
}

# ---------- per-rule detection (--scan-file) ----------

@test "function-too-long fires on a >200-line function body" {
    run "$PY" "$AUD" --scan-file "$FIX/known-bad-long.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"OversizedFunction"* ]]
}

@test "function-too-branchy fires on a >30-branch function under the line cap" {
    run "$PY" "$AUD" --scan-file "$FIX/known-bad-branchy.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"function-too-branchy"* ]]
    [[ "$output" == *"BranchyFunction"* ]]
    # under 200 lines -> NOT also function-too-long
    [[ "$output" != *"function-too-long"* ]]
}

@test "known-good fixture produces no findings (aggregate init + lambda not misdetected)" {
    run "$PY" "$AUD" --scan-file "$FIX/known-good.cpp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# ---------- delta gate (--diff) via throwaway git repos ----------

# Build a minimal repo with a scoped source file; $1 = body lines in the function.
_mk_repo() {
    local dir="$1" body_lines="$2" prefix="${3:-}"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    mkdir -p "$dir/Source/Core/src/Tracker"
    {
        echo "$prefix"
        echo "int Generated(int seed) {"
        echo "    int acc = seed;"
        local i=0
        while [ "$i" -lt "$body_lines" ]; do echo "    acc += $i;"; i=$((i+1)); done
        echo "    return acc;"
        echo "}"
    } > "$dir/Source/Core/src/Tracker/Gen.cpp"
}

@test "--diff FAILS when a working-tree function newly crosses the 200-line cap" {
    repo="$BATS_TEST_TMPDIR/new"
    _mk_repo "$repo" 10               # base: small function
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    # grow the body well past the cap in the working tree (uncommitted)
    _mk_repo "$repo" 230 ""           # rewrite Gen.cpp with a 230-line body (same repo dir)
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"Generated"* ]]
}

@test "--diff PASSES (grandfathered) when an already-oversized function only grows" {
    repo="$BATS_TEST_TMPDIR/grand"
    _mk_repo "$repo" 230              # base ALREADY oversized
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 260 ""          # grows further, same name
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--diff PASSES when SMATCHET_DEVIATION suppresses a new oversized function" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo" 10
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 230 "// SMATCHET_DEVIATION(rule=function-too-long; reason=test; owner=t; revisit=never)"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
