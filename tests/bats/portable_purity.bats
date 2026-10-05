#!/usr/bin/env bats
# tests/bats/portable_purity.bats
# ----------------------------------------------------------------------------
# Rename-following in agents/scripts/core/test-portable-purity.sh (process.md
# 2026-06-03 "a doc-file RENAME silently orphans the test-portable-purity
# baseline", part 3). The baseline keys grandfathered literals by FILENAME, so a
# `git mv` used to re-report every carried-over literal as a NEW leak. Check mode
# now remaps baseline keys old -> new from `git diff -M --name-status
# --diff-filter=R <merge-base>` before comparing.
#
# Invariants under test (a real throwaway repo per test — the script reads git
# rename detection, the config denylist and the committed baseline):
#   1. a pure rename of a portable doc stays green;
#   2. a rename that ALSO adds a new project literal goes red, naming the new path;
#   3. with no resolvable base ref (shallow / no develop) behaviour is unchanged:
#      the renamed file's grandfathered literal reports as new.
#
# Requires: bash, bats, git, python3.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    tmp="$(mktemp -d)"
}

teardown() {
    [ -n "${tmp:-}" ] && rm -rf "$tmp"
    return 0
}

# make_repo <base-branch> — fixture with one grandfathered literal in a portable
# doc, committed on <base-branch>; leaves HEAD on a `feature` branch.
make_repo() {
    git -C "$tmp" init -q -b "$1"
    git -C "$tmp" config user.email t@t && git -C "$tmp" config user.name t
    mkdir -p "$tmp/agents/scripts/core" "$tmp/docs/agent-rules" "$tmp/docs/high-integrity"
    cp "$REPO_ROOT/agents/scripts/core/test-portable-purity.sh" "$tmp/agents/scripts/core/"
    printf '%s\n' '{"project":{"name":"Acme","env_prefix":"ACME","literals":["Acme"]},' \
        '"build":{"presets":[]},"vcs":{"p4_streams":[]}}' > "$tmp/project.config.json"
    printf '# Rule\n\nThe Acme widget rule, carried over verbatim.\n' > "$tmp/docs/agent-rules/old.md"
    printf 'docs/agent-rules/old.md\tAcme\n' > "$tmp/docs/high-integrity/portable-purity-baseline.txt"
    git -C "$tmp" add -A && git -C "$tmp" commit -qm base
    git -C "$tmp" checkout -qb feature
}

purity() {
    run bash -c "cd '$tmp' && PORTABLE_PURITY_BASE='${1:-}' bash agents/scripts/core/test-portable-purity.sh"
}

@test "portable-purity: baseline holds on the unchanged fixture" {
    make_repo develop
    purity develop
    [ "$status" -eq 0 ]
    [[ "$output" == *"baseline holds"* ]]
}

@test "portable-purity: a pure git mv of a portable doc stays green (baseline key follows the rename)" {
    make_repo develop
    git -C "$tmp" mv docs/agent-rules/old.md docs/agent-rules/new.md
    git -C "$tmp" commit -qm rename
    purity develop
    [ "$status" -eq 0 ]
    [[ "$output" == *"followed 1 rename(s)"* ]]
    [[ "$output" == *"baseline holds"* ]]
}

@test "portable-purity: a staged (uncommitted) git mv is followed too" {
    make_repo develop
    git -C "$tmp" mv docs/agent-rules/old.md docs/agent-rules/new.md
    purity develop
    [ "$status" -eq 0 ]
    [[ "$output" == *"baseline holds"* ]]
}

@test "portable-purity: rename plus a NEW literal in the renamed file goes red" {
    make_repo develop
    git -C "$tmp" mv docs/agent-rules/old.md docs/agent-rules/new.md
    printf 'Set ACME_FLAG before running.\n' >> "$tmp/docs/agent-rules/new.md"
    git -C "$tmp" commit -qam "rename + new literal"
    purity develop
    [ "$status" -eq 1 ]
    [[ "$output" == *"docs/agent-rules/new.md"*"ACME"* ]]
    # Only the NEW literal is reported; the grandfathered one followed the rename.
    local tab=$'\t'
    [[ "$output" != *"new.md${tab}Acme"* ]]
}

@test "portable-purity: no resolvable base ref keeps the filename-keyed behaviour (rename reports)" {
    make_repo main
    git -C "$tmp" mv docs/agent-rules/old.md docs/agent-rules/new.md
    git -C "$tmp" commit -qm rename
    purity ""
    [ "$status" -eq 1 ]
    [[ "$output" == *"docs/agent-rules/new.md"*"Acme"* ]]
}
