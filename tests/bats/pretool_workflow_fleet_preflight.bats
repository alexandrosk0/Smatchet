#!/usr/bin/env bats
# tests/bats/pretool_workflow_fleet_preflight.bats
# ----------------------------------------------------------------------------
# Bats tests for docs/harness/claude-code/hooks/pretool-workflow-fleet-preflight.sh
# — the PreToolUse hook that runs `fleet-preflight.sh --strict` on a Workflow
# fan-out and BLOCKS (exit 2) a launch with a hard violation in an above-threshold
# (>2-agent) fan-out. (tooling.md fleet-preflight-not-enforced-at-launch option b.)
#
# Integration-style: drives the REAL fleet-preflight.sh via the hook. Fixtures
# mirror fleet-preflight's own selftest (prompts carry offset/limit + Source/X.cpp:N
# + build/x/r.md, so ONLY the model: pin varies → the sole hard violation is the
# unpinned agent()). PREFLIGHT_SLOTS pinned like the selftest so check-6 fan-out
# width can't false-warn on a low-core runner.
#
# Requires: bash, jq, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/pretool-workflow-fleet-preflight.sh"
    export HOOK
    # The hook resolves fleet-preflight.sh via CLAUDE_PROJECT_DIR.
    export CLAUDE_PROJECT_DIR="$REPO_ROOT"
    # Pin slot count so check 6 (fan-out width) can't warn on a <=3-core CI box.
    export PREFLIGHT_SLOTS=16
    # Hermetic hook defaults — a leaked override would flip block/threshold.
    unset SMATCHET_FLEET_PREFLIGHT_HOOK SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK \
          SMATCHET_FLEET_PREFLIGHT_HOOK_MIN_AGENTS

    VIOLATING_3="$(cat <<'EOF'
await parallel([
  agent({ prompt: 'read Source/Foo.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ prompt: 'read Source/Bar.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ prompt: 'read Source/Baz.cpp:10 offset 0 limit 50; write build/x/r.md' }),
]);
EOF
)"
    CLEAN_3="$(cat <<'EOF'
await pipeline([
  agent({ model: 'sonnet', prompt: 'read Source/Foo.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ model: 'sonnet', prompt: 'read Source/Bar.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ model: 'sonnet', prompt: 'read Source/Baz.cpp:10 offset 0 limit 50; write build/x/r.md' }),
]);
EOF
)"
    VIOLATING_2="$(cat <<'EOF'
await parallel([
  agent({ prompt: 'read Source/Foo.cpp:10 offset 0 limit 50; write build/x/r.md' }),
  agent({ prompt: 'read Source/Bar.cpp:10 offset 0 limit 50; write build/x/r.md' }),
]);
EOF
)"
}

# Pipe a Workflow tool_input (inline script) to the hook; sets $status/$output.
run_hook_script() {
    local payload
    payload="$(jq -n --arg s "$1" '{tool_name:"Workflow", tool_input:{script:$s}}')"
    printf '%s' "$payload" | bash "$HOOK"
}

# Pipe a Workflow tool_input (scriptPath) to the hook; sets $status/$output.
run_hook_scriptpath() {
    local payload
    payload="$(jq -n --arg p "$1" '{tool_name:"Workflow", tool_input:{scriptPath:$p}}')"
    printf '%s' "$payload" | bash "$HOOK"
}

@test "unpinned 3-agent fan-out is BLOCKED (exit 2)" {
    run run_hook_script "$VIOLATING_3"
    [ "$status" -eq 2 ]
    [[ "$output" == *"hard pre-launch violation"* ]]
    [[ "$output" == *"[fleet-preflight-hook] blocking launch"* ]]
}

@test "clean (pinned) 3-agent fan-out is ALLOWED (exit 0)" {
    run run_hook_script "$CLEAN_3"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "unpinned 2-agent fan-out is ALLOWED (at-or-below the >2 threshold)" {
    run run_hook_script "$VIOLATING_2"
    [ "$status" -eq 0 ]
}

@test "repo-relative scriptPath is resolved + enforced (not skipped fail-open)" {
    mkdir -p "$REPO_ROOT/build"
    local rel="build/.preflight-hook-scriptpath-test.js"
    printf '%s' "$VIOLATING_3" > "$REPO_ROOT/$rel"
    run run_hook_scriptpath "$rel"
    rm -f "$REPO_ROOT/$rel"
    [ "$status" -eq 2 ]
    [[ "$output" == *"[fleet-preflight-hook] blocking launch"* ]]
}

@test "SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK=0 downgrades block to advisory (exit 0, still warns)" {
    export SMATCHET_FLEET_PREFLIGHT_HOOK_BLOCK=0
    run run_hook_script "$VIOLATING_3"
    [ "$status" -eq 0 ]
    [[ "$output" == *"hard pre-launch violation"* ]]
    [[ "$output" != *"[fleet-preflight-hook] blocking launch"* ]]
}

@test "SMATCHET_FLEET_PREFLIGHT_HOOK=0 fully disables the hook (exit 0, silent)" {
    export SMATCHET_FLEET_PREFLIGHT_HOOK=0
    run run_hook_script "$VIOLATING_3"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "SMATCHET_FLEET_PREFLIGHT_HOOK_MIN_AGENTS raises the threshold (3-agent now below it)" {
    export SMATCHET_FLEET_PREFLIGHT_HOOK_MIN_AGENTS=4
    run run_hook_script "$VIOLATING_3"
    [ "$status" -eq 0 ]
}

@test "name-only payload (saved workflow, no inline script) is ALLOWED (exit 0)" {
    run bash -c 'printf "%s" "$(jq -n "{tool_name:\"Workflow\", tool_input:{name:\"some-saved-flow\"}}")" | bash "$HOOK"'
    [ "$status" -eq 0 ]
}

@test "non-Workflow tool payload is ignored (exit 0)" {
    run bash -c 'printf "%s" "$(jq -n "{tool_name:\"Bash\", tool_input:{command:\"ls\"}}")" | bash "$HOOK"'
    [ "$status" -eq 0 ]
}

@test "empty stdin is ALLOWED (fail-open, exit 0)" {
    run bash -c ': | bash "$HOOK"'
    [ "$status" -eq 0 ]
}
