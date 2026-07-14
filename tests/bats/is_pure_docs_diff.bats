#!/usr/bin/env bats
# is_pure_docs_diff.bats — regression suite for
# agents/scripts/core/is-pure-docs-diff.sh (the FF-clean docs-batch classifier
# that lets git-janitor skip the test-all.sh gate on a pure-docs diff).
#
# Contract under test (exit codes):
#   0 — the ahead-range diff is strictly within the pure-docs allow-list
#       (docs/**, backlog/**, agents/scripts/**, or ANY *.md at any depth), OR
#       the diff is empty.
#   1 — at least one changed file is outside the allow-list (a .cpp, a CMake
#       file, a non-scripts agents/ file, .github/, etc.).
#   2 — the base ref cannot be resolved.
#
# Runs against a REAL throwaway git repo (no stubs): the script's whole job is
# to read `git diff --name-only base...HEAD`, so a real repo is the faithful
# fixture. base resolution prefers origin/<name>; with no remote it falls back
# to the local branch, so the suite drives a local `develop` branch.

setup() {
    SCRIPT="$(git rev-parse --show-toplevel)/agents/scripts/core/is-pure-docs-diff.sh"
    export SCRIPT
    REPO_TMP="$(mktemp -d)"
    export REPO_TMP
    git init --quiet -b develop "$REPO_TMP"
    git -C "$REPO_TMP" config user.email t@t
    git -C "$REPO_TMP" config user.name t
    # Seed develop with one committed file so the base ref exists.
    echo seed > "$REPO_TMP/seed.txt"
    git -C "$REPO_TMP" add seed.txt
    git -C "$REPO_TMP" commit --quiet -m seed
    git -C "$REPO_TMP" checkout --quiet -b feature
}

teardown() {
    rm -rf "${REPO_TMP:-}"
}

# commit_files <path> [<path> ...] — create each path (with any parent dirs),
# stage, and commit on the current (feature) branch.
commit_files() {
    local p
    for p in "$@"; do
        mkdir -p "$REPO_TMP/$(dirname "$p")"
        echo x > "$REPO_TMP/$p"
    done
    git -C "$REPO_TMP" add -A
    git -C "$REPO_TMP" commit --quiet -m "add $*"
}

# Run the classifier from inside the temp repo with base=develop.
run_classify() {
    run bash -c 'cd "$1" && bash "$2" develop' _ "$REPO_TMP" "$SCRIPT"
}

@test "empty diff (feature == develop) is pure-docs -> exit 0" {
    run_classify
    [ "$status" -eq 0 ]
}

@test "docs/** only -> exit 0" {
    commit_files "docs/guides/foo.md" "docs/adr/0099-bar.md"
    run_classify
    [ "$status" -eq 0 ]
}

@test "backlog/** only -> exit 0" {
    commit_files "backlog/item.md"
    run_classify
    [ "$status" -eq 0 ]
}

@test "agents/scripts/** only -> exit 0" {
    commit_files "agents/scripts/core/whatever.sh"
    run_classify
    [ "$status" -eq 0 ]
}

@test "a .md anywhere (incl. under Source/) -> exit 0" {
    commit_files "Source/Core/src/Tracker/AGENTS.md" "CONTEXT-MAP.md"
    run_classify
    [ "$status" -eq 0 ]
}

@test "a C++ source file -> exit 1 (outside allow-list)" {
    commit_files "Source/Core/src/Tracker/Foo.cpp"
    run_classify
    [ "$status" -eq 1 ]
}

@test "mixed docs + one non-doc -> exit 1 (any outside file denies)" {
    commit_files "docs/ok.md" "CMakeLists.txt"
    run_classify
    [ "$status" -eq 1 ]
}

@test "a non-scripts agents/ file -> exit 1" {
    commit_files "agents/core/some-agent.md.tmpl"
    run_classify
    [ "$status" -eq 1 ]
}

@test "a .github/ workflow -> exit 1" {
    commit_files ".github/workflows/ci.yml"
    run_classify
    [ "$status" -eq 1 ]
}

@test "unresolvable base ref -> exit 2" {
    commit_files "docs/ok.md"
    run bash -c 'cd "$1" && bash "$2" __no_such_base__' _ "$REPO_TMP" "$SCRIPT"
    [ "$status" -eq 2 ]
}
