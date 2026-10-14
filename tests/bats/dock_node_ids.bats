#!/usr/bin/env bats
#
# dock_node_ids.bats — every dock-slot constant names a node the default layout cuts.
#
# Source/Core/include/Ui/SmatchetDockNodeIds.h declares the dockspace node ids windows dock
# into (`constexpr ImGuiID k<Name> = 0x...u;`). Docking to an id the layout never cut makes
# ImGui mint an orphan root node: the window LOOKS docked but owns no slot, and the illusion
# breaks only when the user drags a splitter. So each constant must appear as a
# `DockNode`/`DockSpace` `ID=0x...` in the embedded default ini (the `[Docking][Data]` block
# in Source/Core/src/Config/ConfigManager.cpp). Build-free: a text check over the two files,
# no ImGui context. Backlog: categories/debt/2026-08-07-dock-node-id-slot-liveness-followups.md.
#
# The constants are read from the HEADER, not the `kEntries` table in SmatchetDockNodeIds.cpp:
# that table never references kSecondarySideBar, so a gate driven off it stays green through
# exactly the orphan this gate exists for.
#
# Known orphans are allow-listed by name and must stay orphans: once the ini cuts one, its
# allow-list entry goes stale and the gate fails until the entry is dropped.

# kSecondarySideBar (0x10): no DockBuilder call and no default-ini node creates it, so the
# assistant's "Right ->" swap falls back to the side that exists. Whether to cut a real node
# or delete the constant + feature is the open product decision in the debt entry above.
KNOWN_ORPHANS="kSecondarySideBar"

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    HEADER="${REPO_ROOT}/Source/Core/include/Ui/SmatchetDockNodeIds.h"
    INI_SRC="${REPO_ROOT}/Source/Core/src/Config/ConfigManager.cpp"
}

is_known_orphan() {
    local name="$1" orphan
    for orphan in $KNOWN_ORPHANS; do
        [ "$orphan" = "$name" ] && return 0
    done
    return 1
}

