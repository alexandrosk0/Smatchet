#!/usr/bin/env bash
# fleet-rescope.sh — compute the abort + RE-SCOPE relaunch plan for a Workflow
# fleet caught in a restart cascade, per docs/agent-rules/workflow-fleets.md
# § Stall watchdog / § Salvage runbook. The launch-protocol other half of
# workflow-watchdog.sh: the watchdog DETECTS + quantifies a cascade (read-only,
# never kills); this turns that verdict into the precise, mechanical answer to
# the nudge's own instruction — "relaunch ONLY the unfinished lanes."
#
# Background (tooling.md `audit-fleet-restart-cascade-never-converges`): a
# compacted subagent is auto-restarted onto the SAME oversized scope, re-compacts,
# and restarts again — never converging (run wf_62807bcd-dc8: 74 runs for ~28
# logical agents, 9/14 lanes done in 40 min; 5 lanes produced NOTHING). The remedy
# is abort + re-scope the churning lanes. But "which lanes are still churning?" was
# orchestrator guesswork — per-lane attribution is infeasible (a compacted restart
# begins from a continuation summary, not its original lane prompt, so a transcript
# carries no restart-stable lane id). This script answers it from the OTHER side:
# the launch's expected-lane manifest MINUS the lanes that produced a completed
# deliverable = exactly the set to abort + relaunch with a tighter scope.
#
# READ-ONLY, like the watchdog: it computes + prints a plan, it NEVER kills a
# fleet. The actual abort (TaskStop on an in-process Workflow run / killing a
# background bash fleet's agent processes) stays orchestrator judgment — a
# destructive action this tool deliberately leaves to the human-on/in-the-loop.
# Its only filesystem write is none (pure read + stdout).
#
# Usage:
#   fleet-rescope.sh <fleet-slug> (--lanes <file> | --lanes-csv a,b,c) [options]
#
#   <fleet-slug>        names build/<slug>/results/ (the deliverable dir the
#                       fleet's agents write to per § Checkpoint contract) — same
#                       slug the watchdog polls.
#   --lanes <file>      expected-lane manifest: one lane id per line; blank lines
#                       and `#` comments ignored; surrounding whitespace trimmed.
#                       The lane id MUST match the deliverable basename sans
#                       extension (the `results/<agent>.md` convention → <agent>
#                       is the lane id).
#   --lanes-csv a,b,c   inline manifest (unions with --lanes when both are given).
#   --list-unfinished   print ONLY the unfinished lane ids (newline-separated),
#                       no plan prose — for piping straight into the relaunch args.
#   --force             skip the watchdog verdict gate and emit the plan regardless
#                       (use when the watchdog already nudged you — you KNOW it is a
#                       cascade and just want the relaunch set). Without it, the
#                       script runs the watchdog and REFUSES (exit 3) on any verdict
#                       but `cascade`, so it can't be used to needlessly tear down a
#                       healthy / merely-crawling fleet.
#   --results-dir DIR   override the deliverable dir (default build/<slug>/results).
#   --session-dir PATH  forwarded to the watchdog (fleet-scoped transcript dir);
#                       also enables the amplification verdict — when given and
#                       --expected-agents is NOT passed, the manifest lane count is
#                       forwarded as --expected-agents automatically.
#   --expected-agents N / --amplification-pct P / --max-cascade-victims N
#                       forwarded verbatim to the watchdog verdict probe.
#
# Test seams (production leaves all unset → identical behaviour):
#   RESCOPE_WATCHDOG_BIN  path to the watchdog used for the verdict gate
#                         (default agents/scripts/core/workflow-watchdog.sh). bats
#                         points this at a stub so the rescope tests stay decoupled
#                         from the watchdog's own (separately-tested) detection.
#
# Exit codes (test-author convention):
#   0  plan emitted (cascade confirmed, --force, or verdict-unconfirmed WARN)
#   2  usage error (missing slug / missing manifest)
#   3  refused — the watchdog ran and the verdict was NOT cascade (pass --force
#      to override). The plan is non-destructive, but relaunching a healthy fleet
#      wastes tokens, so the gate defaults closed on a confirmed non-cascade.

set -euo pipefail
cd "$(dirname "$0")/../../.."

usage() {
    echo "usage: fleet-rescope.sh <fleet-slug> (--lanes <file> | --lanes-csv a,b,c) [--list-unfinished] [--force] [--results-dir DIR] [--session-dir PATH] [--expected-agents N] [--amplification-pct P] [--max-cascade-victims N]" >&2
}

slug=""
LANES_FILE=""
LANES_CSV=""
LIST_ONLY=0
FORCE=0
RESULTS_DIR_OVERRIDE=""
SESSION_DIR=""
EXPECTED_AGENTS=""
AMPLIFICATION_PCT=""
MAX_CASCADE_VICTIMS=""

