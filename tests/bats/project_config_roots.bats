#!/usr/bin/env bats
# tests/bats/project_config_roots.bats
# ----------------------------------------------------------------------------
# Bats tests for scripts/dev/project-config.sh — the dual-root pair
# (PROJECT_ROOT / AGENT_LAYER_ROOT) and the four-rung project.config.json
# resolution order that lets the same file work host-side, from inside a
# submodule, and standalone.
#
# Rungs, most specific first:
#   0  PC_CONFIG_FILE           — an exact file named by the caller
#   1  SMATCHET_PROJECT_CONFIG  — a file or a directory
#   2  the superproject working tree, when this copy lives in a submodule
#   3  this tree's own root
#
# Requires: bash, git, python, bats.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export CONFIG_SH="$REPO_ROOT/scripts/dev/project-config.sh"

    # Every test runs the script in a clean env: an inherited PROJECT_ROOT or
    # SMATCHET_PROJECT_CONFIG from the surrounding session would mask the rung
    # under test.
    unset PC_CONFIG_FILE PC_SCHEMA_FILE SMATCHET_PROJECT_CONFIG
    unset PROJECT_ROOT AGENT_LAYER_ROOT PC_PROJECT_ROOT PC_AGENT_LAYER_ROOT

    TMP="$(mktemp -d)"
    export TMP
}

teardown() {
    [ -n "${TMP:-}" ] && rm -rf "$TMP"
}

# Value of one exported root, as printed by a direct (non-sourced) run.
root_of() {
    local var="$1"; shift
    "$@" bash "$CONFIG_SH" | sed -n "s/^export ${var}=//p" | tr -d "'"
}

# A minimal but schema-complete config directory.
make_config_dir() {
    local dir="$1"
    mkdir -p "$dir"
    cp "$REPO_ROOT/project.config.json" "$dir/project.config.json"
    cp "$REPO_ROOT/project.config.schema.json" "$dir/project.config.schema.json"
}

@test "rung 3: both roots default to the repo root pre-flip" {
    run bash "$CONFIG_SH"
    [ "$status" -eq 0 ]
    local project layer
    project="$(printf '%s\n' "$output" | sed -n "s/^export PC_PROJECT_ROOT=//p" | tr -d "'")"
    layer="$(printf '%s\n' "$output" | sed -n "s/^export PC_AGENT_LAYER_ROOT=//p" | tr -d "'")"
    [ -n "$project" ]
    [ "$project" = "$layer" ]
    [ "$(cd "$project" && pwd)" = "$(cd "$REPO_ROOT" && pwd)" ]
}

@test "rung 3: sourcing exports the bare names as well as the PC_ twins" {
    run bash -c '. "$CONFIG_SH" && printf "%s|%s|%s|%s\n" \
        "$PROJECT_ROOT" "$AGENT_LAYER_ROOT" "$PC_PROJECT_ROOT" "$PC_AGENT_LAYER_ROOT"'
    [ "$status" -eq 0 ]
    IFS='|' read -r p l pp pl <<<"$output"
    [ -n "$p" ]
    [ "$p" = "$pp" ]
    [ "$l" = "$pl" ]
    [ "$p" = "$l" ]
}

@test "rung 1: SMATCHET_PROJECT_CONFIG as a directory moves PROJECT_ROOT only" {
    make_config_dir "$TMP/host"
    local project layer
    project="$(root_of PC_PROJECT_ROOT env SMATCHET_PROJECT_CONFIG="$TMP/host")"
    layer="$(root_of PC_AGENT_LAYER_ROOT env SMATCHET_PROJECT_CONFIG="$TMP/host")"
    [ "$(cd "$project" && pwd)" = "$(cd "$TMP/host" && pwd)" ]
    # The layer root follows the SCRIPT, never the config — that separation is
    # the whole point of the pair.
    [ "$(cd "$layer" && pwd)" = "$(cd "$REPO_ROOT" && pwd)" ]
}

