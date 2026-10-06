#!/usr/bin/env bats
# Every agent-layer path a host workflow or action calls must exist in the pinned agent-layer/
# mount. A host change that starts calling a new layer script has to ship with the pointer bump
# that brings it: merged without it, the workflow fails on develop (lock cleanup, for one) until
# some later bump lands.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    LAYER="$REPO_ROOT/agent-layer"
}

# The layer-relative paths host workflows and actions reach through $AGENT_LAYER_ROOT.
layer_paths() {
    git -C "$REPO_ROOT" grep -hoE '\$\{?AGENT_LAYER_ROOT\}?/[A-Za-z0-9_./-]+' -- .github/workflows .github/actions |
        sed -E -e 's#^\$\{?AGENT_LAYER_ROOT\}?/##' -e 's#[./]+$##' | sort -u
}

@test "every AGENT_LAYER_ROOT path in host workflows exists in the pinned agent-layer mount" {
    if [ ! -f "$LAYER/scripts/dev/project-config.sh" ]; then
        skip "agent-layer/ is not checked out (git submodule update --init agent-layer)"
    fi
    local missing=() p
    while IFS= read -r p; do
        [ -n "$p" ] || continue
        [ -e "$LAYER/$p" ] || missing+=("$p")
    done < <(layer_paths)
    if [ "${#missing[@]}" -gt 0 ]; then
        printf 'missing in agent-layer/ at %s (bump the pointer with this change):\n' \
            "$(git -C "$LAYER" rev-parse --short HEAD)"
        printf '  %s\n' "${missing[@]}"
        return 1
    fi
}

@test "the path extraction finds the layer calls it guards (non-vacuous)" {
    run layer_paths
    [ "$status" -eq 0 ]
    [[ "$output" == *"agents/scripts/core/"* ]]
}
