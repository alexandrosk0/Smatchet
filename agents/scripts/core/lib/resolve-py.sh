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
#   Prints the first WORKING Python 3 interpreter (ABSOLUTE path) to stdout and
#   returns 0, or prints nothing and returns 1 when no candidate resolves,
#   executes, AND is Python 3. Candidates, in order: python3, python, py.
#   Callers decide their own degrade contract:
#     * hard-require:  PY="$(resolve_py)" || { echo "python required" >&2; exit 2; }
#     * skip-if-absent: PY="$(resolve_py || true)"; [ -n "$PY" ] || skip
#   Every downstream invocation must use "$PY", never a bare `python3`.
#
#   Python 3 is part of the validation (CR #2019 round 1): a lingering Python 2
#   `python` must not win the fallback — callers run py3-only code
#   (`open(..., encoding=…)`, f-strings), so a py2 pick trades the loud
#   stub-failure this lib exists to prevent for a quieter mid-script one.
#   The printed path is made ABSOLUTE (same round): the PATH lookup echoes a
#   relative path for a relative PATH entry, which breaks the first caller
#   that cd's between resolving and invoking. Resolution uses `type -P`, not
#   `command -v` (round 2): a shell function or alias shadowing a candidate
#   name would win `command -v`, pass the run probe, and canonicalize to a
#   bogus "$PWD/<name>" — `type -P` only returns executable file paths.
#
#   SMATCHET_RESOLVE_PY_FORCE_NONE=1 forces the "no working python" branch —
#   a test seam so no-python degrade paths are testable in an environment that
#   DOES have python (mirrors SMATCHET_PRESHIP_FORCE_NO_PY in review-ack.sh).
#
# This file is sourced; it defines one function and does NOT `set -e`
# (the sourcing script owns shell options).

resolve_py() {
    if [ "${SMATCHET_RESOLVE_PY_FORCE_NONE:-0}" = "1" ]; then
        return 1
    fi
    local _rp_cand _rp_path
    for _rp_cand in python3 python py; do
        # type -P, not command -v (CR #2019 round 2): command -v echoes the BARE
        # NAME for a shell function/alias shadowing a candidate, which would
        # pass validation (the function runs) and canonicalize to a bogus
        # "$PWD/<name>". type -P only ever returns an executable file path.
        _rp_path="$(type -P "$_rp_cand" 2>/dev/null)" || continue
        [ -n "$_rp_path" ] || continue
        if "$_rp_path" -c 'import sys; raise SystemExit(0 if sys.version_info[0] >= 3 else 1)' \
            >/dev/null 2>&1; then
            case "$_rp_path" in
                /*|[A-Za-z]:[/\\]*) ;;   # already absolute (POSIX or Windows drive)
                *) _rp_path="$(cd "$(dirname "$_rp_path")" 2>/dev/null && pwd)/$(basename "$_rp_path")" || continue ;;
            esac
            printf '%s\n' "$_rp_path"
            return 0
        fi
    done
    return 1
}