# check_dock_ids <header> <ini-source>
# One line per constant: `ok`, `known orphan`, `MISSING`, or `STALE allow-list`.
# Exit 0 clean · 1 a constant has no node (or a stale allow-list entry) · 2 parse rot
# (no constants / no ini ids, or a declared constant the parser could not read).
check_dock_ids() {
    local header="$1" ini="$2"
    local consts declared parsed ids
    consts="$(sed -nE 's/^[[:space:]]*constexpr[[:space:]]+ImGuiID[[:space:]]+(k[A-Za-z0-9_]+)[[:space:]]*=[[:space:]]*0[xX]([0-9A-Fa-f]+)[uU]?[[:space:]]*;.*$/\1 \2/p' "$header")"
    declared="$(grep -cE '^[[:space:]]*constexpr[[:space:]]+ImGuiID[[:space:]]+k' "$header" || true)"
    parsed="$(printf '%s\n' "$consts" | grep -c . || true)"
    if [ "$parsed" -eq 0 ]; then
        echo "parse rot: no 'constexpr ImGuiID k* = 0x...;' constants in $header"
        return 2
    fi
    if [ "$parsed" -ne "$declared" ]; then
        echo "parse rot: $declared constexpr ImGuiID constants declared, $parsed read as hex literals"
        return 2
    fi
    ids="$(grep -oE '(DockNode|DockSpace)[[:space:]]+ID=0[xX][0-9A-Fa-f]+' "$ini" | sed -E 's/.*ID=0[xX]//' || true)"
    if [ -z "$ids" ]; then
        echo "parse rot: no DockNode/DockSpace ID=0x... in $ini"
        return 2
    fi

    local fail=0 name hex id present
    while read -r name hex; do
        present=0
        for id in $ids; do
            if [ $((16#$hex)) -eq $((16#$id)) ]; then
                present=1
                break
            fi
        done
        if [ "$present" -eq 1 ] && is_known_orphan "$name"; then
            echo "STALE allow-list: $name (0x$hex) is now cut by the ini — drop it from KNOWN_ORPHANS"
            fail=1
        elif [ "$present" -eq 1 ]; then
            echo "ok $name (0x$hex)"
        elif is_known_orphan "$name"; then
            echo "known orphan $name (0x$hex)"
        else
            echo "MISSING $name (0x$hex): no DockNode ID in the default ini — docking to it mints an orphan node"
            fail=1
        fi
    done <<<"$consts"
    return "$fail"
}

write_header() { # $1 = file, rest = `kName 0xHEX` pairs
    local file="$1"
    shift
    printf '#pragma once\nnamespace SmatchetDockNodeIds {\n' >"$file"
    while [ "$#" -gt 1 ]; do
        printf 'constexpr ImGuiID %s = %su;\n' "$1" "$2" >>"$file"
        shift 2
    done
    printf '}\n' >>"$file"
}

write_ini() { # $1 = file, rest = hex ids
    local file="$1" id
    shift
    printf 'const char* kIni =\n    "[Docking][Data]\\n"\n' >"$file"
    printf '    "DockSpace         ID=0x08BD597D Window=0x1BBC0F80 Split=Y\\n"\n' >>"$file"
    for id in "$@"; do
        printf '    "  DockNode        ID=%s Parent=0x08BD597D SizeRef=10,10\\n"\n' "$id" >>"$file"
    done
    printf '    ;\n' >>"$file"
}

@test "every dock-slot constant in the real header is cut by the embedded default ini (or allow-listed)" {
    run check_dock_ids "$HEADER" "$INI_SRC"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok kCentralNode"* ]]
    [[ "$output" == *"ok kBottomPanel"* ]]
    [[ "$output" == *"known orphan kSecondarySideBar"* ]]
}

@test "goes red on a fixture header constant the ini never cuts" {
    write_header "$BATS_TEST_TMPDIR/ids.h" kCentralNode 0x00000002 kMissingSlot 0x00000077
    write_ini "$BATS_TEST_TMPDIR/ini.cpp" 0x00000002 0x00000004
    run check_dock_ids "$BATS_TEST_TMPDIR/ids.h" "$BATS_TEST_TMPDIR/ini.cpp"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" == *"MISSING kMissingSlot (0x00000077)"* ]]
    [[ "$output" == *"ok kCentralNode"* ]]
}

@test "goes red when an allow-listed orphan is now cut (stale allow-list entry)" {
    write_header "$BATS_TEST_TMPDIR/ids.h" kCentralNode 0x00000002 kSecondarySideBar 0x00000010
    write_ini "$BATS_TEST_TMPDIR/ini.cpp" 0x00000002 0x00000010
    run check_dock_ids "$BATS_TEST_TMPDIR/ids.h" "$BATS_TEST_TMPDIR/ini.cpp"
    echo "$output"
    [ "$status" -eq 1 ]
    [[ "$output" == *"STALE allow-list: kSecondarySideBar"* ]]
}

@test "compares ids numerically, not as text (0x0Au matches ID=0x0000000A)" {
    write_header "$BATS_TEST_TMPDIR/ids.h" kBottomPanel 0x0A
    write_ini "$BATS_TEST_TMPDIR/ini.cpp" 0x0000000A
    run check_dock_ids "$BATS_TEST_TMPDIR/ids.h" "$BATS_TEST_TMPDIR/ini.cpp"
    echo "$output"
    [ "$status" -eq 0 ]
    [[ "$output" == *"ok kBottomPanel"* ]]
}

@test "fails closed on parse rot: a constant not written as a hex literal" {
    write_header "$BATS_TEST_TMPDIR/ids.h" kCentralNode 0x00000002
    printf 'constexpr ImGuiID kDecimalSlot = 16;\n' >>"$BATS_TEST_TMPDIR/ids.h"
    write_ini "$BATS_TEST_TMPDIR/ini.cpp" 0x00000002
    run check_dock_ids "$BATS_TEST_TMPDIR/ids.h" "$BATS_TEST_TMPDIR/ini.cpp"
    echo "$output"
    [ "$status" -eq 2 ]
    [[ "$output" == *"parse rot"* ]]
}

@test "fails closed on parse rot: no docking ids in the ini source" {
    write_header "$BATS_TEST_TMPDIR/ids.h" kCentralNode 0x00000002
    printf 'const char* kIni = "[Window][Main]\\n";\n' >"$BATS_TEST_TMPDIR/ini.cpp"
    run check_dock_ids "$BATS_TEST_TMPDIR/ids.h" "$BATS_TEST_TMPDIR/ini.cpp"
    echo "$output"
    [ "$status" -eq 2 ]
}
