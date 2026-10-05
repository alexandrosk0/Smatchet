#!/usr/bin/env bats
# tests/bats/setup_harness_hooks_path.bats
# ----------------------------------------------------------------------------
# core.hooksPath repair in agents/scripts/core/setup-harness.sh (install_git_hooks,
# reached through the `git-hooks` subcommand) and the matching doctor.sh WARN
# (tooling 2026-08-19, absolute core.hooksPath serves a stale hook to worktrees).
#
# git runs a hook from the worktree root, so the relative `scripts/git-hooks`
# resolves inside each worktree, while an absolute path into the main checkout
# makes every worktree run main's revision of the hooks. The fixture is a temp
# repo whose main checkout and linked worktree carry DIFFERENT pre-commit hooks
# that each write their own name to a marker, so "which hook ran" is observable.
# Both scripts are committed into the fixture and run from it, so they resolve
# their roots there and never touch the real repo's config.
#
# Requires: bash, git (>= 2.20 for extensions.worktreeConfig), bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    BASE="$(mktemp -d)"
    MAIN="$BASE/main"
    WT="$BASE/wt"
    MARKER="$BASE/marker"
    git init -q -b develop "$MAIN"
    git -C "$MAIN" config user.email t@local
    git -C "$MAIN" config user.name t
    mkdir -p "$MAIN/scripts/git-hooks" "$MAIN/scripts/dev" "$MAIN/agents/scripts/core/lib"
    cp "$REPO_ROOT/agents/scripts/core/setup-harness.sh" "$MAIN/agents/scripts/core/"
    cp "$REPO_ROOT/agents/scripts/core/lib/agents-dir-current.sh" "$MAIN/agents/scripts/core/lib/"
    cp "$REPO_ROOT/scripts/dev/doctor.sh" "$MAIN/scripts/dev/"
    write_hook "$MAIN" main
    git -C "$MAIN" add -A
    git -C "$MAIN" commit -qm seed
    git -C "$MAIN" worktree add -q -b wt "$WT" >/dev/null 2>&1
    # The worktree's copy of the hook differs from main's (an unmerged fix).
    write_hook "$WT" worktree
}

teardown() { rm -rf "$BASE" 2>/dev/null || true; }

write_hook() {  # <tree> <name> — a pre-commit hook that records <name>
    printf '#!/usr/bin/env bash\necho %s > "%s"\n' "$2" "$MARKER" > "$1/scripts/git-hooks/pre-commit"
    chmod +x "$1/scripts/git-hooks/pre-commit"
}

install_hooks() {  # <tree> — run that tree's setup-harness.sh git-hooks from it
    ( cd "$1" && bash "$1/agents/scripts/core/setup-harness.sh" git-hooks )
}

hooks_path() {  # <git config scope args...>
    git -C "$WT" config "$@" --get core.hooksPath
}

@test "an absolute hooksPath into the main checkout is rewritten to the relative form" {
    git -C "$MAIN" config --local core.hooksPath "$MAIN/scripts/git-hooks"
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"rewritten to scripts/git-hooks"* ]]
    [ "$(hooks_path --local)" = "scripts/git-hooks" ]
}

@test "after the repair a worktree runs its own hook revision, not main's" {
    git -C "$MAIN" config --local core.hooksPath "$MAIN/scripts/git-hooks"
    git -C "$WT" commit -q --allow-empty -m before
    [ "$(cat "$MARKER")" = "main" ]           # the defect: main's hook ran
    install_hooks "$WT"
    git -C "$WT" commit -q --allow-empty -m after
    [ "$(cat "$MARKER")" = "worktree" ]       # the worktree's own hook ran
}

@test "an absolute hooksPath in a config.worktree is rewritten too" {
    git -C "$MAIN" config --local core.hooksPath scripts/git-hooks
    git -C "$MAIN" config --local extensions.worktreeConfig true
    git -C "$WT" config --worktree core.hooksPath "$MAIN/scripts/git-hooks"
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"config.worktree: absolute core.hooksPath"* ]]
    [ "$(hooks_path --worktree)" = "scripts/git-hooks" ]
    git -C "$WT" commit -q --allow-empty -m after
    [ "$(cat "$MARKER")" = "worktree" ]
}

@test "an absolute path to this worktree's own hooks dir is rewritten" {
    git -C "$MAIN" config --local core.hooksPath "$WT/scripts/git-hooks"
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [ "$(hooks_path --local)" = "scripts/git-hooks" ]
}

@test "a symlinked spelling of the repo's hooks dir is recognised by its real path" {
    case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) skip "ln -s copies instead of linking on Windows" ;; esac
    ln -s "$MAIN" "$BASE/alias"
    git -C "$MAIN" config --local core.hooksPath "$BASE/alias/scripts/git-hooks"
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [ "$(hooks_path --local)" = "scripts/git-hooks" ]
}

@test "a foreign absolute hooksPath is left alone with a WARNING" {
    mkdir -p "$BASE/elsewhere/hooks"
    git -C "$MAIN" config --local core.hooksPath "$BASE/elsewhere/hooks"
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARNING: core.hooksPath is '$BASE/elsewhere/hooks'"* ]]
    [ "$(hooks_path --local)" = "$BASE/elsewhere/hooks" ]
}

@test "an unset hooksPath is still set to the relative form" {
    run install_hooks "$WT"
    [ "$status" -eq 0 ]
    [[ "$output" == *"core.hooksPath set to scripts/git-hooks"* ]]
    [ "$(hooks_path --local)" = "scripts/git-hooks" ]
}

@test "doctor WARNs on an absolute hooksPath and passes a relative one" {
    git -C "$MAIN" config --local core.hooksPath "$MAIN/scripts/git-hooks"
    run bash "$MAIN/scripts/dev/doctor.sh"
    [[ "$output" == *"[WARN] hooksPath"*"local '$MAIN/scripts/git-hooks'"* ]]
    [[ "$output" == *"setup-harness.sh git-hooks"* ]]
    git -C "$MAIN" config --local core.hooksPath scripts/git-hooks
    run bash "$MAIN/scripts/dev/doctor.sh"
    [[ "$output" == *"[PASS] hooksPath"* ]]
    [[ "$output" != *"[WARN] hooksPath"* ]]
}

@test "doctor WARNs on an absolute hooksPath in this worktree's config.worktree" {
    git -C "$MAIN" config --local core.hooksPath scripts/git-hooks
    git -C "$MAIN" config --local extensions.worktreeConfig true
    git -C "$WT" config --worktree core.hooksPath "$MAIN/scripts/git-hooks"
    run bash "$WT/scripts/dev/doctor.sh"
    [[ "$output" == *"[WARN] hooksPath"*"worktree '$MAIN/scripts/git-hooks'"* ]]
}
