#!/usr/bin/env bash
# workflow-watchdog.sh — classify a background Workflow fleet as progressing /
# crawl / frozen, per docs/agent-rules/workflow-fleets.md § Stall watchdog. A
# "stalled" fleet has two opposite causes with opposite remedies — crawl (TPM
# starvation: work still landing slowly → reduce concurrency, do NOT kill) vs
# frozen (permission prompt or agent death: newest transcript static ≥ ~10 min →
# kill + salvage). This automates the by-hand transcript-mtime archaeology that
# the 2026-06-10/11 audit-fleet salvage waves needed mid-incident.
#
# READ-ONLY. Never kills, never edits a fleet — it polls + classifies + (optional)
# nudges. The only file it writes is its own cross-poll state baseline.
#
# Usage:
#   workflow-watchdog.sh <fleet-slug> [--nudge] [--max-cascade-victims N]
#                        [--session-dir PATH] [--expected-agents N]
#                        [--amplification-pct P]
#
#   <fleet-slug>  names build/<slug>/results (deliverable count) + the state file.
#   --nudge       SessionStart-formatted block, emitted ONLY when frozen OR
#                 cascade (silent otherwise). Default mode prints a one-line
#                 verdict every poll.
#
# Verdict (needs two polls to tell crawl from frozen — a single sample cannot
# show "advancing"; the first poll establishes the baseline and leans crawl):
#   cascade victims ≥ MAX_CASCADE_VICTIMS     → cascade      (restart churn, OVERRIDES)
#   or runs ≥ AMPLIFICATION_PCT% of EXPECTED  → cascade      (run amplification, OVERRIDES)
#   age ≤ FRESH_SECS                          → progressing (transcript just touched)
#   else, count or newest-mtime grew vs prev  → crawl       (work still landing)
#   else, age ≥ FROZEN_SECS                   → frozen       (static, nothing new)
#   else                                      → crawl       (uncertain → don't kill)
#
# CASCADE is a THIRD failure the mtime/count signals miss: a compacted subagent
# is auto-restarted onto the SAME oversized scope, re-compacts, and restarts again
# — 2.6× run amplification that never converges (run wf_62807bcd-dc8: 35/74
# transcripts ended compacted+interrupted, 9/14 lanes done in 40 min). Its remedy
# is OPPOSITE to crawl/frozen: not "reduce concurrency", not "kill+salvage", but
# ABORT + RE-SCOPE the churning lane (smaller model / tighter file list / windowed
# reads) so it stops re-crossing the compact threshold. Cascade can co-occur with
# progressing (some lanes land while others churn), so it OVERRIDES the verdict —
# it is the actionable signal. Cascade has TWO independent triggers:
#   (1) VICTIM count — agent-*.jsonl carrying BOTH `"isCompactSummary":true` AND a
#       "[Request interrupted by user]" marker (the compact→interrupt→restart
#       signature) — reaches MAX_CASCADE_VICTIMS.
#   (2) RUN AMPLIFICATION — total agent runs ≥ AMPLIFICATION_PCT% of the launch's
#       EXPECTED_AGENTS fan-out width. This catches churn the victim signature
#       misses (a restart that compacts without the literal interrupt string, or
#       a fleet whose run-count balloons past its logical agent count). It is OFF
#       by default (EXPECTED_AGENTS=0) because the only robust agent-run count is
#       fleet-scoped: set --session-dir to one fleet's `<session>/subagents/` dir
#       so other concurrent sessions' agents do not inflate the ratio. Without a
#       session-dir the transcript root spans every session and a raw run count is
#       meaningless, so amplification stays disabled and only the victim signature
#       (rare + recency-windowed) drives cascade.
#
# Test seams (production leaves all unset → identical behaviour):
#   WATCHDOG_RESULTS_DIR       deliverable dir (default build/<slug>/results).
#   WATCHDOG_TRANSCRIPT_DIR    agent-transcript root (default $HOME/.claude/projects).
#   WATCHDOG_SESSION_DIR       fleet-scoped transcript dir (default: TRANSCRIPT_DIR,
#                              the whole projects history). Set to a single fleet's
#                              `<session>/subagents/` dir so the victim + amplification
#                              + mtime signals ignore other sessions (--session-dir).
#   WATCHDOG_EXPECTED_AGENTS   logical fan-out width for the amplification check
#                              (default 0 = disabled; --expected-agents N).
#   WATCHDOG_AMPLIFICATION_PCT amplification trigger as a percent of EXPECTED_AGENTS
#                              (default 200 = 2.0×; --amplification-pct P).
#   WATCHDOG_STATE_FILE        cross-poll baseline (default build/<slug>/.watchdog-state).
#   WATCHDOG_FRESH_SECS        progressing threshold (default 120).
#   WATCHDOG_FROZEN_SECS       frozen threshold (default 600 = ~10 min).
#   WATCHDOG_MAX_CASCADE_VICTIMS  cascade trigger (default 3; --max-cascade-victims N).
#   WATCHDOG_CASCADE_WINDOW_SECS  victim/run-recency window (default 1800 = 30 min);
#                              only transcripts modified within it count, so a stale
#                              incident in the shared history can't force cascade.
#   WATCHDOG_NOW               "now" epoch seconds (default `date +%s`).
#
# GNU find (`-printf '%T@'`) is required for transcript mtimes — present in Git
# Bash / Linux CI where the bats gate runs.

