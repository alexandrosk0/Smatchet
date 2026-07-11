#!/usr/bin/env bats
# tests/bats/repo_health_facts_nudge.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/repo-health-facts-nudge.sh (SessionStart
# staleness nudge for tools/repo-health/facts.json).
#
# Deterministic without fixtures: the script keys on the git-commit age of the
# facts file, so tests pin "now" via REPO_HEALTH_NOW relative to the real last
# commit that touched facts.json.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/agents/scripts/core/repo-health-facts-nudge.sh"
    export SCRIPT
    LAST_COMMIT="$(git -C "$REPO_ROOT" log -1 --format='%ct' -- tools/repo-health/facts.json)"
    export LAST_COMMIT
}

@test "fresh (age < threshold) -> silent exit 0 in --nudge mode" {
    export REPO_HEALTH_NOW=$(( LAST_COMMIT + 86400 ))
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "fresh -> --list prints a fresh line" {
    export REPO_HEALTH_NOW=$(( LAST_COMMIT + 86400 ))
    run bash "$SCRIPT" --list
    [ "$status" -eq 0 ]
    [[ "$output" == *"fresh"* ]]
}

@test "stale (age >= threshold) -> nudge block, still exit 0" {
    export REPO_HEALTH_NOW=$(( LAST_COMMIT + 10 * 86400 ))
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"repo-health facts stale"* ]]
    [[ "$output" == *"Keeping facts.json fresh"* ]]
}

@test "threshold override via REPO_HEALTH_FACTS_MAX_AGE_DAYS" {
    export REPO_HEALTH_NOW=$(( LAST_COMMIT + 2 * 86400 ))
    export REPO_HEALTH_FACTS_MAX_AGE_DAYS=1
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [[ "$output" == *"stale"* ]]
}

@test "missing facts file -> silent exit 0" {
    export REPO_HEALTH_FACTS_FILE="tools/repo-health/no-such-facts.json"
    run bash "$SCRIPT" --nudge
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "untracked facts file (no git history) -> silent exit 0" {
    tmp="$(mktemp "$REPO_ROOT/tools/repo-health/untracked-XXXXXX.json")"
    export REPO_HEALTH_FACTS_FILE="tools/repo-health/$(basename "$tmp")"
    export REPO_HEALTH_NOW=$(( LAST_COMMIT + 100 * 86400 ))
    run bash "$SCRIPT" --nudge
    rm -f "$tmp"
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

@test "bad mode -> usage + exit 2" {
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
    [[ "$output" == *"usage"* ]]
}
