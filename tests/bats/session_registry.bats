#!/usr/bin/env bats
# tests/bats/session_registry.bats
# ----------------------------------------------------------------------------
# Bats tests for the concurrent-session registry liveness layer:
#   - agents/scripts/core/session-registry-lib.sh   (shared primitives + --selftest)
#   - docs/harness/claude-code/hooks/guard-shared-tree.sh  (PreToolUse aggressor guard)
#   - agents/scripts/core/session-tree-banner.sh    (SessionStart writer + cleanup)
#
# Covers the ppid=1 liveness defect fix (tooling.md):
#   - A real session pid that is alive  -> sibling is LIVE  (guard denies the op).
#   - A real session pid that has EXITED -> sibling is DEAD even while its ts is
#     still inside the 30-min freshness window (the over-block fix: a just-closed
#     sibling stops blocking at once instead of for up to 30 min).
#   - Legacy ppid=1 entries (no authoritative pid) fall back to ts freshness
#     exactly as before — fresh => live, stale => not — so no regression.
#   - Own entry is never counted / never pruned; non-mutating ops pass; the
#     SMATCHET_ALLOW_SHARED_SWITCH=1 override always allows.
#   - Banner cleanup prunes ONLY dead+stale entries (keeps live, dead+fresh,
#     legacy, own) plus the lazy >7d sweep for ancient legacy entries.
#
# Liveness is forced onto the POSIX (kill -0) branch via SMATCHET_REGISTRY_OS=posix
# so the suite is deterministic on Linux CI and Windows git-bash alike: a live pid
# is this test process ($$), a dead pid is 4000000000 (never a live Win32/POSIX pid).
#
# Requires: bash, git, jq, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export LIB="$REPO_ROOT/agents/scripts/core/session-registry-lib.sh"
    export GUARD="$REPO_ROOT/docs/harness/claude-code/hooks/guard-shared-tree.sh"
    export BANNER="$REPO_ROOT/agents/scripts/core/session-tree-banner.sh"

    # Sandbox integration tree (a real .git DIRECTORY => the guard treats it as the
    # shared tree). Per-test isolation.
    SANDBOX="$(mktemp -d)"
    export SANDBOX
    export MAIN="$SANDBOX/main"
    git init --quiet -b develop "$MAIN"
    git -C "$MAIN" -c user.email=t@t -c user.name=t commit --allow-empty --quiet -m seed

    export REGDIR="$MAIN/.claude/.active-sessions"
    mkdir -p "$REGDIR"

    export CLAUDE_PROJECT_DIR="$MAIN"
    # Determinism: POSIX liveness branch (kill -0), default freshness window.
    export SMATCHET_REGISTRY_OS=posix
    export SMATCHET_REGISTRY_FRESH_SECS=1800
    # A pid that is definitely alive while this test runs, and one that is not.
    export ALIVE_PID="$$"
    export DEAD_PID=4000000000
    export NOW
    NOW="$(date -u +%s)"
    export STALE_TS=$((NOW - 9000))      # 2.5 h old  -> ts-stale
    export ANCIENT_TS=$((NOW - 700000))  # ~8.1 d old -> caught by the >7d sweep
}

teardown() {
    rm -rf "$SANDBOX"
}

# seed <sid> <ppid> <ts> — write one registry entry.
seed() {
    {
        echo "branch=develop"
        echo "sha=$(git -C "$MAIN" rev-parse HEAD)"
        echo "ppid=$2"
        echo "ts=$3"
    } > "$REGDIR/$1"
}

# Feed the shared-tree guard a PreToolUse event ($1 = invoking session_id, $2 =
# command); output captured by `run`.
invoke_guard() {
    jq -cn --arg sid "$1" --arg cmd "$2" \
        '{session_id:$sid, tool_name:"Bash", tool_input:{command:$cmd}}' \
        | bash "$GUARD"
}

assert_denied() {
    [ "$status" -eq 0 ]
    echo "$output" | jq -e '.hookSpecificOutput.permissionDecision == "deny"' >/dev/null
}

assert_allowed() {
    [ "$status" -eq 0 ]
    [ -z "$output" ]
}

# --- guard: liveness drives deny/allow ---------------------------------------

@test "guard: live real-pid sibling denies a HEAD-moving op" {
    seed sib-live "$ALIVE_PID" "$NOW"
    run invoke_guard my-session "git pull"
    assert_denied
}

@test "guard: dead real-pid sibling with FRESH ts allows the op (over-block fix)" {
    # The bug: a just-closed sibling kept blocking for up to 30 min because its
    # ts was still fresh. With an authoritative pid we know it exited -> allow.
    seed sib-dead-fresh "$DEAD_PID" "$NOW"
    run invoke_guard my-session "git pull"
    assert_allowed
}

