#!/usr/bin/env bash
# 85-ui-include-direction.sh — no-ui-include-in-domain rule (sourced by test-lint-rules.sh, not run directly).

# no-ui-include-in-domain — include-direction rule over the DOMAIN subsystems (Source/Core/
# {src,include}/{Tracker,Sync,Persistence,Config}/ + Source/Plugins/Mcp/). A quote-form
# `#include "Ui/..."` in domain code inverts the architecture layer DAG (Ui=6 sits ABOVE every
# domain layer — include_cycle_audit.py's ranking): the domain becomes compile-coupled to the
# render layer, so a Ui refactor rebuilds/breaks Tracker and the domain TUs stop being buildable
# without the Ui surface (the mobile/posix lanes compile CORE_SOURCES standalone). The include-cycle
# gate cannot catch this class — its back-edge check is deliberately header->header ("a .cpp pulls
# in the higher-layer headers it implements against" is normal for e.g. AppController TUs), so a
# domain .cpp -> Ui/ header edge sailed through (the TouchCellEditGesture.h escape: three cell-editor
# TUs included a Ui/ header for a layer-neutral gesture gate). This rule closes exactly that
# direction where it is never legitimate. Shared layer-neutral helpers belong at the include/ root
# (leaf/utility rank), not under Ui/.
#
# Commands/ is intentionally NOT in scope: its Scenarios/ TUs drive UI-level machinery on the UI
# thread by contract (MobileTextureGuardScenario -> SmatchetImGuiTextureGuardRuntime.h) and
# AppViewCommands.cpp consumes the Ui view-visibility seam — sanctioned couplings that would need
# grandfathering; graduate Commands/ in once those seams are inverted. The domain dirs in scope are
# Ui-clean today, so any hit is a regression (absolute-0, no grandfathering — same model as no-glfw
# / ui-request-flag-off-thread). A SMATCHET_DEVIATION(rule=no-ui-include-in-domain; ...) on the
# nearest non-blank line above the include escapes.
UIINCLUDE_RULES=(no-ui-include-in-domain)

scan_ui_include_direction_file() {
    # $1 = a first-party C++ file (.cpp/.h/.hpp). Emits `no-ui-include-in-domain\t<f>:<line>` per
    # quote-form `#include "Ui/..."` with no suppressing deviation. Path-agnostic (the domain-dir
    # scoping lives in compute_ui_include_direction_violations; --scan-file / selftest fixtures
    # exercise this directly).
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp|*.h|*.hpp) ;; *) return 0 ;; esac
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i j
    for ((i = 0; i < n; i++)); do
        # Quote-form Ui/ include only (`#include "Ui/..."`); a comment mentioning the path or an
        # angle-bracket include never matches.
        [[ "${lines[$i]}" =~ ^[[:space:]]*#[[:space:]]*include[[:space:]]*\"Ui/ ]] || continue
        # Nearest non-blank line ABOVE the include may carry the suppressing deviation (mirrors the
        # include-cycle / dup nearest-non-blank-above grammar).
        local dev=0
        for ((j = i - 1; j >= 0; j--)); do
            [[ "${lines[$j]}" =~ ^[[:space:]]*$ ]] && continue
            case "${lines[$j]}" in *'SMATCHET_DEVIATION(rule=no-ui-include-in-domain'*) dev=1 ;; esac
            break
        done
        if [ "$dev" -eq 0 ]; then
            printf 'no-ui-include-in-domain\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_ui_include_direction_violations() {
    # Domain scope = the strict-zone subsystems MINUS Commands/ (see the header comment), src +
    # include mirrors + the Mcp plugin. Ui/ itself, AppController TUs, and the root src/include
    # leaves are out of scope (legitimate or grandfathered Ui consumers live there).
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/src/Tracker/**' 'Source/Core/include/Tracker/**' \
            'Source/Core/src/Sync/**' 'Source/Core/include/Sync/**' \
            'Source/Core/src/Persistence/**' 'Source/Core/include/Persistence/**' \
            'Source/Core/src/Config/**' 'Source/Core/include/Config/**' \
            'Source/Plugins/Mcp/**' \
            2>/dev/null \
            | grep -E '\.(cpp|h|hpp)$' | grep -vE '(^|/)ThirdParty/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_ui_include_direction_file "$f"; done
}
