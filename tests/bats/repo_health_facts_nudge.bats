#!/usr/bin/env bats
# tests/bats/repo_health_facts_nudge.bats
# ----------------------------------------------------------------------------
# Bats tests for agents/scripts/core/repo-health-facts-nudge.sh (SessionStart
# staleness nudge for tools/repo-health/facts.json).
#
# Hermetic: each test runs a COPY of the script inside a scratch git repo whose
# only commit adds a stub tools/repo-health/facts.json. The script resolves its
# tree from its own location, so the copy reads the scratch repo, never the real
# one — the suite needs no product dashboard data (the standalone agent layer has
# none) and never writes into the real working tree. "now" is pinned via
# REPO_HEALTH_NOW relative to that commit's time.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SANDBOX="$(mktemp -d)"
    mkdir -p "$SANDBOX/agents/scripts/core" "$SANDBOX/tools/repo-health"
    cp "$REPO_ROOT/agents/scripts/core/repo-health-facts-nudge.sh" "$SANDBOX/agents/scripts/core/"
    printf '{"updated": "2026-01-01"}\n' > "$SANDBOX/tools/repo-health/facts.json"
    git -C "$SANDBOX" init -q
    git -C "$SANDBOX" add -A
    GIT_COMMITTER_DATE="2026-01-01T00:00:00Z" GIT_AUTHOR_DATE="2026-01-01T00:00:00Z" \
        git -C "$SANDBOX" -c user.name=bats -c user.email=bats@invalid -c commit.gpgsign=false \
            commit -q -m "stub facts"
    SCRIPT="$SANDBOX/agents/scripts/core/repo-health-facts-nudge.sh"
    export SCRIPT
    LAST_COMMIT="$(git -C "$SANDBOX" log -1 --format='%ct' -- tools/repo-health/facts.json)"
    export LAST_COMMIT
}

teardown() {
    rm -rf "${SANDBOX:-}"
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
    tmp="$(mktemp "$SANDBOX/tools/repo-health/untracked-XXXXXX.json")"
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
