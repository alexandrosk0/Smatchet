#!/usr/bin/env bash
# 70-ui-request-flag.sh — ui-request-flag-off-thread rule (sourced by test-lint-rules.sh, not run directly).

# ui-request-flag-off-thread — strict-zone rule over command-dispatch TUs (Source/Core/src/Commands/
# EXCEPT Scenarios/). A write to a g_ui request-flag field (requestWindowResize / requestWindowWidth
# / requestWindowHeight / requestScreenshot / requestScreenshotPath) from a command handler races
# the standalone main loop, which polls those non-atomic int/bool/std::string fields each frame: a
# command dispatched from an MCP/Lua WORKER thread writing them unsynchronised is a genuine data race
# (UB — the std::string buffer can reallocate under the reader; Pillar-3 never-crash). The conformant
# seam is RunOnUiThreadAsCommandResult / RunOnUiThread<...>(app, [...]{ ...write... }) which marshals
# the write onto the UI thread (see BuiltinCommands_Debug.cpp). So a request-flag WRITE in a command
# TU must sit inside a RunOnUiThread* closure.
#
# Scenarios/ is EXEMPT: IScenario lifecycle methods (OnStart/OnFrame/OnFinish/IsDone) run ON the UI
# thread by the scenario-runner contract, so their direct request-flag writes are correct (and would
# false-positive without the carve-out). The rule is scoped to the command-handler TUs where the
# off-thread risk is real.
#
# Heuristic (NOT an AST/thread-analysis — a text proxy): for each request-flag WRITE line (`X.field =`
# where field is a request-flag and the line is an assignment, not a read/compare), scan BACKWARD
# within a bounded window for an enclosing `RunOnUiThread` token. No enclosing RunOnUiThread in the
# window -> the write is (heuristically) off the UI thread -> fire. The window is generous
# ($SMATCHET_UI_REQFLAG_WINDOW lines) so a multi-statement closure body still sees its RunOnUiThread
# head. A `// SMATCHET_DEVIATION(rule=ui-request-flag-off-thread; ...)` above the write escapes. The
# tree's command handlers all marshal correctly today, so any unwrapped write is a regression
# (absolute-0, no grandfathering — same model as no-glfw / cmake-local-gate-ci-scope).
UI_REQFLAG_RE='requestWindowResize|requestWindowWidth|requestWindowHeight|requestScreenshotPath|requestScreenshot'
UI_REQFLAG_WINDOW="${SMATCHET_UI_REQFLAG_WINDOW:-40}"

scan_ui_request_flag_file() {
    # $1 = a command-dispatch .cpp under Source/Core/src/Commands/ (NOT Scenarios/). Emits
    # `ui-request-flag-off-thread\t<f>:<line>` per request-flag write with no enclosing RunOnUiThread.
    local f="$1"
    [ -f "$f" ] || return 0
    case "$f" in *.cpp) ;; *) return 0 ;; esac
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i j start
    for ((i = 0; i < n; i++)); do
        local raw="${lines[$i]}"
        # Skip pure-comment lines.
        local s="${raw#"${raw%%[![:space:]]*}"}"
        case "$s" in '//'*|'*'*|'/*'*) continue ;; esac
        # Must be an ASSIGNMENT to a request-flag field: `.<field> =` (single `=`, not `==`/`!=`/`>=`).
        [[ "$raw" =~ \.($UI_REQFLAG_RE)[[:space:]]*=[^=] ]] || continue
        # Scan backward for an enclosing RunOnUiThread (the conformant marshalling seam) OR an
        # in-window SMATCHET_DEVIATION suppressing this rule.
        start=$(( i - UI_REQFLAG_WINDOW )); [ "$start" -lt 0 ] && start=0
        local marshalled=0 dev=0
        for ((j = i; j >= start; j--)); do
            case "${lines[$j]}" in *'RunOnUiThread'*) marshalled=1; break ;; esac
            case "${lines[$j]}" in *'SMATCHET_DEVIATION(rule=ui-request-flag-off-thread'*) dev=1; break ;; esac
        done
        if [ "$marshalled" -eq 0 ] && [ "$dev" -eq 0 ]; then
            printf 'ui-request-flag-off-thread\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_ui_request_flag_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files \
            'Source/Core/src/Commands/**' \
            2>/dev/null \
            | grep -E '\.cpp$' | grep -vE '(^|/)(ThirdParty|Commands/Scenarios)/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_ui_request_flag_file "$f"; done
}
