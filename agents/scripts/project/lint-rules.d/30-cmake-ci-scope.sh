#!/usr/bin/env bash
# 30-cmake-ci-scope.sh — cmake-local-gate-ci-scope rule (sourced by test-lint-rules.sh, not run directly).

# cmake-local-gate-ci-scope — ABSOLUTE-0 over CMakeLists.txt / cmake/*.cmake.
# A `message(FATAL_ERROR ...)` keyed on a LOCAL-dev project.config knob
# (msvc_toolset_pin) must be scoped to non-CI (`NOT DEFINED ENV{CI}` /
# `ENV{GITHUB_ACTIONS}`): CI runners configure FRESH every run and use their own
# (consistent, often newer) toolset, so they can NEVER hit the stale-cache
# cl-vs-headers class the toolset guard targets — applied unconditionally the
# guard FATALs every runner whose toolset != the pin (incident #1074 red-walled
# all 5 Windows CI required checks; the fix was `if(... AND NOT DEFINED ENV{CI})`).
# The local-only intent lived in the COMMENT, not the CONDITION — this lint moves
# it into the condition. The tree's one such guard is correctly CI-scoped today, so
# any unguarded hit is a regression (no grandfathering — same model as no-glfw).
# Cheap heuristic (not a CMake AST parse): for each `message(FATAL_ERROR` line, a
# backward window of $CMAKE_CI_SCOPE_WINDOW lines that mentions a local knob but
# carries no ENV{CI}/ENV{GITHUB_ACTIONS} token fires. A
# `# SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope; ...)` within the window escapes.
CMAKE_LOCAL_KNOBS_RE='msvc_toolset_pin'           # extend with | for new local knobs
CMAKE_CI_SCOPE_WINDOW="${SMATCHET_CMAKE_CI_WINDOW:-80}"

scan_cmake_ci_scope_file() {
    # $1 = a CMakeLists.txt / *.cmake file. Emits `cmake-local-gate-ci-scope\t<f>:<line>`
    # per un-CI-scoped local-knob FATAL_ERROR. CMake files are small — read fully.
    local f="$1"
    [ -f "$f" ] || return 0
    local -a lines=()
    local line
    while IFS= read -r line || [ -n "$line" ]; do lines+=("$line"); done < "$f"
    local n=${#lines[@]} i j start knob ci dev
    for ((i = 0; i < n; i++)); do
        case "${lines[$i]}" in *'message(FATAL_ERROR'*) ;; *) continue ;; esac
        start=$(( i - CMAKE_CI_SCOPE_WINDOW )); [ "$start" -lt 0 ] && start=0
        knob=0; ci=0; dev=0
        for ((j = start; j <= i; j++)); do
            case "${lines[$j]}" in *"$CMAKE_LOCAL_KNOBS_RE"*) knob=1 ;; esac
            case "${lines[$j]}" in *'ENV{CI}'*|*'ENV{GITHUB_ACTIONS}'*) ci=1 ;; esac
            case "${lines[$j]}" in *'SMATCHET_DEVIATION(rule=cmake-local-gate-ci-scope'*) dev=1 ;; esac
        done
        if [ "$knob" -eq 1 ] && [ "$ci" -eq 0 ] && [ "$dev" -eq 0 ]; then
            printf 'cmake-local-gate-ci-scope\t%s:%s\n' "$f" "$((i + 1))"
        fi
    done
}

compute_cmake_ci_scope_violations() {
    local files=() f
    while IFS= read -r f; do files+=("$f"); done < <(
        git ls-files 2>/dev/null \
            | grep -E '(^|/)CMakeLists\.txt$|(^|/)cmake/.*\.cmake$' \
            | grep -vE '(^|/)(ThirdParty|build)/' || true
    )
    [ "${#files[@]}" -gt 0 ] || return 0
    for f in "${files[@]}"; do scan_cmake_ci_scope_file "$f"; done
}