while [ $# -gt 0 ]; do
    case "$1" in
        --lanes)               [ $# -ge 2 ] || { echo "fleet-rescope: --lanes needs a value" >&2; usage; exit 2; }
                               LANES_FILE="$2"; shift 2 ;;
        --lanes=*)             LANES_FILE="${1#*=}"; shift ;;
        --lanes-csv)           [ $# -ge 2 ] || { echo "fleet-rescope: --lanes-csv needs a value" >&2; usage; exit 2; }
                               LANES_CSV="$2"; shift 2 ;;
        --lanes-csv=*)         LANES_CSV="${1#*=}"; shift ;;
        --list-unfinished)     LIST_ONLY=1; shift ;;
        --force)               FORCE=1; shift ;;
        --results-dir)         [ $# -ge 2 ] || { echo "fleet-rescope: --results-dir needs a value" >&2; usage; exit 2; }
                               RESULTS_DIR_OVERRIDE="$2"; shift 2 ;;
        --results-dir=*)       RESULTS_DIR_OVERRIDE="${1#*=}"; shift ;;
        --session-dir)         [ $# -ge 2 ] || { echo "fleet-rescope: --session-dir needs a value" >&2; usage; exit 2; }
                               SESSION_DIR="$2"; shift 2 ;;
        --session-dir=*)       SESSION_DIR="${1#*=}"; shift ;;
        --expected-agents)     [ $# -ge 2 ] || { echo "fleet-rescope: --expected-agents needs a value" >&2; usage; exit 2; }
                               EXPECTED_AGENTS="$2"; shift 2 ;;
        --expected-agents=*)   EXPECTED_AGENTS="${1#*=}"; shift ;;
        --amplification-pct)   [ $# -ge 2 ] || { echo "fleet-rescope: --amplification-pct needs a value" >&2; usage; exit 2; }
                               AMPLIFICATION_PCT="$2"; shift 2 ;;
        --amplification-pct=*) AMPLIFICATION_PCT="${1#*=}"; shift ;;
        --max-cascade-victims) [ $# -ge 2 ] || { echo "fleet-rescope: --max-cascade-victims needs a value" >&2; usage; exit 2; }
                               MAX_CASCADE_VICTIMS="$2"; shift 2 ;;
        --max-cascade-victims=*) MAX_CASCADE_VICTIMS="${1#*=}"; shift ;;
        -h|--help)             usage; exit 0 ;;
        -*)                    echo "fleet-rescope: unknown flag: $1" >&2; usage; exit 2 ;;
        *)                     [ -z "$slug" ] && slug="$1"; shift ;;
    esac
done

[ -z "$slug" ] && { usage; exit 2; }
[ -z "$LANES_FILE" ] && [ -z "$LANES_CSV" ] && { echo "fleet-rescope: a manifest is required (--lanes <file> or --lanes-csv a,b,c)" >&2; usage; exit 2; }

RESULTS_DIR="${RESULTS_DIR_OVERRIDE:-build/$slug/results}"
WATCHDOG_BIN="${RESCOPE_WATCHDOG_BIN:-agents/scripts/core/workflow-watchdog.sh}"

# --- resolve the expected-lane manifest --------------------------------------
# A lane id is a deliverable basename: [A-Za-z0-9._-]+ (no slash, no glob char) so
# it slots safely into a glob test below. Invalid ids are dropped with a WARN
# rather than silently mismatched.
LANES=()
add_lane() {
    local l="$1"
    # trim surrounding whitespace
    l="${l#"${l%%[![:space:]]*}"}"; l="${l%"${l##*[![:space:]]}"}"
    [ -z "$l" ] && return 0
    case "$l" in \#*) return 0 ;; esac   # comment line
    case "$l" in *[!A-Za-z0-9._-]*) echo "fleet-rescope: WARN skipping invalid lane id '$l' (allowed: A-Za-z0-9._-)" >&2; return 0 ;; esac
    LANES+=("$l")
}

if [ -n "$LANES_FILE" ]; then
    [ -f "$LANES_FILE" ] || { echo "fleet-rescope: --lanes file not found: $LANES_FILE" >&2; exit 2; }
    while IFS= read -r line || [ -n "$line" ]; do add_lane "$line"; done < "$LANES_FILE"
fi
if [ -n "$LANES_CSV" ]; then
    IFS=',' read -ra _csv <<< "$LANES_CSV"
    for l in "${_csv[@]}"; do add_lane "$l"; done
fi

# de-dup (a lane named in both --lanes and --lanes-csv counts once)
if [ "${#LANES[@]}" -gt 0 ]; then
    mapfile -t LANES < <(printf '%s\n' "${LANES[@]}" | awk '!seen[$0]++')
fi
[ "${#LANES[@]}" -eq 0 ] && { echo "fleet-rescope: manifest resolved to zero lanes" >&2; exit 2; }

# --- a lane is COMPLETE iff a non-empty deliverable exists -------------------
# Matches `<lane>` or `<lane>.<ext>` (the results/<agent>.md convention) at the
# top level of the results dir. Non-empty (-s) so a 0-byte progress-marker stub
# never reads as a finished deliverable. No nullglob needed: an unmatched
# `<lane>.*` glob stays literal and fails the -f test.
lane_complete() {
    local lane="$1" f
    [ -d "$RESULTS_DIR" ] || return 1
    for f in "$RESULTS_DIR/$lane" "$RESULTS_DIR/$lane".*; do
        [ -f "$f" ] && [ -s "$f" ] && return 0
    done
    return 1
}

