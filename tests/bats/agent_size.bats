#!/usr/bin/env bats
# tests/bats/agent_size.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/core/agent_size_audit.py — the agent-prompt /
# AGENTS.md size delta gate (reduce-agent-prompt-bloat Slice 0), wired into
# test-lint-rules.sh + doc-validation.yml.
#
# Covers: per-class --scan-file classification (agent-prompt hard / AGENTS.md
# contract hard / sink soft-warn-only / out-of-scope), --selftest, and the --diff
# delta gate (new over-cap file fails, grandfathered file passes, SMATCHET_DEVIATION
# suppresses, sink never blocks) via throwaway git repos.
#
# Budgets resolve from the REAL repo's project.config.json (the audit's _repo_root
# is relative to its own __file__, not cwd), so throwaway repos see the live caps
# (prompt 250 / AGENTS.md 150 / sink 400) — same values --selftest asserts.
#
# Requires: bash, bats, a working python interpreter (python3/python/py).
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export AUD="$REPO_ROOT/agents/scripts/core/agent_size_audit.py"
    PY=""
    for c in python3 python py; do
        if command -v "$c" >/dev/null 2>&1 && "$c" -c "" >/dev/null 2>&1; then PY="$c"; break; fi
    done
    export PY
    [ -n "$PY" ] || skip "no working python interpreter"
}

# Emit an $2-line markdown file to $1 (one "x" per line).
_mk_md() {
    local out="$1" lines="$2" i=0
    : > "$out"
    while [ "$i" -lt "$lines" ]; do echo "filler line $i" >> "$out"; i=$((i+1)); done
}

# ---------- per-class detection (--scan-file; relative paths so classify() matches) ----------

@test "agent-prompt over 250 lines fires agent-too-long (hard, stdout)" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p agents/core
    _mk_md agents/core/big.md 300
    run "$PY" "$AUD" --scan-file agents/core/big.md
    [ "$status" -eq 0 ]
    [[ "$output" == *"agent-too-long"* ]]
    [[ "$output" == *"agents/core/big.md"* ]]
    [[ "$output" == *"agent-prompt"* ]]
}

@test "agent-prompt 200 lines (>150 soft, <250 hard) WARNS only, no hard violation" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p agents/project
    _mk_md agents/project/mid.md 200
    run "$PY" "$AUD" --scan-file agents/project/mid.md
    [ "$status" -eq 0 ]
    [[ "$output" != *"agent-too-long"* ]]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"soft tier"* ]]
}

@test "AGENTS.md over 150 lines fires agent-too-long (contract class)" {
    cd "$BATS_TEST_TMPDIR"
    _mk_md AGENTS.md 160
    run "$PY" "$AUD" --scan-file AGENTS.md
    [ "$status" -eq 0 ]
    [[ "$output" == *"agent-too-long"* ]]
    [[ "$output" == *"contract"* ]]
}

@test "skill SKILL.md over 400 lines WARNS only - sink never blocks" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p agents/_shared/skills/fake
    _mk_md agents/_shared/skills/fake/SKILL.md 450
    run "$PY" "$AUD" --scan-file agents/_shared/skills/fake/SKILL.md
    [ "$status" -eq 0 ]
    [[ "$output" != *"agent-too-long"* ]]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"skill"* ]]
}

@test "skill SKILL.md under 400 lines produces no output" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p agents/_shared/skills/small
    _mk_md agents/_shared/skills/small/SKILL.md 300
    run "$PY" "$AUD" --scan-file agents/_shared/skills/small/SKILL.md
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "out-of-scope file (agents/README.md) is ignored even when large" {
    cd "$BATS_TEST_TMPDIR"
    mkdir -p agents
    _mk_md agents/README.md 600
    run "$PY" "$AUD" --scan-file agents/README.md
    [ "$status" -eq 0 ]
    [[ "$output" != *"agent-too-long"* ]]
    [[ "$output" != *"WARN"* ]]
}

# ---------- selftest ----------

@test "--selftest passes (budgets in sync with project.config.json + AGENTS.md)" {
    run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"in sync"* ]]
}

# ---------- delta gate (--diff) via throwaway git repos ----------

# Build a repo with an agent prompt of $2 lines; $3 = optional content prefix (e.g. a deviation).
_mk_repo() {
    local dir="$1" lines="$2" prefix="${3:-}"
    git init -q "$dir"
    git -C "$dir" config user.email t@t.t
    git -C "$dir" config user.name t
    mkdir -p "$dir/agents/core"
    {
        [ -n "$prefix" ] && echo "$prefix"
        local i=0
        while [ "$i" -lt "$lines" ]; do echo "prompt line $i"; i=$((i+1)); done
    } > "$dir/agents/core/Big.md"
}

@test "--diff FAILS when a NEW agent prompt crosses the 250-line cap" {
    repo="$BATS_TEST_TMPDIR/new"
    _mk_repo "$repo" 50
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 300 ""        # grow past cap, uncommitted working tree
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"agent-too-long"* ]]
    [[ "$output" == *"agents/core/Big.md"* ]]
}

