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

# ---------- tiered line cap: UI/non-UI classification ----------

# Build a single-file fixture with a function of $2 body lines; $3 = function name, $4 = subdir
# under Source/Core/src (Tracker = non-UI, Ui = UI). Emitted to $1 (a .cpp path).
_mk_file() {
    local out="$1" body_lines="$2" name="$3"
    {
        echo "int ${name}(int seed) {"
        echo "    int acc = seed;"
        local i=0
        while [ "$i" -lt "$body_lines" ]; do echo "    acc += $i;"; i=$((i+1)); done
        echo "    return acc;"
        echo "}"
    } > "$out"
}

@test "tiered: a 130-line NON-UI function exceeds the 120 cap (function-too-long)" {
    f="$BATS_TEST_TMPDIR/Compute.cpp"
    _mk_file "$f" 126 "ComputeTotals"     # ~130 body lines incl signature/braces
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"ComputeTotals"* ]]
}

@test "tiered: a 130-line ImGui-draw function (Draw-named) PASSES the 200 cap" {
    f="$BATS_TEST_TMPDIR/DrawPanel.cpp"
    _mk_file "$f" 126 "DrawAssistantPanel"   # Draw-prefixed -> 200 cap, 130 < 200
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"function-too-long"* ]]
}

@test "tiered: a 130-line function under a Ui/ path PASSES the 200 cap" {
    mkdir -p "$BATS_TEST_TMPDIR/Source/Core/src/Ui"
    f="$BATS_TEST_TMPDIR/Source/Core/src/Ui/Widget.cpp"
    _mk_file "$f" 126 "HelperThing"          # non-Draw name but Ui/ path -> 200 cap
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    [[ "$output" != *"function-too-long"* ]]
}

# ---------- soft warning tier (advisory; stderr; exit 0) ----------

@test "soft tier: a 105-line / 22-branch function WARNS but exits 0 with no hard violation" {
    f="$BATS_TEST_TMPDIR/Warnable.cpp"
    {
        echo "int Warnable(int x) {"
        echo "    int acc = 0;"
        # 22 branches (> 20 soft, <= 30 hard) and ~105 lines (> 100 soft, < 120 hard non-UI).
        i=0; while [ "$i" -lt 22 ]; do echo "    if (x == $i) acc += $i;"; i=$((i+1)); done
        # pad to push line count past 100 but under 120
        i=0; while [ "$i" -lt 78 ]; do echo "    acc += $i;"; i=$((i+1)); done
        echo "    return acc;"
        echo "}"
    } > "$f"
    run "$PY" "$AUD" --scan-file "$f"
    [ "$status" -eq 0 ]
    # no hard violation on stdout
    [[ "$output" != *"function-too-long"* ]]
    [[ "$output" != *"function-too-branchy"* ]]
    # advisory warning is emitted (combined stdout+stderr via `run`)
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"Warnable"* ]]
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

@test "oversized operator overload is detected (not skipped on empty name)" {
    run "$PY" "$AUD" --scan-file "$FIX/known-bad-operator.cpp"
    [ "$status" -eq 0 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"operator()"* ]]
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

@test "tiered: --diff FAILS when a NEW non-UI function newly crosses the 120-line cap" {
    repo="$BATS_TEST_TMPDIR/new120"
    _mk_repo "$repo" 10               # base: small function (well under 120)
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 150 ""           # grow to ~154 lines (> 120 non-UI cap, < 200)
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"Generated"* ]]
}

@test "tiered: --diff PASSES (grandfathered) for an existing 150-line non-UI function under the new 120 cap" {
    repo="$BATS_TEST_TMPDIR/grand120"
    _mk_repo "$repo" 150              # base ALREADY over the new 120 cap (~154 lines)
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 160 ""          # grows slightly further, same name -> still grandfathered
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
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

# Build a repo whose function trips BOTH caps (>200 lines AND >30 branches); $3 = deviation prefix.
_mk_repo_dualcap() {
    local dir="$1" prefix="${2:-}"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    mkdir -p "$dir/Source/Core/src/Tracker"
    {
        echo "$prefix"
        echo "int Generated(int x) {"
        echo "    int acc = 0;"
        local i=0
        while [ "$i" -lt 230 ]; do echo "    if (x == $i) acc += $i;"; i=$((i+1)); done
        echo "    return acc;"
        echo "}"
    } > "$dir/Source/Core/src/Tracker/Gen.cpp"
}

@test "a dual-cap function fires BOTH function-too-long and function-too-branchy" {
    repo="$BATS_TEST_TMPDIR/dual"
    _mk_repo "$repo" 10
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo_dualcap "$repo" ""
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"function-too-long"* ]]
    [[ "$output" == *"function-too-branchy"* ]]
}

@test "a comma-separated SMATCHET_DEVIATION suppresses BOTH caps at once" {
    repo="$BATS_TEST_TMPDIR/dual2"
    _mk_repo "$repo" 10
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo_dualcap "$repo" "// SMATCHET_DEVIATION(rule=function-too-long,function-too-branchy; reason=test; owner=t; revisit=never)"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}