set -euo pipefail
cd "$(dirname "$0")/../../.."

usage() { echo "usage: workflow-watchdog.sh <fleet-slug> [--nudge] [--max-cascade-victims N] [--session-dir PATH] [--expected-agents N] [--amplification-pct P]" >&2; }

NUDGE=0
slug=""
MAX_CASCADE_VICTIMS="${WATCHDOG_MAX_CASCADE_VICTIMS:-3}"
SESSION_DIR="${WATCHDOG_SESSION_DIR:-}"
EXPECTED_AGENTS="${WATCHDOG_EXPECTED_AGENTS:-0}"
AMPLIFICATION_PCT="${WATCHDOG_AMPLIFICATION_PCT:-200}"
while [ $# -gt 0 ]; do
    case "$1" in
        --nudge)                 NUDGE=1; shift ;;
        --max-cascade-victims)   [ $# -ge 2 ] || { echo "workflow-watchdog: --max-cascade-victims needs a value" >&2; usage; exit 2; }
                                 MAX_CASCADE_VICTIMS="$2"; shift 2 ;;
        --max-cascade-victims=*) MAX_CASCADE_VICTIMS="${1#*=}"; shift ;;
        --session-dir)           [ $# -ge 2 ] || { echo "workflow-watchdog: --session-dir needs a value" >&2; usage; exit 2; }
                                 SESSION_DIR="$2"; shift 2 ;;
        --session-dir=*)         SESSION_DIR="${1#*=}"; shift ;;
        --expected-agents)       [ $# -ge 2 ] || { echo "workflow-watchdog: --expected-agents needs a value" >&2; usage; exit 2; }
                                 EXPECTED_AGENTS="$2"; shift 2 ;;
        --expected-agents=*)     EXPECTED_AGENTS="${1#*=}"; shift ;;
        --amplification-pct)     [ $# -ge 2 ] || { echo "workflow-watchdog: --amplification-pct needs a value" >&2; usage; exit 2; }
                                 AMPLIFICATION_PCT="$2"; shift 2 ;;
        --amplification-pct=*)   AMPLIFICATION_PCT="${1#*=}"; shift ;;
        -h|--help)               usage; exit 0 ;;
        -*)                      echo "workflow-watchdog: unknown flag: $1" >&2; usage; exit 2 ;;
        *)                       [ -z "$slug" ] && slug="$1"; shift ;;
    esac
done
[ -z "$slug" ] && { usage; exit 2; }
case "$MAX_CASCADE_VICTIMS" in ''|*[!0-9]*) MAX_CASCADE_VICTIMS=3 ;; esac
case "$EXPECTED_AGENTS"    in ''|*[!0-9]*) EXPECTED_AGENTS=0 ;; esac
case "$AMPLIFICATION_PCT"  in ''|*[!0-9]*) AMPLIFICATION_PCT=200 ;; esac

RESULTS_DIR="${WATCHDOG_RESULTS_DIR:-build/$slug/results}"
TRANSCRIPT_DIR="${WATCHDOG_TRANSCRIPT_DIR:-$HOME/.claude/projects}"
FRESH_SECS="${WATCHDOG_FRESH_SECS:-120}"
FROZEN_SECS="${WATCHDOG_FROZEN_SECS:-600}"
now="${WATCHDOG_NOW:-$(date +%s)}"

