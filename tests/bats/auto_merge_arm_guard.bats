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

# ---------- tokenizer: bypasses of the old single-regex match ----------

@test "deny: gh global flags anywhere (gh pr -R o/r merge N --auto)" {
    for c in 'gh pr -R o/r merge 5 --auto' \
             'gh -R o/r pr merge 5 --auto' \
             'gh pr merge --repo o/r 5 --auto' \
             'C:/tools/gh.exe pr merge 5 --auto'; do
        bash_call "$c"
        denied
    done
}

@test "deny: keyword / compound-command prefixes (for-do, if-then, !, braces, subshell)" {
    for c in 'for p in 1 2; do gh pr merge $p --auto; done' \
             'if gh pr merge 5 --auto; then echo ok; fi' \
             'if true; then gh pr merge 5 --auto; fi' \
             'while true; do gh pr merge 5 --auto && break; done' \
             '! gh pr merge 5 --auto' \
             '{ gh pr merge 5 --auto; }' \
             '(cd /tmp; gh pr merge 5 --auto)' \
             $'cd /tmp\ngh pr merge 5 --auto'; do
        bash_call "$c"
        denied
    done
}

@test "deny: wrapper prefixes (xargs, timeout <dur>, nice, nohup, env, sudo)" {
    for c in 'echo 5 | xargs gh pr merge --auto' \
             'echo 5 | xargs -I{} gh pr merge {} --auto' \
             'timeout 30 gh pr merge 5 --auto' \
             'timeout -s KILL 30s gh pr merge 5 --auto' \
             'nice -n 5 nohup env A=1 gh pr merge 5 --auto &' \
             'sudo -u me gh pr merge 5 --auto'; do
        bash_call "$c"
        denied
    done
}

@test "deny: a quoted --auto or one glued to a redirection" {
    for c in 'gh pr merge 5 "--auto"' \
             "gh pr merge 5 '--auto' --squash" \
             'gh pr merge 5 --auto>/dev/null' \
             'gh pr merge 5 --auto 2>&1' \
             'gh pr merge 5 --auto=true'; do
        bash_call "$c"
        denied
    done
}

@test "deny: a heredoc / here-string / -c string fed to a shell, and eval" {
    for c in $'bash <<\'EOF\'\ngh pr merge 5 --auto\nEOF' \
             $'bash <<EOF\ngh pr merge 5 --auto\nEOF' \
             $'sh -s <<EOF\ngh pr merge 5 --auto\nEOF' \
             $'bash -s -- 5 <<\'EOF\'\ngh pr merge "$1" --auto\nEOF' \
             'bash <<< "gh pr merge 5 --auto"' \
             "bash -c 'gh pr merge 5 --auto'" \
             "sudo -u me bash -lc 'gh pr merge 5 --auto'" \
             'eval "gh pr merge 5 --auto"'; do
        bash_call "$c"
        denied
    done
}

@test "deny: command substitution runs, even inside double quotes or an unquoted heredoc" {
    for c in 'x="$(gh pr merge 5 --auto)"' \
             'echo "`gh pr merge 5 --auto`"' \
             'diff <(gh pr merge 5 --auto) x' \
             $'cat <<EOF\n$(gh pr merge 5 --auto)\nEOF'; do
        bash_call "$c"
        denied
    done
}

@test "deny: a gh api GraphQL enablePullRequestAutoMerge mutation" {
    bash_call "gh api graphql -f query='mutation { enablePullRequestAutoMerge(input: {pullRequestId: \"x\"}) { clientMutationId } }'"
    denied
}

# ---------- tokenizer: false denials of the old single-regex match ----------

@test "allow: a multi-line quoted argument whose line starts with gh pr merge N --auto" {
    bash_call $'git commit -m \'feat: x\n\ngh pr merge 5 --auto is banned here\''
    allowed
    bash_call $'git commit -m "feat: x\ngh pr merge 5 --auto"'
    allowed
}

@test "allow: a quoted --body carrying backticked gh pr merge --auto --squash text" {
    bash_call "gh pr create --title t --body 'never run \`gh pr merge --auto --squash\` by hand'"
    allowed
    # The repo's usual PR-body form: a quoted-delimiter heredoc inside \$(...).
    bash_call $'gh pr create --title t --body "$(cat <<\'EOF\'\n## Summary\nnever run `gh pr merge --auto --squash` by hand\nEOF\n)"'
    allowed
}

@test "allow: text that only looks like an arm (quoted separator, lookup, script file, --auto=false)" {
    for c in "echo ';' gh pr merge 5 --auto" \
             'command -v gh pr merge --auto' \
             $'bash script.sh <<EOF\ngh pr merge 5 --auto\nEOF' \
             'gh pr merge 12 --auto=false' \
             '# gh pr merge 5 --auto' \
             'bash agents/scripts/core/safe-merge.sh 2286 --auto'; do
        bash_call "$c"
        allowed
    done
}

@test "no working python: the regex fallback still denies a plain arm and allows text" {
    stub="$BATS_TEST_TMPDIR/nopy"
    mkdir -p "$stub"
    # A broken interpreter (the Windows Store alias shape): resolves on PATH, fails to run.
    for p in python3 python; do printf '#!/usr/bin/env bash\nexit 9009\n' > "$stub/$p"; chmod +x "$stub/$p"; done
    PATH="$stub:$PATH" bash_call 'gh pr merge 5 --squash --auto'
    denied
    PATH="$stub:$PATH" bash_call 'git commit -m "docs: never run gh pr merge --auto"'
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
