#!/usr/bin/env bash
# test-drag-keyboard-equivalent.sh — rule `drag-without-command`: every drag source in first-party C++ names
# the command that does the same thing from the keyboard.
# ----------------------------------------------------------------------------
# Rule (docs/audits/INPUT_UX_AUDIT.md § 3, "Pointer gestures"): every drag gesture has a registered command with
# a rebindable key, and every popup list is navigable with arrows and Enter. This script is the "list the drag
# sources without a matching command" check of audit § 4.6 (docs/plans/input-ux-w2-gesture-keys.md).
#
# A drag source is a line of code under Source/ (ThirdParty excluded) holding one of
#   BeginDragDropSource(   IsMouseDragging(   GetMouseDragDelta(   ImGuiTableFlags_Reorderable
# Comment-only lines never count. Each one needs, on its own line or within the three lines above it, ONE of:
#   // keyboard-equivalent: <command.id>[, <command.id>...]
#       every id must be registered (its quoted id appears in a file under Source/Core/src/Commands/) and bound
#       by default (its quoted id appears in Source/Core/src/Config/KeybindingsConfig.cpp);
#   // keyboard-equivalent: native(<why>)
#       the gesture's keyboard path is ImGui's own, or the drag test is not a gesture; <why> says so;
#   SMATCHET_DEVIATION(rule=drag-without-command; reason=...; owner=...; revisit=...)
#       a genuinely mouse-only drag, with a dated revisit.
#
# Pending sites: a site in a file another open plan holds is listed in PENDING below with the follow-up it waits
# on. It prints a WARN and does not fail, so the gate can land before the held files are free. `--strict` makes
# every WARN a failure; the follow-up flips it on when the last pending site is marked.
#
# Modes:
#   (no args) | --check   check the repo (or $DRAG_KBD_ROOT, a git work tree).
#   --strict              as --check, with pending sites failing too.
#   --selftest            build fixture trees under a temp root and assert each violation FAILs and each clean
#                         form PASSes.
#
# Fail-CLOSED: a tree with no Source/ directory, or no registered command ids to check against, is an infra
# error (exit 2), never a silent pass.
#
# Exit: 0 clean · 1 violation · 2 infra error.
# selftest: asserts-failure
# ----------------------------------------------------------------------------
set -uo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/$(basename "${BASH_SOURCE[0]}")"
PATTERN='BeginDragDropSource\(|IsMouseDragging\(|GetMouseDragDelta\(|ImGuiTableFlags_Reorderable'
CONTEXT_LINES=3
COMMANDS_DIR="Source/Core/src/Commands"
BINDINGS_FILE="Source/Core/src/Config/KeybindingsConfig.cpp"
MARKER_RE='keyboard-equivalent:'
DEVIATION_RE='SMATCHET_DEVIATION\(rule=drag-without-command;[^)]*reason='
ID_RE='^[a-z][a-z0-9_]*(\.[a-z0-9_]+)+$'

# Sites whose files another open plan holds. "<path>|<what waits>". Overridable for the selftest.
DEFAULT_PENDING='Source/Core/src/Ui/SmatchetViewsDashboardUi_widgets.cpp|views-dashboard row reorder: rebind Alt+Up/Down to ui.list.* (shared-dropdown-recents lock)
Source/Core/src/Ui/SmatchetGridColumnInteraction.cpp|grid header drag: grid.column.move_left/right commands (bulk-edit-rows lock)
Source/Core/src/Ui/SmatchetActiveProjectGridTable.cpp|grid columns reorderable: grid.column.* commands (bulk-edit-rows lock)'

pending_reason() { # <path>  -> prints the reason when the path is pending
    local want="$1" line
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        [ "${line%%|*}" = "$want" ] && { printf '%s\n' "${line#*|}"; return 0; }
    done <<< "${DRAG_KBD_PENDING-$DEFAULT_PENDING}"
    return 1
}

