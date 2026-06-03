#!/usr/bin/env bats
# tests/bats/issue_sweep.bats
# Coverage for agents/scripts/core/issue-sweep.sh (issue-triage Slice 3).
# Tests the gh-free surface via an ISSUES_JSON fixture: verdict logic + the
# [issue-propose] line + the bot-only auto-act guardrail (dry-run mutates nothing).

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPT="$REPO_ROOT/agents/scripts/core/issue-sweep.sh"
    WORK="$BATS_TEST_TMPDIR"; export WORK
    for c in python3 python py; do command -v "$c" >/dev/null 2>&1 && { PY="$c"; break; }; done
    [ -n "${PY:-}" ] || skip "no python"
    cat > "$WORK/issues.json" <<'JSON'
[
 {"number":734,"title":"bulkImportFutures.clear() blocks UI thread","author":{"login":"app/coderabbitai"},"labels":[]},
 {"number":900,"title":"User crash on export","author":{"login":"alexk"},"labels":[{"name":"bug"},{"name":"P0"}]},
 {"number":901,"title":"bot found a P1 race","author":{"login":"coderabbitai[bot]"},"labels":[{"name":"bug"},{"name":"P1"}]}
]
JSON
    export ISSUES_JSON="$WORK/issues.json"
}

@test "bot-authored unlabeled Issue -> RELABEL" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RELABEL #734"*"bot"* ]]
}

@test "human-authored Issue -> KEEP (report-only, never auto-acted)" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"KEEP    #900"*"human"* ]]
}

@test "labelled bot Issue -> KEEP (already triaged)" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"KEEP    #901"* ]]
}

@test "the top P0 is surfaced as an [issue-propose] line" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"[issue-propose] #900"* ]]
    [[ "$output" == *"gh issue develop 900"* ]]
}

@test "dry-run acts on nothing" {
    ISSUES_JSON="$ISSUES_JSON" run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"dry-run; --apply acts on bot-authored only"* ]]
}

@test "bad arg exits 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}
