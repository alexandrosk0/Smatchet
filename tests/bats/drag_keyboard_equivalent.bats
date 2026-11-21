#!/usr/bin/env bats
# tests/bats/drag_keyboard_equivalent.bats
# ----------------------------------------------------------------------------
# scripts/dev/test-drag-keyboard-equivalent.sh (rule `drag-without-command`, docs/plans/input-ux-w2-gesture-keys.md):
# every drag source under Source/ names the command that does the same thing from the keyboard. The gate's
# --selftest covers each violation on synthetic trees; these cases pin the real tree green and drive the
# DRAG_KBD_ROOT fixture seam on a copy of the real command and keybinding files, so a rename of the command
# directory or the default-binding table that the gate no longer reads shows up here.
# ----------------------------------------------------------------------------

setup() {
    REPO_ROOT="$(git rev-parse --show-toplevel)"
    GATE="$REPO_ROOT/scripts/dev/test-drag-keyboard-equivalent.sh"
    FIX="$(mktemp -d)"
    mkdir -p "$FIX/Source/Core/src/Commands" "$FIX/Source/Core/src/Config" "$FIX/Source/Core/src/Ui"
    cp "$REPO_ROOT/Source/Core/src/Commands/GestureKeyCommands.cpp" "$FIX/Source/Core/src/Commands/"
    cp "$REPO_ROOT/Source/Core/src/Config/KeybindingsConfig.cpp" "$FIX/Source/Core/src/Config/"
    git init -q "$FIX"
    git -C "$FIX" add -A
}

teardown() { rm -rf "$FIX" 2>/dev/null || true; }

gate() { DRAG_KBD_PENDING="" DRAG_KBD_ROOT="$FIX" bash "$GATE" "$@"; }

put() { printf '%b' "$2" > "$FIX/Source/Core/src/Ui/$1"; git -C "$FIX" add -A; }

@test "--selftest passes" {
    run bash "$GATE" --selftest
    [ "$status" -eq 0 ]
    [[ "$output" == *"--selftest: PASS"* ]]
}

@test "the real tree is green" {
    run bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"PASS"* ]]
}

@test "the real command and binding files resolve a gesture command" {
    put Row.cpp '// keyboard-equivalent: ui.list.move_up\nImGui::BeginDragDropSource();\n'
    run gate
    [ "$status" -eq 0 ]
}

@test "an unmarked drag source fails and names the rule" {
    put Row.cpp 'ImGui::BeginDragDropSource();\n'
    run gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"Row.cpp:1 drag source with no keyboard equivalent"* ]]
    [[ "$output" == *"drag-without-command"* ]]
}

@test "a marker naming a command nobody registers fails" {
    put Row.cpp '// keyboard-equivalent: ui.list.teleport\nImGui::BeginDragDropSource();\n'
    run gate
    [ "$status" -eq 1 ]
    [[ "$output" == *"ui.list.teleport, which no file under"* ]]
}

@test "a pending site passes, and --strict fails it" {
    put Row.cpp 'ImGui::BeginDragDropSource();\n'
    run env DRAG_KBD_PENDING='Source/Core/src/Ui/Row.cpp|waits on a sibling' DRAG_KBD_ROOT="$FIX" bash "$GATE"
    [ "$status" -eq 0 ]
    [[ "$output" == *"pending: waits on a sibling"* ]]
    run env DRAG_KBD_PENDING='Source/Core/src/Ui/Row.cpp|waits on a sibling' DRAG_KBD_ROOT="$FIX" bash "$GATE" --strict
    [ "$status" -eq 1 ]
}

@test "a root that is not a git work tree is an infra error, not a pass" {
    nogit="$(mktemp -d)"
    mkdir -p "$nogit/Source"
    run env DRAG_KBD_ROOT="$nogit" bash "$GATE"
    rm -rf "$nogit"
    [ "$status" -eq 2 ]
}
