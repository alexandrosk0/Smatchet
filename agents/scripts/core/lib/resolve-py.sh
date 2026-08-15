#!/usr/bin/env bash
# resolve-py.sh — shared exec-validating python resolver (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (tooling py-probe-single-candidate-residual)
#   On Windows, `python3` on PATH is routinely the Microsoft Store *App
#   Execution Alias* stub: `command -v python3` resolves it and returns 0, but
#   running it prints an install banner and exits non-zero. A resolve-only
#   probe therefore hands back an interpreter that cannot run — silently wrong
#   on one platform. The repo's rule for pickers (shell-lint rule 9,
#   SHELL_LINT_PY_PROBE) is "exec-validate, don't just resolve"; this lib is
#   the one blessed home for that probe, so single-candidate guards
#   (`command -v python3 || exit 2` … then a bare `python3 foo.py`) stop
#   re-implementing it per script and the lint can enforce one shape instead
#   of guessing at intent from regexes.
#
# CONTRACT — resolve_py
#   Prints the first WORKING python interpreter (resolved path) to stdout and
#   returns 0, or prints nothing and returns 1 when no candidate both resolves
#   AND executes. Candidates, in order: python3, python, py. Callers decide
#   their own degrade contract:
#     * hard-require:  PY="$(resolve_py)" || { echo "python required" >&2; exit 2; }
#     * skip-if-absent: PY="$(resolve_py || true)"; [ -n "$PY" ] || skip
#   Every downstream invocation must use "$PY", never a bare `python3`.
#
#   SMATCHET_RESOLVE_PY_FORCE_NONE=1 forces the "no working python" branch —
#   a test seam so no-python degrade paths are testable in an environment that
#   DOES have python (mirrors SMATCHET_PRESHIP_FORCE_NO_PY in review-ack.sh).
#
#   NOTE resolve_py answers "does a python RUN here", not "which version" — a
#   caller that needs Python 3 specifically (e.g. a #!/usr/bin/env python3
#   module that must not meet a lingering py2) still owns its version check:
#       "$PY" -c 'import sys; sys.exit(0 if sys.version_info[0] >= 3 else 1)'
#
# This file is sourced; it defines one function and does NOT `set -e`
# (the sourcing script owns shell options).

resolve_py() {
    if [ "${SMATCHET_RESOLVE_PY_FORCE_NONE:-0}" = "1" ]; then
        return 1
    fi
    local _rp_cand _rp_path
    for _rp_cand in python3 python py; do
        _rp_path="$(command -v "$_rp_cand" 2>/dev/null)" || continue
        if "$_rp_path" -c "" >/dev/null 2>&1; then
            printf '%s\n' "$_rp_path"
            return 0
        fi
    done
    return 1
}