trim() { local s="$1"; s="${s#"${s%%[![:space:]]*}"}"; printf '%s' "${s%"${s##*[![:space:]]}"}"; }

# id_registered <root> <id> — the quoted id appears under the commands directory.
id_registered() { git -C "$1" grep -qF --untracked -e "\"$2\"" -- "$COMMANDS_DIR" 2>/dev/null; }
# id_bound <root> <id> — the quoted id appears in the default keybinding table.
id_bound() { git -C "$1" grep -qF --untracked -e "\"$2\"" -- "$BINDINGS_FILE" 2>/dev/null; }

# check_marker <root> <path> <line> <marker-text> — prints a problem, or nothing when the marker is sound.
check_marker() {
    local root="$1" rest id ids
    rest="$(trim "${4#*"$MARKER_RE"}")"
    # Strip a trailing comment terminator or text after the list.
    rest="${rest%%\*/*}"
    if [[ "$rest" == native\(* && "$rest" != *\)* ]]; then
        printf 'native( is not closed on the marker line: keep the reason short enough that clang-format does not wrap it'
        return 0
    fi
    if [[ "$rest" == native\(*\)* ]]; then
        local why="${rest#native(}"; why="$(trim "${why%%)*}")"
        [ -n "$why" ] && return 0
        printf 'native() needs a reason'
        return 0
    fi
    IFS=',' read -r -a ids <<< "$rest"
    [ "${#ids[@]}" -gt 0 ] || { printf 'names no command'; return 0; }
    for id in "${ids[@]}"; do
        id="$(trim "$id")"
        if ! [[ "$id" =~ $ID_RE ]]; then printf 'is not a command id: "%s"' "$id"; return 0; fi
        if ! id_registered "$root" "$id"; then printf 'names %s, which no file under %s registers' "$id" "$COMMANDS_DIR"; return 0; fi
        if ! id_bound "$root" "$id"; then printf 'names %s, which %s does not bind by default' "$id" "$BINDINGS_FILE"; return 0; fi
    done
    return 0
}

check_root() {
    local root="$1" bad=0 warn=0 checked=0 hit path line text from ctx mline problem why
    [ -d "$root/Source" ] || { echo "test-drag-keyboard-equivalent: no Source/ under $root" >&2; return 2; }
    git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1 || {
        echo "test-drag-keyboard-equivalent: $root is not a git work tree" >&2; return 2; }
    if ! git -C "$root" grep -qE --untracked -e '"[a-z][a-z0-9_]*\.[a-z0-9_.]+"' -- "$COMMANDS_DIR" 2>/dev/null; then
        echo "test-drag-keyboard-equivalent: no command ids found under $COMMANDS_DIR — fail-closed" >&2
        return 2
    fi

    while IFS= read -r hit; do
        [ -n "$hit" ] || continue
        path="${hit%%:*}"; hit="${hit#*:}"; line="${hit%%:*}"; text="${hit#*:}"
        case "$text" in
            [[:space:]]*) text="$(trim "$text")" ;;
        esac
        case "$text" in //*|\**|/\**) continue ;; esac # a comment mentions the call; it is not one
        checked=$((checked + 1))
        from=$((line > CONTEXT_LINES ? line - CONTEXT_LINES : 1))
        ctx="$(sed -n "${from},${line}p" "$root/$path")"
        mline="$(printf '%s\n' "$ctx" | grep -F "$MARKER_RE" | tail -n 1)"
        if [ -n "$mline" ]; then
            problem="$(check_marker "$root" "$path" "$line" "$mline")"
            if [ -n "$problem" ]; then
                echo "  FAIL  $path:$line keyboard-equivalent marker $problem" >&2
                bad=1
            fi
            continue
        fi
        if printf '%s\n' "$ctx" | grep -qE "$DEVIATION_RE"; then
            continue
        fi
        if why="$(pending_reason "$path")"; then
            echo "  WARN  $path:$line drag source with no keyboard equivalent yet — pending: $why"
            warn=$((warn + 1))
            continue
        fi
        echo "  FAIL  $path:$line drag source with no keyboard equivalent: add a command and a 'keyboard-equivalent: <id>' comment above it (or 'native(<why>)', or a drag-without-command deviation)" >&2
        bad=1
    done < <(git -C "$root" grep -nE --untracked -e "$PATTERN" -- 'Source/*.cpp' 'Source/*.h' 'Source/*.hpp' \
                 ':(exclude)Source/**/ThirdParty/**' ':(exclude)Source/ThirdParty/**' 2>/dev/null)

    echo "  scanned $checked drag source(s), $warn pending"
    [ "$bad" -eq 0 ] || return 1
    if [ "$warn" -gt 0 ] && [ "${STRICT:-0}" = 1 ]; then
        echo "  FAIL  --strict: $warn pending drag source(s) remain" >&2
        return 1
    fi
    return 0
}

if [ "${1:-}" = "--selftest" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    fail=0
    # A clean tree: one registered, bound command and one source file that points at it.
    fixture() { # <dir>
        local d="$1"
        mkdir -p "$d/Source/Core/src/Commands" "$d/Source/Core/src/Config" "$d/Source/Core/src/Ui" "$d/Source/Core/ThirdParty/x"
        printf 'c.Name = "ui.list.move_up";\nc.Name = "view.panel.grow";\n' > "$d/Source/Core/src/Commands/Cmds.cpp"
        printf 'MakeBinding("Alt+Up", "ui.list.move_up", "{}");\n' > "$d/$BINDINGS_FILE"
        printf '// keyboard-equivalent: ui.list.move_up\nif (ImGui::BeginDragDropSource()) {}\n' > "$d/Source/Core/src/Ui/A.cpp"
        git -C "$d" init -q
        git -C "$d" add -A
    }
    expect() { # <want-rc> <label> <dir> [<output-fragment>] [<strict>]
        local rc=0 out
        out="$(DRAG_KBD_ROOT="$3" DRAG_KBD_PENDING="${PENDING_OVERRIDE-}" STRICT="${5:-0}" bash "$SELF" 2>&1)" || rc=$?
        if [ "$rc" -ne "$1" ]; then
            echo "  FAIL [want $1 got $rc] $2"; echo "$out" | sed 's/^/        /'; fail=1
        elif [ -n "${4:-}" ] && [[ "$out" != *"$4"* ]]; then
            echo "  FAIL [$1 for the wrong reason, wanted '$4'] $2"; echo "$out" | sed 's/^/        /'; fail=1
        else
            echo "  ok   [$1] $2"
        fi
    }
    n=0
    new_case() { n=$((n + 1)); d="$tmp/$n"; fixture "$d"; }
    put() { printf '%b' "$2" > "$d/Source/Core/src/Ui/$1"; git -C "$d" add -A; }

    new_case; expect 0 "a marked drag source passes" "$d" "scanned 1 drag source"
    new_case; put B.cpp 'if (ImGui::IsMouseDragging(0)) {}\n'
    expect 1 "a drag source with no marker fails" "$d" "B.cpp:1 drag source with no keyboard equivalent"
    new_case; put B.cpp 'ImGui::GetMouseDragDelta(0);\n'
    expect 1 "GetMouseDragDelta is a drag source" "$d" "B.cpp:1 drag source"
    new_case; put B.cpp 'flags |= ImGuiTableFlags_Reorderable;\n'
    expect 1 "a reorderable table is a drag source" "$d" "B.cpp:1 drag source"
    new_case; put B.cpp '// IsMouseDragging( in a comment\n * BeginDragDropSource( in a block comment\n'
    expect 0 "a comment that names the call is not a drag source" "$d" "scanned 1 drag source"
    new_case; put B.cpp '// keyboard-equivalent: ui.list.nope\nImGui::IsMouseDragging(0);\n'
    expect 1 "a marker naming an unregistered command fails" "$d" "ui.list.nope, which no file under"
    new_case; printf 'c.Name = "ui.list.nope";\n' >> "$d/Source/Core/src/Commands/Cmds.cpp"; put B.cpp '// keyboard-equivalent: ui.list.nope\nImGui::IsMouseDragging(0);\n'
    expect 1 "a marker naming an unbound command fails" "$d" "does not bind by default"
    new_case; put B.cpp '// keyboard-equivalent: not a command\nImGui::IsMouseDragging(0);\n'
    expect 1 "a marker that is not a command id fails" "$d" "is not a command id"
    new_case; put B.cpp '// keyboard-equivalent: native()\nImGui::IsMouseDragging(0);\n'
    expect 1 "native() with no reason fails" "$d" "native() needs a reason"
    new_case; put B.cpp '// keyboard-equivalent: native(a reason that clang-format wrapped\n// onto a second line)\nImGui::IsMouseDragging(0);\n'
    expect 1 "a native() marker wrapped onto a second line fails with the reason" "$d" "native( is not closed on the marker line"
    new_case; put B.cpp '// keyboard-equivalent: native(ImGui nav moves between the buttons)\nImGui::IsMouseDragging(0);\n'
    expect 0 "native(<why>) passes" "$d" "scanned 2 drag source"
    new_case; put B.cpp '// keyboard-equivalent: ui.list.move_up, view.panel.grow\nImGui::IsMouseDragging(0);\n'
    expect 1 "a list with one unbound id fails" "$d" "view.panel.grow, which"
    new_case; printf 'MakeBinding("Ctrl+Alt+PageUp", "view.panel.grow", "{}");\n' >> "$d/$BINDINGS_FILE"
    put B.cpp '// keyboard-equivalent: ui.list.move_up, view.panel.grow\nImGui::IsMouseDragging(0);\n'
    expect 0 "a list of registered, bound ids passes" "$d" "scanned 2 drag source"
    new_case; put B.cpp '// SMATCHET_DEVIATION(rule=drag-without-command; reason=mouse-only paint; owner=ui; revisit=2099-01-01)\nImGui::IsMouseDragging(0);\n'
    expect 0 "a drag-without-command deviation passes" "$d" "scanned 2 drag source"
    new_case; put B.cpp '// keyboard-equivalent: ui.list.move_up\n\n\n\nImGui::IsMouseDragging(0);\n'
    expect 1 "a marker more than three lines above does not count" "$d" "B.cpp:5 drag source"
    new_case; put B.cpp '// keyboard-equivalent: ui.list.move_up\n\nImGui::IsMouseDragging(0);\n'
    expect 0 "a marker within three lines counts" "$d" "scanned 2 drag source"
    new_case; mkdir -p "$d/Source/Core/ThirdParty/x"; printf 'ImGui::IsMouseDragging(0);\n' > "$d/Source/Core/ThirdParty/x/T.cpp"; git -C "$d" add -A
    expect 0 "ThirdParty is not scanned" "$d" "scanned 1 drag source"
    new_case; put B.cpp 'ImGui::IsMouseDragging(0);\n'; PENDING_OVERRIDE='Source/Core/src/Ui/B.cpp|waits on a sibling plan'
    expect 0 "a pending site warns and passes" "$d" "pending: waits on a sibling plan"
    expect 1 "--strict fails a pending site" "$d" "--strict: 1 pending" 1
    PENDING_OVERRIDE=''
    new_case; rm -rf "$d/Source"; expect 2 "a tree with no Source/ is an infra error" "$d" "no Source/"
    new_case; rm -rf "$d/Source/Core/src/Commands"; mkdir -p "$d/Source/Core/src/Commands"; git -C "$d" add -A
    expect 2 "no registered command ids is an infra error" "$d" "fail-closed"

    [ "$fail" -eq 0 ] && { echo "test-drag-keyboard-equivalent --selftest: PASS"; echo "Passed: 1  Failed: 0"; exit 0; }
    echo "test-drag-keyboard-equivalent --selftest: FAIL"; echo "Passed: 0  Failed: 1"; exit 1
fi

STRICT="${STRICT:-0}"
case "${1:---check}" in
    --check) ;;
    --strict) STRICT=1 ;;
    *) echo "usage: $0 [--check|--strict] | --selftest" >&2; exit 2 ;;
esac

ROOT="${DRAG_KBD_ROOT:-$(git rev-parse --show-toplevel 2>/dev/null)}" || {
    echo "test-drag-keyboard-equivalent: not inside a git work tree" >&2; exit 2; }
[ -n "$ROOT" ] || { echo "test-drag-keyboard-equivalent: not inside a git work tree" >&2; exit 2; }

export STRICT
check_root "$ROOT"
rc=$?
case "$rc" in
    0) echo "test-drag-keyboard-equivalent: PASS — every drag source names a keyboard equivalent (or is pending)."
       echo "Passed: 1  Failed: 0"; exit 0 ;;
    1) echo "test-drag-keyboard-equivalent: FAIL — see above (rule drag-without-command)." >&2
       echo "Passed: 0  Failed: 1"; exit 1 ;;
    *) echo "Passed: 0  Failed: 1"; exit 2 ;;
esac