@test "--diff PASSES (grandfathered) when an already-over-cap prompt only grows" {
    repo="$BATS_TEST_TMPDIR/grand"
    _mk_repo "$repo" 300           # base ALREADY over 250
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 330 ""        # grows further, same file
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--diff PASSES when SMATCHET_DEVIATION(rule=agent-too-long) suppresses a new over-cap prompt" {
    repo="$BATS_TEST_TMPDIR/dev"
    _mk_repo "$repo" 50
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 300 "<!-- SMATCHET_DEVIATION(rule=agent-too-long; reason=test; owner=t; revisit=never) -->"
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "--diff STILL FAILS when the deviation token appears only in backtick prose (not a marker)" {
    # Regression guard: AGENTS.md documents the escape hatch in prose; a bare-substring
    # suppression match let that prose exempt AGENTS.md from its own cap (found 2026-07-13).
    repo="$BATS_TEST_TMPDIR/prose"
    _mk_repo "$repo" 50
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    _mk_repo "$repo" 300 '`SMATCHET_DEVIATION(rule=agent-too-long; ...)` anywhere in the file escapes'
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 1 ]
    [[ "$output" == *"agent-too-long"* ]]
    [[ "$output" == *"agents/core/Big.md"* ]]
}

@test "--diff does NOT block a NEW over-400 skill (sink WARNs, exit 0)" {
    repo="$BATS_TEST_TMPDIR/sink"
    git init -q "$repo"
    git -C "$repo" config user.email t@t.t
    git -C "$repo" config user.name t
    mkdir -p "$repo/agents/_shared/skills/fake"
    : > "$repo/agents/_shared/skills/fake/SKILL.md"
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    i=0; while [ "$i" -lt 450 ]; do echo "sink line $i" >> "$repo/agents/_shared/skills/fake/SKILL.md"; i=$((i+1)); done
    cd "$repo"
    run "$PY" "$AUD" --diff HEAD
    [ "$status" -eq 0 ]
    [[ "$output" != *"agent-too-long"* ]]
    [[ "$output" == *"WARN"* ]]
}

# ---------- --prune-stale-baseline (core-scripts-python-08) ----------
# SMATCHET_AGENTSIZE_BASELINE points the read/write at a throwaway file so the
# real committed baseline is never touched. scan_head() runs against the CWD
# repo, so a ghost path absent from an EMPTY throwaway repo reads as stale.

# Write a baseline-md snapshot listing one entry ($1 = path, default a ghost).
_mk_baseline() {
    local out="$1" entry="${2:-agents/core/ghost-nonexistent.md}"
    cat > "$out" <<EOF
# Agent-prompt / AGENTS.md size — grandfathered baseline

## agent-too-long (1 entries, cap 250 lines agent-prompt / 150 lines AGENTS.md)
- \`$entry\` · 999 lines (cap 250, agent-prompt)

## Totals
- oversized agent files grandfathered: 1
EOF
}

@test "--prune-stale-baseline flags a stale (deleted) entry and exits 1" {
    cd "$BATS_TEST_TMPDIR"; git init -q .
    bl="$BATS_TEST_TMPDIR/baseline.md"; _mk_baseline "$bl"
    SMATCHET_AGENTSIZE_BASELINE="$bl" run "$PY" "$AUD" --prune-stale-baseline
    [ "$status" -eq 1 ]
    [[ "$output" == *"stale-baseline"* ]]
    [[ "$output" == *"agents/core/ghost-nonexistent.md"* ]]
    [[ "$output" == *"deleted"* ]]
}

@test "--prune-stale-baseline --fix rewrites the snapshot (ghost dropped) and exits 0" {
    cd "$BATS_TEST_TMPDIR"; git init -q .
    bl="$BATS_TEST_TMPDIR/baseline_fix.md"; _mk_baseline "$bl"
    SMATCHET_AGENTSIZE_BASELINE="$bl" run "$PY" "$AUD" --prune-stale-baseline --fix
    [ "$status" -eq 0 ]
    [[ "$output" == *"rewritten"* ]]
    run grep -c "ghost-nonexistent" "$bl"
    [ "$output" -eq 0 ]
    # A re-run is now clean.
    SMATCHET_AGENTSIZE_BASELINE="$bl" run "$PY" "$AUD" --prune-stale-baseline
    [ "$status" -eq 0 ]
    [[ "$output" == *"no stale grandfather entries"* ]]
}

@test "--prune-stale-baseline is clean when the entry is still over-cap in the tree" {
    repo="$BATS_TEST_TMPDIR/current"; git init -q "$repo"
    git -C "$repo" config user.email t@t.t; git -C "$repo" config user.name t
    mkdir -p "$repo/agents/core"
    i=0; while [ "$i" -lt 300 ]; do echo "line $i" >> "$repo/agents/core/Big.md"; i=$((i+1)); done
    git -C "$repo" add -A && git -C "$repo" commit -qm base
    bl="$repo/baseline.md"; _mk_baseline "$bl" "agents/core/Big.md"
    cd "$repo"
    SMATCHET_AGENTSIZE_BASELINE="$bl" run "$PY" "$AUD" --prune-stale-baseline
    [ "$status" -eq 0 ]
    [[ "$output" == *"no stale grandfather entries"* ]]
}

@test "--selftest WARNs (but still passes) on a stale baseline entry" {
    cd "$BATS_TEST_TMPDIR"; git init -q .
    bl="$BATS_TEST_TMPDIR/baseline_st.md"; _mk_baseline "$bl"
    SMATCHET_AGENTSIZE_BASELINE="$bl" run "$PY" "$AUD" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN stale baseline entry"* ]]
    [[ "$output" == *"in sync"* ]]
}