# The dir the transcript signals (mtime / victims / run count) scan. Defaults to
# the whole TRANSCRIPT_DIR (every session); --session-dir narrows it to one
# fleet's `<session>/subagents/` so concurrent sessions don't pollute the counts.
SCAN_DIR="${SESSION_DIR:-$TRANSCRIPT_DIR}"
STATE_FILE="${WATCHDOG_STATE_FILE:-build/$slug/.watchdog-state}"

# Newest *.jsonl mtime (epoch int) under a dir, or 0 when none/absent.
newest_jsonl_mtime() {
    local dir="$1" m
    [ -d "$dir" ] || { echo 0; return 0; }
    m=$(find "$dir" -name '*.jsonl' -printf '%T@\n' 2>/dev/null | sort -nr | head -1 || true)
    [ -z "$m" ] && { echo 0; return 0; }
    echo "${m%.*}"   # drop the fractional part → integer epoch
}

# Count cascade VICTIM transcripts under a dir: an agent-*.jsonl that carries
# BOTH a compaction summary AND an interrupt marker (the compact→interrupt→
# restart signature). grep-based (no jq dependency) so it runs anywhere the
# crawl/frozen path does. Per-lane attribution (which lane to re-scope) needs a
# harness lane-id the transcript does not carry across restarts — deferred to the
# launch-protocol abort; this aggregate count is the convergence-failure signal.
# RUN-SCOPED: only victims modified within the recency window ($2 = cutoff epoch)
# count — SCAN_DIR may default to the whole ~/.claude/projects history, so a
# stale incident from days ago must NOT permanently force `cascade` on a healthy
# current run (`-newermt @cutoff` drops anything older than the window).
count_cascade_victims() {
    local dir="$1" cutoff="$2" f n=0
    [ -d "$dir" ] || { echo 0; return 0; }
    while IFS= read -r f; do
        grep -q '"isCompactSummary":[[:space:]]*true' "$f" 2>/dev/null || continue
        grep -qi 'request interrupted by user' "$f" 2>/dev/null || continue
        n=$((n + 1))
    done < <(find "$dir" -name 'agent-*.jsonl' -newermt "@$cutoff" 2>/dev/null)
    echo "$n"
}

# Count ALL agent-*.jsonl transcripts under a dir within the recency window — the
# total agent-RUN count for the run-amplification check. Only meaningful when
# SCAN_DIR is fleet-scoped (--session-dir), else it spans every session.
count_agent_runs() {
    local dir="$1" cutoff="$2"
    [ -d "$dir" ] || { echo 0; return 0; }
    find "$dir" -name 'agent-*.jsonl' -newermt "@$cutoff" 2>/dev/null | wc -l | tr -d ' '
}

# Current deliverable count (top-level files only).
count=0
if [ -d "$RESULTS_DIR" ]; then
    count=$(find "$RESULTS_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
fi

newest_mtime="$(newest_jsonl_mtime "$SCAN_DIR")"
age=$(( now - newest_mtime ))
[ "$age" -lt 0 ] && age=0

# Run-scope the victim + run counts to a recency window (default 30 min) so a
# stale incident in the shared ~/.claude/projects history can't permanently force
# `cascade` on a healthy current run.
CASCADE_WINDOW_SECS="${WATCHDOG_CASCADE_WINDOW_SECS:-1800}"
case "$CASCADE_WINDOW_SECS" in ''|*[!0-9]*) CASCADE_WINDOW_SECS=1800 ;; esac
cascade_cutoff=$(( now - CASCADE_WINDOW_SECS )); [ "$cascade_cutoff" -lt 0 ] && cascade_cutoff=0
cascade_victims="$(count_cascade_victims "$SCAN_DIR" "$cascade_cutoff")"

# Run-amplification: total agent runs vs the launch's expected fan-out width.
# OFF unless EXPECTED_AGENTS>0 AND a fleet-scoped --session-dir is supplied (a
# raw run count over the whole projects root is meaningless). amplified=1 when
# runs*100 ≥ EXPECTED_AGENTS*AMPLIFICATION_PCT.
agent_runs=0
amplified=0
amp_active=0
if [ "$EXPECTED_AGENTS" -gt 0 ] && [ -n "$SESSION_DIR" ]; then
    amp_active=1
    agent_runs="$(count_agent_runs "$SCAN_DIR" "$cascade_cutoff")"
    if [ $(( agent_runs * 100 )) -ge $(( EXPECTED_AGENTS * AMPLIFICATION_PCT )) ]; then
        amplified=1
    fi
