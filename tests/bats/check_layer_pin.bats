#!/usr/bin/env bats
# tests/bats/check_layer_pin.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/check-layer-pin.sh — the gate that the agent-layer/
# gitlink is a commit on the layer's develop.
#
# Layer PRs squash-merge, so a host bump that pins a layer branch commit points
# at history develop never contains. Each test builds a throwaway layer (a bare
# repo standing in for GitHub, plus a work clone that pushes to it) and a host
# repo that mounts it as agent-layer/, then runs the gate from the host.
#
# Exit contract pinned here: 0 pin on develop · 1 pin off develop (fail-closed)
# · 2 cannot verify (no gitlink, mount not checked out, fetch failure).
#
# Requires: bash, git, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/scripts/dev/check-layer-pin.sh"
    WORK="$(mktemp -d)"
    # Isolate from the developer's git config; file:// submodule clones need the
    # protocol allowed explicitly since git 2.38.1. GIT_CONFIG_GLOBAL overrides
    # $HOME/.gitconfig on git 2.32+, so it is pinned inside the scratch HOME too,
    # or the `config --global` writes below would land in the caller's real config.
    export HOME="$WORK/home"
    export GIT_CONFIG_GLOBAL="$HOME/.gitconfig"
    export GIT_CONFIG_NOSYSTEM=1
    unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GITHUB_ACTIONS
    unset SMATCHET_LAYER_PIN_PATH SMATCHET_LAYER_PIN_BRANCH SMATCHET_LAYER_PIN_REV SMATCHET_LAYER_PIN_DEPTH
    mkdir -p "$HOME"
    git config --global user.name "bats"
    git config --global user.email "bats@example.invalid"
    git config --global init.defaultBranch develop
    git config --global protocol.file.allow always
    git config --global advice.detachedHead false

    LAYER_BARE="$WORK/layer.git"
    LAYER_WORK="$WORK/layer-work"
    HOST="$WORK/host"
    git init --quiet --bare "$LAYER_BARE"
    git init --quiet "$LAYER_WORK"
    git -C "$LAYER_WORK" remote add origin "$LAYER_BARE"
    layer_commit "base"
    layer_commit "second"
    git -C "$LAYER_WORK" push --quiet origin develop
}

teardown() {
    rm -rf "$WORK"
}

# layer_commit <msg> — one commit on the layer work clone's current branch.
layer_commit() {
    printf '%s\n' "$1" >"$LAYER_WORK/$1.txt"
    git -C "$LAYER_WORK" add "$1.txt"
    git -C "$LAYER_WORK" commit --quiet -m "$1"
}

# host_pin <sha> — a host repo whose agent-layer/ gitlink is <sha>, committed.
host_pin() {
    local sha="$1"
    if [ ! -d "$HOST/.git" ]; then
        git init --quiet "$HOST"
        printf 'host\n' >"$HOST/README.md"
        git -C "$HOST" add README.md
        git -C "$HOST" commit --quiet -m "host base"
        git -C "$HOST" submodule --quiet add "$LAYER_BARE" agent-layer
    fi
    git -C "$HOST/agent-layer" fetch --quiet origin "+refs/heads/*:refs/remotes/origin/*"
    git -C "$HOST/agent-layer" checkout --quiet "$sha"
    git -C "$HOST" add agent-layer
    git -C "$HOST" commit --quiet -m "pin agent-layer $sha"
}

run_gate() {
    run bash -c 'cd "$1" && bash "$2"' _ "$HOST" "$GATE"
}

@test "the suite's git config writes land in the scratch HOME, not the caller's config" {
    run git config --global --show-origin --get user.name
    [ "$status" -eq 0 ]
    [[ "$output" == "file:$WORK/home/.gitconfig"* ]]
}

@test "a pin at the develop tip passes" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    run_gate
    [ "$status" -eq 0 ]
    [[ "$output" == *"OK"* ]]
}

@test "a pin develop has moved past still passes (an ancestor of develop)" {
    local old
    old="$(git -C "$LAYER_WORK" rev-parse HEAD~1)"
    layer_commit "third"
    git -C "$LAYER_WORK" push --quiet origin develop
    host_pin "$old"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a pin on a layer branch fails with the re-pin message" {
    git -C "$LAYER_WORK" checkout --quiet -b feature
    layer_commit "feature-work"
    git -C "$LAYER_WORK" push --quiet origin feature
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"not on the layer develop"* ]]
    [[ "$output" == *"re-pin agent-layer to the layer develop squash commit after the layer PR merges"* ]]
}

@test "the branch commit still fails after its PR squash-merges into develop" {
    git -C "$LAYER_WORK" checkout --quiet -b feature
    layer_commit "feature-work"
    git -C "$LAYER_WORK" push --quiet origin feature
    local branch_sha
    branch_sha="$(git -C "$LAYER_WORK" rev-parse HEAD)"
    git -C "$LAYER_WORK" checkout --quiet develop
    git -C "$LAYER_WORK" merge --quiet --squash feature
    git -C "$LAYER_WORK" commit --quiet -m "feature (#1)"
    git -C "$LAYER_WORK" push --quiet origin develop
    host_pin "$branch_sha"
    run_gate
    [ "$status" -eq 1 ]
    # ...and re-pinning to the squash is what makes it pass.
    host_pin "$(git -C "$LAYER_WORK" rev-parse develop)"
    run_gate
    [ "$status" -eq 0 ]
}