@test "guard: dead real-pid sibling with STALE ts allows the op" {
    seed sib-dead-stale "$DEAD_PID" "$STALE_TS"
    run invoke_guard my-session "git pull"
    assert_allowed
}

@test "guard: legacy ppid=1 sibling with FRESH ts denies (ts fallback, no regression)" {
    seed sib-legacy-fresh 1 "$NOW"
    run invoke_guard my-session "git checkout main"
    assert_denied
}

@test "guard: legacy ppid=1 sibling with STALE ts allows (ts fallback)" {
    seed sib-legacy-stale 1 "$STALE_TS"
    run invoke_guard my-session "git checkout main"
    assert_allowed
}

@test "guard: own entry is excluded from the sibling count (allow)" {
    seed my-session "$ALIVE_PID" "$NOW"
    run invoke_guard my-session "git pull"
    assert_allowed
}

@test "guard: a non-mutating git op passes even with a live sibling" {
    seed sib-live "$ALIVE_PID" "$NOW"
    run invoke_guard my-session "git status"
    assert_allowed
}

@test "guard: SMATCHET_ALLOW_SHARED_SWITCH=1 overrides a live-sibling deny" {
    seed sib-live "$ALIVE_PID" "$NOW"
    SMATCHET_ALLOW_SHARED_SWITCH=1 run invoke_guard my-session "git pull"
    assert_allowed
}

@test "guard: missing registry dir never false-blocks (allow)" {
    rm -rf "$REGDIR"
    run invoke_guard my-session "git pull"
    assert_allowed
}

@test "guard: stash pop is treated as mutating (live sibling -> deny)" {
    seed sib-live "$ALIVE_PID" "$NOW"
    run invoke_guard my-session "git stash pop"
    assert_denied
}

# --- banner: SessionStart cleanup --------------------------------------------

# Run the banner hook with a fresh own session id; output discarded.
run_banner() {
    jq -cn --arg sid "$1" '{session_id:$sid}' | bash "$BANNER" >/dev/null 2>&1
}

@test "banner: prunes a dead+stale entry" {
    seed gone "$DEAD_PID" "$STALE_TS"
    run_banner banner-sid
    [ ! -f "$REGDIR/gone" ]
}

@test "banner: keeps a dead+FRESH entry (precise prune needs both conditions)" {
    seed dead-fresh "$DEAD_PID" "$NOW"
    run_banner banner-sid
    [ -f "$REGDIR/dead-fresh" ]
}

@test "banner: keeps a live entry" {
    seed live "$ALIVE_PID" "$STALE_TS"
    run_banner banner-sid
    [ -f "$REGDIR/live" ]
}

@test "banner: keeps a legacy ppid=1 stale entry (unprovable -> not dead-pruned)" {
    seed legacy-stale 1 "$STALE_TS"
    run_banner banner-sid
    [ -f "$REGDIR/legacy-stale" ]
}

@test "banner: the >7d sweep removes an ancient legacy entry" {
    seed legacy-ancient 1 "$ANCIENT_TS"
    run_banner banner-sid
    [ ! -f "$REGDIR/legacy-ancient" ]
}

@test "banner: never prunes its own entry, and records an authoritative-or-PPID pid" {
    # Pre-seed the own entry as dead+stale; the banner must NOT prune it (own sid),
    # and must rewrite it with a fresh ts + a numeric ppid.
    seed banner-sid "$DEAD_PID" "$STALE_TS"
    run_banner banner-sid
    [ -f "$REGDIR/banner-sid" ]
    run grep -E '^ppid=[0-9]+$' "$REGDIR/banner-sid"
    [ "$status" -eq 0 ]
}

@test "banner: writes a SessionStart entry when none exists" {
    rm -f "$REGDIR/new-sid"
    run_banner new-sid
    [ -f "$REGDIR/new-sid" ]
}

# --- lib: self-test ----------------------------------------------------------

@test "session-registry-lib.sh --selftest passes" {
    run bash "$LIB" --selftest
    [ "$status" -eq 0 ]
    echo "$output" | grep -qE '^1\.\.[0-9]+'
    ! echo "$output" | grep -qE '^not ok'
}

@test "banner: WARNs before the degraded PPID fallback when session-registry-lib.sh is missing (hooks-session-lifecycle-04)" {
    # Copy the banner ALONE into an isolated dir (no sibling lib) with a PROJ that
    # also lacks agents/scripts/core/session-registry-lib.sh — both lookups miss,
    # so the degraded $PPID-liveness fallback fires. It must WARN first, not
    # silently degrade.
    local isodir="$SANDBOX/nolib"
    mkdir -p "$isodir"
    cp "$BANNER" "$isodir/session-tree-banner.sh"
    run bash -c "jq -cn '{session_id:\"s1\"}' | CLAUDE_PROJECT_DIR='$isodir' bash '$isodir/session-tree-banner.sh' 2>&1 1>/dev/null"
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"session-registry-lib.sh not found"* ]]
}
