#!/usr/bin/env bats
# Every agent-layer path a host workflow or action calls must exist in the pinned agent-layer/
# mount. A host change that starts calling a new layer script has to ship with the pointer bump
# that brings it: merged without it, the workflow fails on develop (lock cleanup, for one) until
# some later bump lands.

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    LAYER="$REPO_ROOT/agent-layer"
}

# The layer-relative paths in the text on stdin, reached through $AGENT_LAYER_ROOT, ${AGENT_LAYER_ROOT},
# "$AGENT_LAYER_ROOT"/ or ${{ env.AGENT_LAYER_ROOT }}.
extract_layer_paths() {
    grep -oE '(\$\{\{[[:space:]]*env\.AGENT_LAYER_ROOT[[:space:]]*\}\}|\$\{?AGENT_LAYER_ROOT\}?"?)/[A-Za-z0-9_./-]+' |
        sed -E -e 's#^[^/]*/##' -e 's#[./]+$##' | sort -u
}

# The layer-relative paths host workflows and actions reach.
layer_paths() {
    git -C "$REPO_ROOT" grep -h 'AGENT_LAYER_ROOT' -- .github/workflows .github/actions | extract_layer_paths
}

@test "every AGENT_LAYER_ROOT path in host workflows exists in the pinned agent-layer mount" {
    if [ ! -e "$LAYER/.git" ]; then
        # CI checks the submodule out, so a missing mount there is a broken checkout, never a skip.
        [ -z "${CI:-}" ] || { echo "agent-layer/ is not checked out on CI"; return 1; }
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

@test "the path extraction reads every spelling of the layer root" {
    run extract_layer_paths <<'EOF'
run: bash "$AGENT_LAYER_ROOT/agents/scripts/core/a.sh"
run: bash "${AGENT_LAYER_ROOT}/agents/scripts/core/b.sh".
run: bash "$AGENT_LAYER_ROOT"/agents/scripts/core/c.sh
with: { path: ${{ env.AGENT_LAYER_ROOT }}/agents/scripts/core/d.sh }
EOF
    [ "$status" -eq 0 ]
    [ "$output" = "$(printf 'agents/scripts/core/a.sh\nagents/scripts/core/b.sh\nagents/scripts/core/c.sh\nagents/scripts/core/d.sh')" ]
}
