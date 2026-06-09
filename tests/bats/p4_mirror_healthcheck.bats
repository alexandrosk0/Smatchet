#!/usr/bin/env bats
# tests/bats/p4_mirror_healthcheck.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/p4-mirror-healthcheck.sh.
#
# Stubs `git` (and `p4` for the fallback path) on PATH; controls the emitted
# SHAs + failure modes via env vars. No live p4d / GitHub / connector needed.
#
# Requires: bash, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    SCRIPT="$REPO_ROOT/scripts/dev/p4-mirror-healthcheck.sh"
    export SCRIPT

    export GITHUB_REMOTE="https://github.com/alexandrosk0/Smatchet"
    export MIRROR_REMOTE="http://localhost:1680"
    export MIRROR_REPO_PATH="repo/smatchet"
    export MIRROR_REF="develop"

    # Stub git + p4 on PATH. Must run AFTER the real `git rev-parse` above.
    STUB_BIN_DIR="$(mktemp -d)"
    export STUB_BIN_DIR

    cat > "$STUB_BIN_DIR/git" <<'STUB'
#!/usr/bin/env bash
# Stub git ls-remote, controlled via env:
#   STUB_GIT_GITHUB_SHA  — SHA emitted for the GitHub remote (empty => ref absent)
#   STUB_GIT_MIRROR_SHA  — SHA emitted for the mirror remote  (empty => ref absent)
#   STUB_GIT_FAIL_REMOTE — substring of a remote URL that must fail (exit 2, no output)
if [ "$1" = "ls-remote" ]; then
    remote="$2"; ref="$3"
    if [ -n "${STUB_GIT_FAIL_REMOTE:-}" ] && [[ "$remote" == *"$STUB_GIT_FAIL_REMOTE"* ]]; then
        exit 2
    fi
    case "$remote" in
        *github.com*) sha="${STUB_GIT_GITHUB_SHA:-}" ;;
        *)            sha="${STUB_GIT_MIRROR_SHA:-}" ;;
    esac
    [ -n "$sha" ] && printf '%s\t%s\n' "$sha" "$ref"
    exit 0
fi
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/git"

    cat > "$STUB_BIN_DIR/p4" <<'STUB'
#!/usr/bin/env bash
# Stub p4 graph log, controlled via env:
#   STUB_P4_MIRROR_SHA   — SHA emitted as the `commit <sha>` line (empty => no output)
#   STUB_P4_FAIL         — when set, exit 2 with no output
if [ "$1" = "graph" ] && [ "$2" = "log" ]; then
    [ -n "${STUB_P4_FAIL:-}" ] && exit 2
    [ -n "${STUB_P4_MIRROR_SHA:-}" ] && printf 'commit %s\n' "$STUB_P4_MIRROR_SHA"
    exit 0
fi
exit 0
STUB
    chmod +x "$STUB_BIN_DIR/p4"

    PATH="$STUB_BIN_DIR:$PATH"
    export PATH
}

teardown() {
    rm -rf "$STUB_BIN_DIR"
}

@test "in sync -> exit 0 with OK message" {
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    export STUB_GIT_MIRROR_SHA="abc123def4567890"
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"in sync"* ]]
}

@test "drift -> non-zero + DRIFT message" {
    export STUB_GIT_GITHUB_SHA="aaaaaaaaaaaaaaaa"
    export STUB_GIT_MIRROR_SHA="bbbbbbbbbbbbbbbb"
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"DRIFT"* ]]
}

@test "mirror unreachable -> non-zero + cannot-reach diagnostic" {
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    export STUB_GIT_FAIL_REMOTE="localhost:1680"
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot reach mirror"* ]]
}

@test "github unreachable -> non-zero + cannot-reach diagnostic" {
    export STUB_GIT_FAIL_REMOTE="github.com"
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot reach GitHub"* ]]
}

@test "mirror ref absent (repo not populated) -> non-zero + not-found" {
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    export STUB_GIT_MIRROR_SHA=""
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"not found"* ]]
}

@test "MIRROR_REMOTE unset (git mode) -> non-zero + clear error" {
    unset MIRROR_REMOTE
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"MIRROR_REMOTE unset"* ]]
}

@test "p4 fallback in sync -> exit 0" {
    export MIRROR_RESOLVE="p4"
    unset MIRROR_REMOTE
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    export STUB_P4_MIRROR_SHA="abc123def4567890"
    run bash "$SCRIPT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"in sync"* ]]
}

@test "p4 fallback unreachable -> non-zero" {
    export MIRROR_RESOLVE="p4"
    unset MIRROR_REMOTE
    export STUB_GIT_GITHUB_SHA="abc123def4567890"
    export STUB_P4_FAIL="1"
    run bash "$SCRIPT"
    [ "$status" -ne 0 ]
    [[ "$output" == *"cannot reach mirror"* ]]
}
