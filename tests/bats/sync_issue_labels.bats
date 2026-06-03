#!/usr/bin/env bats
# tests/bats/sync_issue_labels.bats
# ----------------------------------------------------------------------------
# Bats coverage for agents/scripts/project/sync-issue-labels.sh (issue-triage
# protocol Slice 2). Tests the gh-free surfaces: --check-parity (area:* ↔
# coderabbit-triage map) + manifest well-formedness. The live gh create/update
# path is exercised by the read-only --dry-run against the repo (not mocked here).
#
# Requires: bash, bats, git.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    export REPO_ROOT
    export SCRIPT="$REPO_ROOT/agents/scripts/project/sync-issue-labels.sh"
    export MANIFEST="$REPO_ROOT/agents/scripts/project/issue-labels.manifest"
    WORK="$BATS_TEST_TMPDIR"
    export WORK
}

@test "real manifest passes --check-parity" {
    cd "$REPO_ROOT"
    run bash "$SCRIPT" --check-parity
    [ "$status" -eq 0 ]
    [[ "$output" == *"parity OK"* ]]
}

@test "an area:* with no matching agent fails parity" {
    # Fixture manifest with a bogus area + a map that does NOT mention it.
    printf 'area:nonesuch|1d76db|bogus area\n' > "$WORK/m.manifest"
    printf '| `Source/Foo` | `tracker-backend` |\n' > "$WORK/map.md"
    cd "$REPO_ROOT"
    ISSUE_LABELS_MANIFEST="$WORK/m.manifest" ISSUE_TRIAGE_MAP="$WORK/map.md" \
        run bash "$SCRIPT" --check-parity
    [ "$status" -eq 1 ]
    [[ "$output" == *"PARITY FAIL"* ]]
    [[ "$output" == *"area:nonesuch"* ]]
}

@test "generic areas (ui/build) pass parity without an agent" {
    printf 'area:ui|1d76db|general ui\narea:build|1d76db|build\n' > "$WORK/m.manifest"
    printf '| `Source/Foo` | `tracker-backend` |\n' > "$WORK/map.md"
    cd "$REPO_ROOT"
    ISSUE_LABELS_MANIFEST="$WORK/m.manifest" ISSUE_TRIAGE_MAP="$WORK/map.md" \
        run bash "$SCRIPT" --check-parity
    [ "$status" -eq 0 ]
}

@test "a subsystem area resolves to its agent in the map" {
    printf 'area:tracker-backend|1d76db|t\n' > "$WORK/m.manifest"
    printf 'noise\n| `x` | `tracker-backend` |\nnoise\n' > "$WORK/map.md"
    cd "$REPO_ROOT"
    ISSUE_LABELS_MANIFEST="$WORK/m.manifest" ISSUE_TRIAGE_MAP="$WORK/map.md" \
        run bash "$SCRIPT" --check-parity
    [ "$status" -eq 0 ]
}

@test "manifest is well-formed (every non-comment line is name|color|desc, 6-hex color)" {
    run bash -c "grep -vE '^[[:space:]]*(#|\$)' '$MANIFEST' | awk -F'|' '{ if (NF<3) exit 1; if (\$2 !~ /^[0-9a-fA-F]{6}\$/) exit 2 }'"
    [ "$status" -eq 0 ]
}

@test "bad arg exits 2" {
    cd "$REPO_ROOT"
    run bash "$SCRIPT" --bogus
    [ "$status" -eq 2 ]
}
