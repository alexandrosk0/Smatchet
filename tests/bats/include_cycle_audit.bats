#!/usr/bin/env bats
# tests/bats/include_cycle_audit.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/include_cycle_audit.py — the
# core-include-dag Phase 0 acyclicity + layer-DAG delta gate. FAILS CLOSED:
# --diff exits 1 on a NEW SCC>1 include cycle or a NEW low->high named-layer
# header back-edge (unlike the WARN-first dup gate).
#
# Covers: --selftest; the --diff delta gate via throwaway git repos —
#   * a NEW header->header back-edge (low->high named layer) FAILs (exit 1);
#   * a NEW include cycle (SCC>1) FAILs (exit 1);
#   * a clean high->low DAG PASSes (exit 0);
#   * a .cpp pulling in a higher-layer header is NOT a violation (graph sink);
#   * an edge already present at base is grandfathered (PASS — the ratchet);
#   * a SMATCHET_DEVIATION(rule=include-cycle) above the #include suppresses.
#
# There is no bats on the Windows CI runner, so these run at the pre-push gate
# (match merge_gates.bats / lint_rules.bats structure).
#
# Requires: bash, bats, a working python interpreter (python3/python/py), git.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/include_cycle_audit.py"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
}

# Init a repo with a clean high->low header DAG + a Config leaf, all in scope under
# Source/Core/include. Ui(6) -> Config(1) is a legal forward (high->low) edge.
_mk_repo() {
    local dir="$1"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    mkdir -p "$dir/Source/Core/include/Ui" "$dir/Source/Core/include/Config" \
             "$dir/Source/Core/include/Commands"
    printf '#include "Config/Low.h"\n#include <string>\n' > "$dir/Source/Core/include/Ui/Top.h"
    printf '#include <string>\n' > "$dir/Source/Core/include/Config/Low.h"
    printf '#include <functional>\n' > "$dir/Source/Core/include/AppController.h"
    git -C "$dir" add -A && git -C "$dir" commit -qm base
}

# ---------- selftest ----------

@test "selftest passes (SCC + layer-rank + back-edge invariants in sync)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"invariants hold"* ]]
}

# ---------- delta gate (--diff) via throwaway git repos ----------

@test "--diff FAILS (exit 1) on a NEW low->high named-layer header back-edge" {
    repo="$BATS_TEST_TMPDIR/backedge"
    _mk_repo "$repo"
    # Config(1) header now reaches UP to AppController(5) — a new low->high back-edge.
    printf '#include "AppController.h"\n#include <string>\n' > "$repo/Source/Core/include/Config/Low.h"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"back-edge"* ]]
    [[ "$output" == *"Config/Low.h"* ]]
}

@test "--diff FAILS (exit 1) on a NEW include cycle (SCC>1)" {
    repo="$BATS_TEST_TMPDIR/cycle"
    _mk_repo "$repo"
    # Make Config/Low.h and a sibling leaf include each other -> a 2-node SCC (same layer, so
    # this is a CYCLE not a back-edge). git add the new sibling — the audit scans tracked content
    # (git ls-files), so an untracked file is invisible to the graph.
    printf '#include "Config/Other.h"\n' > "$repo/Source/Core/include/Config/Low.h"
    printf '#include "Config/Low.h"\n' > "$repo/Source/Core/include/Config/Other.h"
    git -C "$repo" add -A
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"cycle"* ]]
}

@test "--diff PASSES (exit 0) for a clean high->low DAG (no new violation)" {
    repo="$BATS_TEST_TMPDIR/clean"
    _mk_repo "$repo"
    # Add another legal high->low edge: Commands(4) -> Config(1). git add the new tracked file.
    printf '#include "Config/Low.h"\n' > "$repo/Source/Core/include/Commands/Cmd.h"
    git -C "$repo" add -A
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"FAIL"* ]]
}

@test "--diff PASSES: a .cpp pulling in a higher-layer header is NOT a violation (graph sink)" {
    repo="$BATS_TEST_TMPDIR/cpp"
    _mk_repo "$repo"
    mkdir -p "$repo/Source/Core/src/Config"
    # A .cpp at Config layer including AppController.h (a higher layer) is the normal direction
    # of an implementation file — never a back-edge (a .cpp is a sink; nothing includes it).
    printf '#include "AppController.h"\n#include "Config/Low.h"\n' > "$repo/Source/Core/src/Config/Low.cpp"
    git -C "$repo" add -A
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"FAIL"* ]]
}

@test "--diff is silent for a back-edge already present at base (grandfathered ratchet)" {
    repo="$BATS_TEST_TMPDIR/grand"
    git init -q "$repo"; git -C "$repo" config user.email t@t.t; git -C "$repo" config user.name t
    mkdir -p "$repo/Source/Core/include/Config"
    # The back-edge exists AT base — it is grandfathered, so a no-op commit on top is clean.
    printf '#include "AppController.h"\n' > "$repo/Source/Core/include/Config/Low.h"
    printf '#include <functional>\n' > "$repo/Source/Core/include/AppController.h"
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    # Touch an unrelated clean file.
    printf '#include <string>\n' > "$repo/Source/Core/include/AppController.h"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"FAIL"* ]]
}

@test "--diff PASSES when SMATCHET_DEVIATION(rule=include-cycle) suppresses a new back-edge" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo"
    {
        echo '// SMATCHET_DEVIATION(rule=include-cycle; reason=test; owner=t; revisit=never)'
        echo '#include "AppController.h"'
        echo '#include <string>'
    } > "$repo/Source/Core/include/Config/Low.h"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"FAIL"* ]]
}