COMPLETED=()
UNFINISHED=()
for lane in "${LANES[@]}"; do
    if lane_complete "$lane"; then COMPLETED+=("$lane"); else UNFINISHED+=("$lane"); fi
done

# --- verdict gate ------------------------------------------------------------
# Without --force, confirm the fleet is actually in a cascade before recommending
# a tear-down. Probe the read-only watchdog (reuse its detection — DRY) and refuse
# on any verdict but `cascade`. A missing/erroring watchdog is fail-OPEN-with-WARN:
# the plan itself is non-destructive (the orchestrator still owns the kill), so an
# unconfirmable verdict should not block computing the relaunch set.
verdict="(unconfirmed)"
if [ "$FORCE" -eq 1 ]; then
    verdict="(skipped — --force)"
else
    wd_args=("$slug")
    [ -n "$SESSION_DIR" ]         && wd_args+=(--session-dir "$SESSION_DIR")
    # Derive --expected-agents from the manifest width when the caller scoped the
    # session but didn't pin it explicitly — the manifest IS the expected fan-out.
    if [ -n "$EXPECTED_AGENTS" ]; then
        wd_args+=(--expected-agents "$EXPECTED_AGENTS")
    elif [ -n "$SESSION_DIR" ]; then
        wd_args+=(--expected-agents "${#LANES[@]}")
    fi
    [ -n "$AMPLIFICATION_PCT" ]   && wd_args+=(--amplification-pct "$AMPLIFICATION_PCT")
    [ -n "$MAX_CASCADE_VICTIMS" ] && wd_args+=(--max-cascade-victims "$MAX_CASCADE_VICTIMS")

    wd_out=""
    if [ -x "$WATCHDOG_BIN" ] || [ -f "$WATCHDOG_BIN" ]; then
        wd_out="$(bash "$WATCHDOG_BIN" "${wd_args[@]}" 2>/dev/null || true)"
    fi
    if [ -z "$wd_out" ]; then
        echo "fleet-rescope: WARN watchdog verdict UNCONFIRMED (probe unavailable) — proceeding; the abort is yours to gate" >&2
        verdict="(unconfirmed)"
    elif printf '%s' "$wd_out" | grep -q ']: cascade'; then
        verdict="cascade"
    else
        # extract the verdict word for the message, best-effort
        verdict="$(printf '%s' "$wd_out" | sed -n 's/.*\]: \([a-z]*\).*/\1/p' | head -1)"
        [ -z "$verdict" ] && verdict="(non-cascade)"
        echo "fleet-rescope: REFUSED — watchdog verdict is '${verdict}', not 'cascade'. Re-scope/relaunch is the cascade remedy; a ${verdict} fleet wants a different action (see workflow-fleets.md § Stall watchdog). Pass --force to override." >&2
        exit 3
    fi
fi

# --- emit --------------------------------------------------------------------
if [ "$LIST_ONLY" -eq 1 ]; then
    [ "${#UNFINISHED[@]}" -gt 0 ] && printf '%s\n' "${UNFINISHED[@]}"
    exit 0
fi

echo "## === fleet re-scope plan: ${slug} ==="
echo "Watchdog verdict: ${verdict}. Plan per workflow-fleets.md § Stall watchdog / § Salvage runbook."
echo "Expected lanes: ${#LANES[@]}  ·  completed: ${#COMPLETED[@]}  ·  unfinished: ${#UNFINISHED[@]}"
echo ""

if [ "${#COMPLETED[@]}" -gt 0 ]; then
    echo "Completed — KEEP, do NOT relaunch (${#COMPLETED[@]}):"
    printf '  %s\n' "${COMPLETED[@]}"
    echo ""
fi

if [ "${#UNFINISHED[@]}" -eq 0 ]; then
    echo "No unfinished lanes — every expected deliverable landed. The fleet converged;"
    echo "there is nothing to relaunch (if it is still spawning runs, that is pure churn —"
    echo "abort it, but no lane needs re-running)."
    exit 0
fi

echo "Unfinished — ABORT + RELAUNCH with a tighter scope (${#UNFINISHED[@]}):"
printf '  %s\n' "${UNFINISHED[@]}"
echo ""
echo "Re-scope EACH relaunched lane (the cascade remedy — NOT reduce-concurrency,"
echo "NOT wait; each restart re-crosses the compact threshold on the same scope):"
echo "  • pin a smaller model: ('sonnet' or below) on every relaunched agent()"
echo "  • tighter file list — repo-relative file:line targets, not a directory/glob scope"
echo "  • windowed reads (offset/limit) + an explicit token budget line in the prompt"
echo ""
echo "Abort is ORCHESTRATOR-executed (this tool computes the plan, never kills):"
echo "  • in-process Workflow: TaskStop / the /workflows UI on the churning run"
echo "  • background bash fleet: kill the fleet's agent processes"
echo "Then re-run fleet-preflight.sh --strict on the relaunch script and launch ONLY"
echo "the unfinished lanes above (--list-unfinished pipes them straight into args)."
exit 0
