#!/usr/bin/env bats
# tests/bats/subsystem_docs.bats
# ----------------------------------------------------------------------------
# Tests for agents/scripts/project/test-subsystem-docs.sh.
#
# Each test runs the gate inside a throwaway sandbox git repo scaffolded to the
# layout the gate expects (the gate cd's to its own `../../..`, so copying it
# into <sandbox>/agents/scripts/project/ scopes every check to the sandbox).
# This keeps synthetic failure scenarios (missing leaf, unregistered leaf,
# duplicated rule, stale README) from touching the real tree.
#
# Requires: bash, git, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE_SRC="$REPO_ROOT/agents/scripts/project/test-subsystem-docs.sh"
    SANDBOX="$(mktemp -d)"

    mkdir -p "$SANDBOX/agents/scripts/project" "$SANDBOX/agents/core" \
             "$SANDBOX/Source/Core/src/Tracker"
    cp "$GATE_SRC" "$SANDBOX/agents/scripts/project/test-subsystem-docs.sh"

    printf '# code-review\n\nGlobal rules only; subsystem invariants live in the leaves.\n' \
        > "$SANDBOX/agents/core/code-review.md"

    printf '# Tracker rules\n\n## Invariants\n- No backend leak into the shared tracker interface family for any concrete backend.\n' \
        > "$SANDBOX/Source/Core/src/Tracker/AGENTS.md"

    cat > "$SANDBOX/CONTEXT-MAP.md" <<'MAP'
# CONTEXT-MAP

## Registry

| Subsystem | Rules | Glossary | Orientation |
|---|---|---|---|
| Tracker | `Source/Core/src/Tracker/AGENTS.md` | — | — |

## Growth path
Add `Source/Core/src/<ctx>/AGENTS.md` when content exists.
MAP

    git -C "$SANDBOX" init -q
    git -C "$SANDBOX" -c user.email=t@t.test -c user.name=test add -A
    git -C "$SANDBOX" -c user.email=t@t.test -c user.name=test commit -qm init

    GATE="$SANDBOX/agents/scripts/project/test-subsystem-docs.sh"
}

teardown() {
    [ -n "${SANDBOX:-}" ] && rm -rf "$SANDBOX"
}

_commit() {
    git -C "$SANDBOX" -c user.email=t@t.test -c user.name=test add -A
    git -C "$SANDBOX" -c user.email=t@t.test -c user.name=test commit -qm "$1"
}

@test "clean tree passes with summary line" {
    run bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

@test "selftest passes on a consistent registry" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
}

@test "registered leaf missing on disk FAILs" {
    rm "$SANDBOX/Source/Core/src/Tracker/AGENTS.md"
    run bash "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"registered leaf missing on disk"* ]]
    [[ "$output" == *"Passed: 0  Failed: 1"* ]]
}

@test "on-disk leaf absent from registry FAILs" {
    mkdir -p "$SANDBOX/Source/Core/src/Sync"
    printf '# Sync rules\n' > "$SANDBOX/Source/Core/src/Sync/AGENTS.md"
    _commit "add unregistered Sync leaf"
    run bash "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"not in"* ]]
    [[ "$output" == *"registry"* ]]
}

@test "leaf rule duplicated in the central checklist FAILs" {
    printf -- '- No backend leak into the shared tracker interface family for any concrete backend.\n' \
        >> "$SANDBOX/agents/core/code-review.md"
    run bash "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"duplicated"* ]]
}

@test "README staleness WARNs but never blocks" {
    printf '# Tracker orientation\n' > "$SANDBOX/Source/Core/src/Tracker/README.md"
    printf 'int x = 1;\n' > "$SANDBOX/Source/Core/src/Tracker/JiraClient.cpp"
    # register the README cell
    sed -i 's#| — | — |#| — | `Source/Core/src/Tracker/README.md` |#' "$SANDBOX/CONTEXT-MAP.md"
    _commit "add README + code baseline"
    printf 'int x = 1; int y = 2;\n' > "$SANDBOX/Source/Core/src/Tracker/JiraClient.cpp"
    _commit "change code, leave README untouched"
    run bash "$GATE" --diff HEAD~1
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"* ]]
    [[ "$output" == *"orientation may be stale"* ]]
    [[ "$output" == *"Passed: 1  Failed: 0"* ]]
}

@test "data-driven: a second subsystem README is discovered for staleness" {
    mkdir -p "$SANDBOX/Source/Core/src/Persistence"
    printf '# Persistence rules\n' > "$SANDBOX/Source/Core/src/Persistence/AGENTS.md"
    printf '# Persistence orientation\n' > "$SANDBOX/Source/Core/src/Persistence/README.md"
    printf 'int a = 0;\n' > "$SANDBOX/Source/Core/src/Persistence/LocalCacheManager.cpp"
    # Rewrite the registry with both subsystems (avoids sed/delimiter pitfalls).
    cat > "$SANDBOX/CONTEXT-MAP.md" <<'MAP'
# CONTEXT-MAP

## Registry

| Subsystem | Rules | Glossary | Orientation |
|---|---|---|---|
| Tracker | `Source/Core/src/Tracker/AGENTS.md` | — | — |
| Persistence | `Source/Core/src/Persistence/AGENTS.md` | — | `Source/Core/src/Persistence/README.md` |

## Growth path
Add a row when content exists.
MAP
    _commit "add Persistence leaf + README + code baseline"
    printf 'int a = 0; int b = 1;\n' > "$SANDBOX/Source/Core/src/Persistence/LocalCacheManager.cpp"
    _commit "change Persistence code only"
    run bash "$GATE" --diff HEAD~1
    [ "$status" -eq 0 ]
    [[ "$output" == *"Source/Core/src/Persistence"* ]]
    [[ "$output" == *"WARN"* ]]
}