@test "rung 1: SMATCHET_PROJECT_CONFIG as a file resolves to that file's directory" {
    make_config_dir "$TMP/host"
    local project
    project="$(root_of PC_PROJECT_ROOT env SMATCHET_PROJECT_CONFIG="$TMP/host/project.config.json")"
    [ "$(cd "$project" && pwd)" = "$(cd "$TMP/host" && pwd)" ]
}

@test "rung 0: PC_CONFIG_FILE outranks SMATCHET_PROJECT_CONFIG" {
    make_config_dir "$TMP/host"
    local project
    project="$(root_of PC_PROJECT_ROOT env \
        PC_CONFIG_FILE="$REPO_ROOT/project.config.json" \
        SMATCHET_PROJECT_CONFIG="$TMP/host")"
    [ "$(cd "$project" && pwd)" = "$(cd "$REPO_ROOT" && pwd)" ]
}

@test "rung 0: a PC_CONFIG_FILE that does not exist still fails loudly" {
    run env PC_CONFIG_FILE=/nonexistent/project.config.json bash "$CONFIG_SH"
    [ "$status" -eq 1 ]
    [[ "$output" == *"not found"* ]]
}

@test "rung 2: a copy inside a submodule resolves the superproject's config" {
    # Build a throwaway superproject whose submodule carries this script at the
    # same relative path, then run the submodule's copy from inside it.
    local layer="$TMP/layer" super="$TMP/super"
    mkdir -p "$layer/scripts/dev"
    cp "$CONFIG_SH" "$layer/scripts/dev/project-config.sh"
    # The layer standalone-resolves against its own root, so give it a config too;
    # rung 2 must still win over it once the layer is mounted as a submodule.
    make_config_dir "$layer"
    git -C "$layer" init -q
    git -C "$layer" add -A
    git -C "$layer" -c user.email=t@t -c user.name=t commit -qm init

    make_config_dir "$super"
    git -C "$super" init -q
    git -C "$super" add -A
    git -C "$super" -c user.email=t@t -c user.name=t commit -qm init
    git -C "$super" -c protocol.file.allow=always submodule add -q "$layer" agent-layer

    local project layer_root
    project="$(cd "$super/agent-layer" && bash scripts/dev/project-config.sh \
        | sed -n 's/^export PC_PROJECT_ROOT=//p' | tr -d "'")"
    layer_root="$(cd "$super/agent-layer" && bash scripts/dev/project-config.sh \
        | sed -n 's/^export PC_AGENT_LAYER_ROOT=//p' | tr -d "'")"
    [ "$(cd "$project" && pwd)" = "$(cd "$super" && pwd)" ]
    [ "$(cd "$layer_root" && pwd)" = "$(cd "$super/agent-layer" && pwd)" ]
}

@test "caller-set roots are honoured (the flip and standalone CI both rely on this)" {
    local layer
    layer="$(root_of PC_AGENT_LAYER_ROOT env AGENT_LAYER_ROOT="$TMP")"
    [ "$layer" = "$TMP" ]
    local project
    project="$(root_of PC_PROJECT_ROOT env PROJECT_ROOT="$TMP")"
    [ "$project" = "$TMP" ]
}

@test "PC_SCHEMA_FILE follows the resolved config, not the script's own root" {
    # A config missing a required key next to a schema that demands it must trip
    # the no-deps required-key gate (exit 2). If the schema were still read from
    # the script's root the pairing would be wrong and this would pass silently.
    mkdir -p "$TMP/host"
    printf '{"project": {"name": "x"}}\n' >"$TMP/host/project.config.json"
    cp "$REPO_ROOT/project.config.schema.json" "$TMP/host/project.config.schema.json"
    #
    # The gate's own exit 2 does not reach the caller: the `|| { ... exit 1; }`
    # guard around the python capture rewrites every failure to 1. That is
    # pre-existing behaviour and not this suite's subject, so assert on the
    # message, which is what proves the right schema was read.
    run env SMATCHET_PROJECT_CONFIG="$TMP/host" bash "$CONFIG_SH"
    [ "$status" -ne 0 ]
    [[ "$output" == *"missing required key"* ]]
}
