#!/usr/bin/env bats
#
# font_asset_resolve.bats — cmake/SmatchetFontAssets.cmake.
#
# assets/fonts/*.ttf is gitignored, so a linked git worktree starts without the
# Font Awesome TTF; the configure-time gate in CMakeLists.txt then skipped its
# POST_BUILD copy and every FA glyph in the toolbar / icon picker degraded to a
# text label with only a LOG_WARN to show for it. The fallback under test walks
# `git rev-parse --git-common-dir` back to the MAIN worktree and picks the TTF
# up from there.
#
# Headless by construction: drives `cmake -P` over real `git worktree add`
# fixtures — no build, no exe (CI cannot boot one; root AGENTS.md).
#
# Requires: cmake, git.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT

    if ! command -v cmake >/dev/null 2>&1; then
        skip "cmake not available"
    fi

    MODULE="${REPO_ROOT}/cmake/SmatchetFontAssets.cmake"
    MAIN="${BATS_TEST_TMPDIR}/main"
    LINKED="${BATS_TEST_TMPDIR}/linked"
}

# MSYS/Cygwin bash hands out POSIX paths that a native cmake.exe cannot open.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -m "$1"
    else
        printf '%s' "$1"
    fi
}

# A main worktree with one commit, plus a linked worktree off it.
make_trees() {
    mkdir -p "${MAIN}"
    git -C "${MAIN}" init --quiet
    git -C "${MAIN}" config user.email t@example.com
    git -C "${MAIN}" config user.name Test
    printf 'x\n' > "${MAIN}/seed.txt"
    git -C "${MAIN}" add seed.txt
    git -C "${MAIN}" commit --quiet -m seed
    git -C "${MAIN}" worktree add --quiet -b linked "${LINKED}" >/dev/null 2>&1
}

drop_ttf() {
    mkdir -p "$1/assets/fonts"
    printf 'ttf\n' > "$1/assets/fonts/fa-solid-900.ttf"
}

# Print the resolved path (or RESOLVED= for a miss) for source dir $1.
resolve() {
    local harness="${BATS_TEST_TMPDIR}/harness.cmake"
    cat > "${harness}" <<EOF
include("$(native_path "${MODULE}")")
smatchet_resolve_font_asset(out "$(native_path "$1")" "fa-solid-900.ttf")
message(STATUS "RESOLVED=\${out}")
EOF
    cmake -P "$(native_path "${harness}")" 2>&1
}

@test "resolves the TTF from the source dir when it is present" {
    make_trees
    drop_ttf "${MAIN}"

    run resolve "${MAIN}"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RESOLVED="*"/main/assets/fonts/fa-solid-900.ttf"* ]]
}

@test "linked worktree falls back to the main worktree's TTF" {
    make_trees
    drop_ttf "${MAIN}"

    run resolve "${LINKED}"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RESOLVED="*"/main/assets/fonts/fa-solid-900.ttf"* ]]
}

@test "linked worktree prefers its own TTF over the main worktree's" {
    make_trees
    drop_ttf "${MAIN}"
    drop_ttf "${LINKED}"

    run resolve "${LINKED}"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RESOLVED="*"/linked/assets/fonts/fa-solid-900.ttf"* ]]
}

@test "resolves empty when neither worktree has the TTF" {
    make_trees

    run resolve "${LINKED}"
    [ "$status" -eq 0 ]
    [[ "$output" == *"RESOLVED="* ]]
    [[ "$output" != *"fa-solid-900.ttf"* ]]
}

@test "resolves empty outside a git tree without failing" {
    mkdir -p "${BATS_TEST_TMPDIR}/plain"

    run resolve "${BATS_TEST_TMPDIR}/plain"
    [ "$status" -eq 0 ]
    [[ "$output" != *"fa-solid-900.ttf"* ]]
}