fi

# Prior poll baseline (count + newest mtime). Absent → 0/0 (first poll).
prev_count=0
prev_mtime=0
if [ -f "$STATE_FILE" ]; then
    read -r prev_count prev_mtime < "$STATE_FILE" 2>/dev/null || true
    prev_count="${prev_count:-0}"
    prev_mtime="${prev_mtime:-0}"
fi

# --- classify ----------------------------------------------------------------
# Cascade is orthogonal to the mtime/count axis (it can co-occur with landing
# work) and has the opposite remedy, so it OVERRIDES once EITHER the victim count
# crosses the threshold OR run amplification fires — it is the actionable signal.
if [ "$cascade_victims" -ge "$MAX_CASCADE_VICTIMS" ] || [ "$amplified" -eq 1 ]; then
    verdict="cascade"
elif [ "$age" -le "$FRESH_SECS" ]; then
    verdict="progressing"
elif [ "$count" -gt "$prev_count" ] || [ "$newest_mtime" -gt "$prev_mtime" ]; then
    verdict="crawl"
elif [ "$age" -ge "$FROZEN_SECS" ]; then
    verdict="frozen"
else
    verdict="crawl"
fi

# Persist this poll as the next poll's baseline (best-effort; the only write).
mkdir -p "$(dirname "$STATE_FILE")" 2>/dev/null || true
printf '%s %s\n' "$count" "$newest_mtime" > "$STATE_FILE" 2>/dev/null || true

case "$verdict" in
    progressing) action="healthy" ;;
    crawl)       action="TPM starvation — reduce concurrency / pin a smaller model; do NOT kill" ;;
    frozen)      action="permission prompt or agent death — kill + enter the salvage runbook; waiting buys nothing" ;;
    cascade)     action="restart cascade — abort + RE-SCOPE the churning lane(s) (smaller model / tighter file list / windowed reads); reducing concurrency will NOT help (each restart re-crosses the compact threshold)" ;;
esac

# --- emit --------------------------------------------------------------------
if [ "$NUDGE" -eq 1 ]; then
    # Silent unless actionable-without-waiting (frozen or cascade); a SessionStart
    # nudge that fires every poll is noise, and crawl/progressing self-resolve.
    if [ "$verdict" = "frozen" ]; then
        echo "## === workflow fleet frozen: ${slug} ==="
        echo "Newest agent transcript static for ${age}s (≥ ${FROZEN_SECS}s) and no new"
        echo "results in ${RESULTS_DIR} (${count} file(s)). Per workflow-fleets.md"
        echo "§ Stall watchdog this is FROZEN, not crawl — waiting buys nothing."
        echo "Action: kill the fleet, then enter the § Salvage runbook (transcripts survive)."
    elif [ "$verdict" = "cascade" ]; then
        echo "## === workflow fleet restart-cascade: ${slug} ==="
        if [ "$amplified" -eq 1 ]; then
            echo "${agent_runs} agent run(s) for ${EXPECTED_AGENTS} expected (≥ ${AMPLIFICATION_PCT}% — run amplification),"
            echo "${cascade_victims} also ended compacted+interrupted — the compact→interrupt→restart"
            echo "cascade (workflow-fleets.md § Stall watchdog)."
        else
            echo "${cascade_victims} agent transcript(s) ended compacted+interrupted (≥ ${MAX_CASCADE_VICTIMS})"
            echo "— the compact→interrupt→restart cascade (workflow-fleets.md § Stall watchdog)."
        fi
        echo "It will NOT converge by waiting or by reducing concurrency: each restart"
        echo "re-crosses the compact threshold on the same oversized scope."
        echo "Action: abort + RE-SCOPE the churning lane(s) — smaller model, tighter"
        echo "file list, windowed reads — then relaunch only the unfinished lanes."
    fi
else
    if [ "$amp_active" -eq 1 ]; then
        echo "workflow-watchdog[${slug}]: ${verdict} — ${count} result(s), ${cascade_victims} cascade victim(s), ${agent_runs}/${EXPECTED_AGENTS} agent run(s), newest transcript ${age}s ago (${action})."
    else
        echo "workflow-watchdog[${slug}]: ${verdict} — ${count} result(s), ${cascade_victims} cascade victim(s), newest transcript ${age}s ago (${action})."
    fi
fi
exit 0
