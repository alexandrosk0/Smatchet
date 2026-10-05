#!/usr/bin/env bash
# run-gate.sh — run a command whose EXIT CODE IS THE VERDICT with its output captured
# in full to a log, then report that exit code explicitly.
# ----------------------------------------------------------------------------
# WHY (process 2026-08-18 piping-a-gate-into-tail-masks-its-exit-code)
#   `bash <gate>.sh 2>&1 | tail -N` reports TAIL's exit status, not the gate's:
#   `set -o pipefail` belongs to the script being run, not to the invoking shell,
#   so a run whose own summary said `Failed: 36` read as exit 0 — and `tail` had
#   already discarded the failure identities. This helper is the one correct
#   invocation shape for gates, test suites, lint runners and verdict checkers:
#   the redirect + explicit status stamp live here, so the masked-status pipe
#   becomes the unusual form instead of the habitual one.
#
# Usage:
#   bash agents/scripts/core/run-gate.sh [--name NAME] <log> -- <cmd> [args...]
#
#   Runs <cmd> with stdout+stderr redirected to <log> (parent dirs created; the
#   log is overwritten), appends `<NAME>_EXIT=<rc>` to the log, then prints:
#     <NAME>_EXIT=<rc>
#     the verdict lines grepped from the log (PASS / FAIL / Passed: N / Failed: N /
#     `not ok` / GATES_* / MISSING / failed-script list entries), capped at
#     $RUN_GATE_MAX_LINES (default 100) with a pointer to the full log.
#   NAME defaults to the command's script basename, upper-cased with every
#   non-alphanumeric as `_` (`bash scripts/dev/test-all.sh` -> TEST_ALL), skipping
#   interpreter / `env` / VAR=value / flag words.
#
# Exit: the command's own exit code (127 when it cannot be found); 2 on a usage
# error. Never masks the command's status — that is the whole point.
set -uo pipefail

usage() {
    echo "usage: run-gate.sh [--name NAME] <log> -- <cmd> [args...]" >&2
    exit 2
}

name=""
if [ "${1:-}" = "--name" ]; then
    [ -n "${2:-}" ] || usage
    name="$2"
    shift 2
fi
[ "$#" -ge 3 ] || usage
log="$1"
[ "$2" = "--" ] || usage
shift 2

# _derive_name <cmd...> — the first argument that names the actual program.
_derive_name() {
    local a base=""
    for a in "$@"; do
        case "$a" in
            -* | *=*) continue ;;
        esac
        # Interpreter / launcher words name no gate; skip to the script they run.
        case "${a##*/}" in
            bash|sh|zsh|env|bats|python|python3|py) continue ;;
        esac
        base="${a##*/}"
        base="${base%.*}"
        break
    done
    printf '%s' "${base:-gate}"
}

[ -n "$name" ] || name="$(_derive_name "$@")"
name="$(printf '%s' "$name" | tr '[:lower:]' '[:upper:]' | tr -c 'A-Z0-9' '_')"

log_dir="$(dirname "$log")"
mkdir -p "$log_dir" || { echo "run-gate: cannot create log dir $log_dir" >&2; exit 2; }

"$@" >"$log" 2>&1
rc=$?
printf '%s_EXIT=%s\n' "$name" "$rc" >>"$log"

# Verdict shapes the repo's gates print: summary tallies (`Passed: N  Failed: M`,
# `AGGREGATE`), whole-word PASS/FAIL/PASSED/FAILED/MISSING verdicts, bats `not ok`,
# merge-gates.sh GATES_* outcomes, and test-all.sh's failed-script list.
VERDICT_RE='(^|[^A-Za-z0-9_])(PASS|PASSED|FAIL|FAILED|MISSING)([^A-Za-z0-9_]|$)|Passed: *[0-9]+|Failed: *[0-9]+|^not ok [0-9]|GATES_(PASSED|BLOCKED|TIMEOUT|SKIPPED)|^Missing binary|^Failed scripts:|exit=[0-9]+\)$'
max="${RUN_GATE_MAX_LINES:-100}"
case "$max" in '' | *[!0-9]*) max=100 ;; esac

mapfile -t verdicts < <(grep -E -- "$VERDICT_RE" "$log" | grep -vE "^${name}_EXIT=" || true)
echo "${name}_EXIT=${rc}"
echo "--- verdict lines (full log: $log) ---"
shown=0
for line in "${verdicts[@]}"; do
    [ "$shown" -lt "$max" ] || break
    printf '%s\n' "$line"
    shown=$((shown + 1))
done
if [ "${#verdicts[@]}" -gt "$shown" ]; then
    echo "... $((${#verdicts[@]} - shown)) more verdict line(s) — read the full log: $log"
fi
[ "${#verdicts[@]}" -gt 0 ] || echo "(no verdict lines matched — the exit code above is the verdict)"
exit "$rc"
