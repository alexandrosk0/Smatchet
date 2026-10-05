#!/usr/bin/env bash
# layer-root.sh — sourced by the harness hooks: where the agent layer lives for a
# project dir.
#
# A hook runs with CLAUDE_PROJECT_DIR set to the host. Before the flip the layer is
# that same tree; after it the layer is the host's agent-layer/ submodule, and a
# hook that joins a layer path onto the project dir finds nothing and fails open
# without a word. The layer is the mount when it is populated, by the marker
# scripts/dev/project-config.sh resolves it by (Phase C row 12), and the project
# dir itself otherwise (before the flip, or the standalone layer repo). Plain tests,
# no subshell, so the per-edit guards can afford it.
#
# hook_layer_root <project dir> — sets HOOK_LAYER (read by the sourcing hook).
# shellcheck disable=SC2034
hook_layer_root() {
    if [ -f "$1/agent-layer/scripts/dev/project-config.sh" ]; then
        HOOK_LAYER="$1/agent-layer"
    else
        HOOK_LAYER="$1"
    fi
}
