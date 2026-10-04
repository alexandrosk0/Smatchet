#!/usr/bin/env bats
# tests/bats/auto_merge_arm_guard.bats
# ----------------------------------------------------------------------------
# Bats coverage for docs/harness/claude-code/hooks/guard-auto-merge-arm.sh — the
# PreToolUse guard that denies arming GitHub-native auto-merge outside
# agents/scripts/core/safe-merge.sh (postmortems.md 2026-10-04 #2286; backlog
# infra/2026-10-04-native-auto-merge-merges-past-a-red-non-required-check).
#
# Deny = a permissionDecision "deny" JSON on stdout (exit 0); allow = exit 0 with
# no output.
#
# Requires: bash, bats, jq.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HOOK="$REPO_ROOT/docs/harness/claude-code/hooks/guard-auto-merge-arm.sh"
    TMPL="$REPO_ROOT/docs/harness/claude-code/settings.json.tmpl"
    export REPO_ROOT HOOK TMPL
}

# bash_call <command> — run the hook on a Bash tool call.
bash_call() {
    run bash "$HOOK" <<<"$(jq -n --arg c "$1" '{tool_name: "Bash", tool_input: {command: $c}}')"
}

# tool_call <tool-name> [tool-input-json]
tool_call() {
    local input="${2:-}"
    [ -n "$input" ] || input='{}'
    run bash "$HOOK" <<<"$(jq -n --arg t "$1" --argjson i "$input" '{tool_name: $t, tool_input: $i}')"
}

denied() {
    [ "$status" -eq 0 ]
    [[ "$output" == *'"permissionDecision":"deny"'* ]]
    [[ "$output" == *"safe-merge.sh"* ]]
    # The deny payload must be valid JSON (an unescaped quote would fail open).
    jq -e . >/dev/null <<<"$output"
}

allowed() {
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "deny: a bare gh pr merge --auto" {
    bash_call 'gh pr merge 2286 --squash --auto'
    denied
}

@test "deny: --auto anywhere in the gh pr merge args, path-qualified gh, env prefix, chained" {
    for c in 'gh pr merge --auto --squash 12' \
             '/usr/bin/gh pr merge 12 --squash --auto' \
             'GH_REPO=o/r gh pr merge 12 --auto' \
             'cd /tmp && gh pr merge 12 --squash --auto' \
             'git push && gh pr merge 12 --auto --delete-branch' \
             'out=$(gh pr merge 12 --auto)' \
             $'gh pr merge 12 \\\n  --squash \\\n  --auto'; do
        bash_call "$c"
        denied
    done
}

@test "deny: the GitHub-MCP enable_pr_auto_merge tool" {
    tool_call mcp__github__enable_pr_auto_merge '{"owner":"o","repo":"r","pullNumber":1}'
    denied
}

@test "deny: a harness set_auto_merge tool unless it explicitly disables" {
    tool_call set_auto_merge '{"pr":1,"enabled":true}'
    denied
    tool_call mcp__desktop__set_auto_merge '{"pr":1}'
    denied
    tool_call set_auto_merge '{"pr":1,"enabled":false}'
    allowed
}

@test "allow: the sanctioned safe-merge.sh path" {
    bash_call 'bash agents/scripts/core/safe-merge.sh 2286'
    allowed
    bash_call 'MERGE_GATES_FLIP_READY=true bash "$CLAUDE_PROJECT_DIR/agents/scripts/core/safe-merge.sh" 2286 --squash'
    allowed
}

@test "allow: disarming, plain merges and unrelated gh commands" {
    for c in 'gh pr merge 12 --disable-auto' \
             'gh pr view 12 --json autoMergeRequest' \
             'gh pr checks 12' \
             'gh pr merge 12 --squash --admin'; do
        bash_call "$c"
        allowed
    done
    tool_call mcp__github__disable_pr_auto_merge '{"pullNumber":1}'
    allowed
}

@test "allow: --auto only mentioned as text (commit message, echo, grep, heredoc body)" {
    for c in 'git commit -m "docs: never run gh pr merge --auto"' \
             'echo "use safe-merge.sh, not gh pr merge --auto"' \
             "grep -rn 'gh pr merge .* --auto' docs/" \
             $'cat > note.md <<EOF\ngh pr merge 12 --auto\nEOF'; do
        bash_call "$c"
        allowed
    done
}

@test "allow: unrelated tools and empty / malformed input" {
    tool_call Read '{"file_path":"/tmp/x"}'
    allowed
    run bash "$HOOK" </dev/null
    allowed
    run bash "$HOOK" <<<'not json'
    allowed
}

@test "settings.json.tmpl wires the hook on a PreToolUse matcher covering Bash + the MCP tools" {
    run jq -r '.hooks.PreToolUse[] | select(any(.hooks[]; .command | contains("guard-auto-merge-arm.sh"))) | .matcher' "$TMPL"
    [ "$status" -eq 0 ]
    [ -n "$output" ]
    for t in Bash PowerShell mcp__github__enable_pr_auto_merge mcp__desktop__set_auto_merge set_auto_merge; do
        [[ "$t" =~ $output ]]
    done
}