@test "a pin the mount does not hold, and develop does not contain, fails (exit 1, not 2)" {
    git -C "$LAYER_WORK" checkout --quiet -b feature
    layer_commit "feature-work"
    git -C "$LAYER_WORK" push --quiet origin feature
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    # A fresh mount that fetched only develop: the pinned commit is not in it.
    rm -rf "$HOST/agent-layer" "$HOST/.git/modules/agent-layer"
    git clone --quiet --single-branch --branch develop "$LAYER_BARE" "$HOST/agent-layer"
    run_gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"re-pin"* ]]
}

@test "a shallow mount is deepened far enough to find an older develop pin" {
    local old
    old="$(git -C "$LAYER_WORK" rev-parse HEAD~1)"
    layer_commit "third"
    layer_commit "fourth"
    git -C "$LAYER_WORK" push --quiet origin develop
    host_pin "$old"
    rm -rf "$HOST/agent-layer" "$HOST/.git/modules/agent-layer"
    git clone --quiet --depth 1 --branch develop "file://$LAYER_BARE" "$HOST/agent-layer"
    run_gate
    [ "$status" -eq 0 ]
    # A depth too small to reach the pin fails closed and names the knob.
    rm -rf "$HOST/agent-layer"
    git clone --quiet --depth 1 --branch develop "file://$LAYER_BARE" "$HOST/agent-layer"
    run bash -c 'cd "$1" && SMATCHET_LAYER_PIN_DEPTH=1 bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"SMATCHET_LAYER_PIN_DEPTH"* ]]
}

@test "a shallow mount deeper than the depth knob is not shortened by the fetch" {
    local i pin
    for i in $(seq 3 24); do layer_commit "c$i"; done
    git -C "$LAYER_WORK" push --quiet origin develop
    pin="$(git -C "$LAYER_WORK" rev-parse HEAD~10)"
    host_pin "$pin"
    rm -rf "$HOST/agent-layer" "$HOST/.git/modules/agent-layer"
    git clone --quiet --depth 20 --branch develop "file://$LAYER_BARE" "$HOST/agent-layer"
    [ "$(git -C "$HOST/agent-layer" rev-list --count HEAD)" -eq 20 ]
    # A --depth 5 fetch would cut the mount to 5 commits and lose the pin at develop~10.
    run bash -c 'cd "$1" && SMATCHET_LAYER_PIN_DEPTH=5 bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 0 ]
    [ "$(git -C "$HOST/agent-layer" rev-list --count origin/develop)" -ge 20 ]
}

@test "a full mount is not made shallow by the fetch" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD~1)"
    # A depth smaller than the history would cut a shallow boundary if it applied here.
    run bash -c 'cd "$1" && SMATCHET_LAYER_PIN_DEPTH=1 bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 0 ]
    [ "$(git -C "$HOST/agent-layer" rev-parse --is-shallow-repository)" = "false" ]
}

@test "no agent-layer submodule exits 2" {
    git init --quiet "$HOST"
    printf 'host\n' >"$HOST/README.md"
    git -C "$HOST" add README.md
    git -C "$HOST" commit --quiet -m "host base"
    run_gate
    [ "$status" -eq 2 ]
    [[ "$output" == *"no agent-layer submodule"* ]]
}

@test "an agent-layer path that is a plain directory, not a gitlink, exits 2" {
    git init --quiet "$HOST"
    mkdir -p "$HOST/agent-layer"
    printf 'x\n' >"$HOST/agent-layer/file.txt"
    git -C "$HOST" add agent-layer
    git -C "$HOST" commit --quiet -m "plain dir"
    run_gate
    [ "$status" -eq 2 ]
    [[ "$output" == *"not a submodule gitlink"* ]]
}

@test "a gitlink whose mount is not checked out exits 2" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    git -C "$HOST" submodule --quiet deinit --force agent-layer
    run_gate
    [ "$status" -eq 2 ]
    [[ "$output" == *"not checked out"* ]]
}

@test "a fetch failure exits 2 (fail-closed, distinct from a wrong pin)" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    git -C "$HOST/agent-layer" remote set-url origin "$WORK/does-not-exist.git"
    run_gate
    [ "$status" -eq 2 ]
    [[ "$output" == *"fetching 'develop' from the layer's origin failed"* ]]
}

@test "a layer without the branch exits 2" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    run bash -c 'cd "$1" && SMATCHET_LAYER_PIN_BRANCH=no-such-branch bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 2 ]
}

@test "a non-numeric depth and an unknown argument exit 2" {
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    run bash -c 'cd "$1" && SMATCHET_LAYER_PIN_DEPTH=lots bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 2 ]
    run bash -c 'cd "$1" && bash "$2" --strict' _ "$HOST" "$GATE"
    [ "$status" -eq 2 ]
}

@test "in GitHub Actions a failure also emits an ::error:: annotation" {
    git -C "$LAYER_WORK" checkout --quiet -b feature
    layer_commit "feature-work"
    git -C "$LAYER_WORK" push --quiet origin feature
    host_pin "$(git -C "$LAYER_WORK" rev-parse HEAD)"
    run bash -c 'cd "$1" && GITHUB_ACTIONS=true bash "$2"' _ "$HOST" "$GATE"
    [ "$status" -eq 1 ]
    [[ "$output" == *"::error::"* ]]
}

@test "--help prints the usage header and exits 0" {
    run bash "$GATE" --help
    [ "$status" -eq 0 ]
    [[ "$output" == *"Exit codes:"* ]]
}
