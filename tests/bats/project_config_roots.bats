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
    # ...and every other PC_* a parent's full load exported (test-all.sh sources
    # this script, so PC_PROJECT_NAME and the rest arrive set): a test asserting
    # what a load did NOT set would otherwise read the parent's value.
    local v
    while IFS= read -r v; do unset "$v"; done < <(compgen -e | grep '^PC_' || true)

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

# A minimal but schema-complete config directory. The schema is a stub: the
# no-deps gate reads only its top-level `required` list, and this suite tests root
# RESOLUTION, not the schema's content — which also keeps it runnable in the
# standalone agent layer, which ships no project.config.schema.json.
make_config_dir() {
    local dir="$1"
    mkdir -p "$dir"
    cp "$REPO_ROOT/project.config.json" "$dir/project.config.json"
    printf '{"required": ["project"]}\n' >"$dir/project.config.schema.json"
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
    # AGENT_LAYER_ROOT as given: the flip's CI points the host's scripts at agent-layer/.
    local layer
    layer="$(root_of PC_AGENT_LAYER_ROOT env AGENT_LAYER_ROOT="$TMP")"
    [ "$layer" = "$TMP" ]
    # PROJECT_ROOT naming the host this copy resolves itself (CI's `.`, the
    # standalone layer, a runner exporting its own tree) is taken, silently.
    run bash -c 'PROJECT_ROOT="$REPO_ROOT" bash "$CONFIG_SH" 2>&1 >/dev/null'
    [ "$status" -eq 0 ]
    [ -z "$output" ]
    # Another tree is taken only on the explicit override (a test fixture).
    local project
    project="$(root_of PC_PROJECT_ROOT env SMATCHET_PROJECT_ROOT_OVERRIDE=1 PROJECT_ROOT="$TMP")"
    [ "$project" = "$TMP" ]
}

@test "a caller-set PROJECT_ROOT naming another tree is ignored, with a warning" {
    # The bare name is common, and a stale export from a sibling checkout (which
    # has a project.config.json of its own) would aim every gate at that tree.
    local sibling="$TMP/sibling"
    mkdir -p "$sibling"
    printf '{}\n' > "$sibling/project.config.json"
    run bash -c 'PC_ROOTS_ONLY=1 PROJECT_ROOT="$1" bash "$CONFIG_SH" 2>&1 >/dev/null' _ "$sibling"
    [ "$status" -eq 0 ]
    [[ "$output" == *"WARN"*"ignoring PROJECT_ROOT=$sibling"*"$REPO_ROOT"* ]]
    local project
    project="$(root_of PC_PROJECT_ROOT env PC_ROOTS_ONLY=1 PROJECT_ROOT="$sibling" 2>/dev/null)"
    [ "$project" = "$(cd "$REPO_ROOT" && pwd)" ]
}

@test "PC_SCHEMA_FILE follows the resolved config, not the script's own root" {
    # A config missing a required key next to a schema that demands it must trip
    # the no-deps required-key gate (exit 2). If the schema were still read from
    # the script's root the pairing would be wrong and this would pass silently.
    mkdir -p "$TMP/host"
    printf '{"project": {"name": "x"}}\n' >"$TMP/host/project.config.json"
    printf '{"required": ["project", "vcs"]}\n' >"$TMP/host/project.config.schema.json"
    #
    # The gate's own exit 2 does not reach the caller: the `|| { ... exit 1; }`
    # guard around the python capture rewrites every failure to 1. That is
    # pre-existing behaviour and not this suite's subject, so assert on the
    # message, which is what proves the right schema was read.
    run env SMATCHET_PROJECT_CONFIG="$TMP/host" bash "$CONFIG_SH"
    [ "$status" -ne 0 ]
    [[ "$output" == *"missing required key"* ]]
}

@test "relative caller-set roots are made absolute" {
    # CI sets the pair relative to the workspace (`PROJECT_ROOT: .`); a consumer
    # that cds into one root and reads the other must still name the right tree.
    mkdir -p "$TMP/ws/agent-layer"
    local project layer
    # $TMP/ws is not this copy's host, so the PROJECT_ROOT half needs the override.
    project="$(cd "$TMP/ws" && root_of PC_PROJECT_ROOT env SMATCHET_PROJECT_ROOT_OVERRIDE=1 PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer)"
    layer="$(cd "$TMP/ws" && root_of PC_AGENT_LAYER_ROOT env PROJECT_ROOT=. AGENT_LAYER_ROOT=agent-layer 2>/dev/null)"
    [ "$project" = "$(cd "$TMP/ws" && pwd)" ]
    [ "$layer" = "$(cd "$TMP/ws/agent-layer" && pwd)" ]
}

@test "roots-only: sourcing exports the pair without loading the config" {
    # The flag is EXPORTED, not a prefix assignment: bash scopes `PC_ROOTS_ONLY=1 .`
    # to the builtin, which would clear it whether or not the script does.
    run bash -c 'export PC_ROOTS_ONLY=1; . "$CONFIG_SH" && printf "%s|%s|%s|%s\n" \
        "$PROJECT_ROOT" "$AGENT_LAYER_ROOT" "${PC_PROJECT_NAME:-none}" "${PC_ROOTS_ONLY:-cleared}"'
    [ "$status" -eq 0 ]
    IFS='|' read -r p l name flag <<<"$output"
    [ "$p" = "$(cd "$REPO_ROOT" && pwd)" ]
    [ "$l" = "$p" ]
    [ "$name" = "none" ]
    [ "$flag" = "cleared" ]
}

@test "roots-only: a later full source in the same shell still loads the config" {
    run bash -c 'export PC_ROOTS_ONLY=1; . "$CONFIG_SH" && . "$CONFIG_SH" && printf "%s\n" "${PC_PROJECT_NAME:-none}"'
    [ "$status" -eq 0 ]
    [ "$output" != "none" ]
    [ -n "$output" ]
}

@test "roots-only: needs no config file and follows the same rungs" {
    # Rung 0 names a config that does not exist: a full load fails (tested above),
    # roots-only still resolves PROJECT_ROOT to that file's directory.
    mkdir -p "$TMP/host"
    local project
    project="$(root_of PC_PROJECT_ROOT env PC_ROOTS_ONLY=1 PC_CONFIG_FILE="$TMP/host/project.config.json")"
    [ "$project" = "$(cd "$TMP/host" && pwd)" ]
}

@test "roots-only: a copy inside a submodule resolves the superproject" {
    local layer="$TMP/layer" super="$TMP/super"
    mkdir -p "$layer/scripts/dev"
    cp "$CONFIG_SH" "$layer/scripts/dev/project-config.sh"
    make_config_dir "$layer"
    git -C "$layer" init -q
    git -C "$layer" add -A
    git -C "$layer" -c user.email=t@t -c user.name=t commit -qm init
    make_config_dir "$super"
    git -C "$super" init -q
    git -C "$super" add -A
    git -C "$super" -c user.email=t@t -c user.name=t commit -qm init
    git -C "$super" -c protocol.file.allow=always submodule add -q "$layer" agent-layer

    run bash -c 'cd "$1" && PC_ROOTS_ONLY=1 . scripts/dev/project-config.sh && printf "%s|%s\n" "$PROJECT_ROOT" "$AGENT_LAYER_ROOT"' _ "$super/agent-layer"
    [ "$status" -eq 0 ]
    IFS='|' read -r p l <<<"$output"
    [ "$p" = "$(cd "$super" && pwd)" ]
    [ "$l" = "$(cd "$super/agent-layer" && pwd)" ]
}
