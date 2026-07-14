#!/usr/bin/env bats
# tests/bats/coderabbit_triage.bats
# ----------------------------------------------------------------------------
# Bats tests for `agents/scripts/core/coderabbit-triage.py selftest` — the
# md<->py rules-version drift guard (campaign findings core-agent-prompts-03 +
# core-scripts-python-03). coderabbit-triage.md is source-of-truth for the rule
# body; coderabbit-triage.py is its executable mirror; both carry a
# `triage-rules-version:` marker that must stay in lockstep. `selftest` is the
# end-of-CI sync check both files advertise.
#
# Requires: bash, a working python3 (exec-validated — the WinStore alias exits
# 49 on `-c ''`), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    PY_SRC="$REPO_ROOT/agents/scripts/core/coderabbit-triage.py"
    MD_SRC="$REPO_ROOT/agents/core/coderabbit-triage.md"
    export PY_SRC MD_SRC

    # Exec-validating python probe (agent_size.bats:25-29 canonical form): a
    # bare python3 can be a WinStore alias that resolves on PATH but exits 49.
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then
            PY="$c"
            break
        fi
    done
    [ -n "$PY" ] || skip "no working python interpreter"
    export PY

    WORK="$(mktemp -d)"
    export WORK
}

teardown() {
    [ -n "${WORK:-}" ] && rm -rf "$WORK"
}

# Build a mirror tree at $WORK with the real py + a (possibly tampered) md, so
# the script's Path(__file__).parents[2]/core/coderabbit-triage.md lookup lands
# on our fixture. `$1` (optional) rewrites the md marker version.
mirror() {
    local md_version="${1:-}"
    mkdir -p "$WORK/agents/scripts/core" "$WORK/agents/core"
    cp "$PY_SRC" "$WORK/agents/scripts/core/coderabbit-triage.py"
    if [ -n "$md_version" ]; then
        sed "s/triage-rules-version: [0-9][0-9]*/triage-rules-version: ${md_version}/" \
            "$MD_SRC" > "$WORK/agents/core/coderabbit-triage.md"
    else
        cp "$MD_SRC" "$WORK/agents/core/coderabbit-triage.md"
    fi
}

@test "selftest passes on the real tree (md and py markers in sync)" {
    run "$PY" "$PY_SRC" selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
    [[ "$output" == *"in sync"* ]]
}

@test "selftest reports the invariant-rule count" {
    run "$PY" "$PY_SRC" selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"invariant rules"* ]]
}

@test "selftest FAILS when the md marker drifts from the py marker" {
    mirror 99
    run "$PY" "$WORK/agents/scripts/core/coderabbit-triage.py" selftest
    [ "$status" -eq 1 ]
    [[ "$output" == *"DRIFT"* ]]
}

@test "selftest FAILS when the md marker is absent entirely" {
    mkdir -p "$WORK/agents/scripts/core" "$WORK/agents/core"
    cp "$PY_SRC" "$WORK/agents/scripts/core/coderabbit-triage.py"
    grep -v 'triage-rules-version:' "$MD_SRC" > "$WORK/agents/core/coderabbit-triage.md"
    run "$PY" "$WORK/agents/scripts/core/coderabbit-triage.py" selftest
    [ "$status" -eq 1 ]
    [[ "$output" == *"marker"* ]]
}

@test "selftest passes again when a drifted md is bumped back into sync" {
    # py marker is 4; set the md to 4 explicitly via the mirror rewrite.
    mirror 4
    run "$PY" "$WORK/agents/scripts/core/coderabbit-triage.py" selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}
