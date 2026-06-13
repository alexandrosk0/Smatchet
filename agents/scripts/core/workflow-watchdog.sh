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
#   workflow-watchdog.sh <fleet-slug> [--nudge]
#
#   <fleet-slug>  names build/<slug>/results (deliverable count) + the state file.
#   --nudge       SessionStart-formatted block, emitted ONLY when frozen (silent
#                 otherwise). Default mode prints a one-line verdict every poll.
#
# Verdict (needs two polls to tell crawl from frozen — a single sample cannot
# show "advancing"; the first poll establishes the baseline and leans crawl):
#   age ≤ FRESH_SECS                          → progressing (transcript just touched)
#   else, count or newest-mtime grew vs prev  → crawl       (work still landing)
#   else, age ≥ FROZEN_SECS                   → frozen      (static, nothing new)
#   else                                      → crawl       (uncertain → don't kill)
#
# Test seams (production leaves all unset → identical behaviour):
#   WATCHDOG_RESULTS_DIR     deliverable dir (default build/<slug>/results).
#   WATCHDOG_TRANSCRIPT_DIR  agent-transcript root (default $HOME/.claude/projects).
#   WATCHDOG_STATE_FILE      cross-poll baseline (default build/<slug>/.watchdog-state).
#   WATCHDOG_FRESH_SECS      progressing threshold (default 120).
#   WATCHDOG_FROZEN_SECS     frozen threshold (default 600 = ~10 min).
#   WATCHDOG_NOW             "now" epoch seconds (default `date +%s`).
#
# GNU find (`-printf '%T@'`) is required for transcript mtimes — present in Git
# Bash / Linux CI where the bats gate runs.

set -euo pipefail
cd "$(dirname "$0")/../../.."

usage() { echo "usage: workflow-watchdog.sh <fleet-slug> [--nudge]" >&2; }

NUDGE=0
slug=""
for a in "$@"; do
    case "$a" in
        --nudge)   NUDGE=1 ;;
        -h|--help) usage; exit 0 ;;
        -*)        echo "workflow-watchdog: unknown flag: $a" >&2; usage; exit 2 ;;
        *)         [ -z "$slug" ] && slug="$a" ;;
    esac
done
[ -z "$slug" ] && { usage; exit 2; }

RESULTS_DIR="${WATCHDOG_RESULTS_DIR:-build/$slug/results}"
TRANSCRIPT_DIR="${WATCHDOG_TRANSCRIPT_DIR:-$HOME/.claude/projects}"
STATE_FILE="${WATCHDOG_STATE_FILE:-build/$slug/.watchdog-state}"
FRESH_SECS="${WATCHDOG_FRESH_SECS:-120}"
FROZEN_SECS="${WATCHDOG_FROZEN_SECS:-600}"
now="${WATCHDOG_NOW:-$(date +%s)}"

# Newest *.jsonl mtime (epoch int) under a dir, or 0 when none/absent.
newest_jsonl_mtime() {
    local dir="$1" m
    [ -d "$dir" ] || { echo 0; return 0; }
    m=$(find "$dir" -name '*.jsonl' -printf '%T@\n' 2>/dev/null | sort -nr | head -1 || true)
    [ -z "$m" ] && { echo 0; return 0; }
    echo "${m%.*}"   # drop the fractional part → integer epoch
}

# Current deliverable count (top-level files only).
count=0
if [ -d "$RESULTS_DIR" ]; then
    count=$(find "$RESULTS_DIR" -maxdepth 1 -type f 2>/dev/null | wc -l | tr -d ' ')
fi

newest_mtime="$(newest_jsonl_mtime "$TRANSCRIPT_DIR")"
age=$(( now - newest_mtime ))
[ "$age" -lt 0 ] && age=0

# Prior poll baseline (count + newest mtime). Absent → 0/0 (first poll).
prev_count=0
prev_mtime=0
if [ -f "$STATE_FILE" ]; then
    read -r prev_count prev_mtime < "$STATE_FILE" 2>/dev/null || true
    prev_count="${prev_count:-0}"
    prev_mtime="${prev_mtime:-0}"
fi

# --- classify ----------------------------------------------------------------
if [ "$age" -le "$FRESH_SECS" ]; then
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
esac

# --- emit --------------------------------------------------------------------
if [ "$NUDGE" -eq 1 ]; then
    # Silent unless frozen (a SessionStart nudge that fires every poll is noise).
    if [ "$verdict" = "frozen" ]; then
        echo "## === workflow fleet frozen: ${slug} ==="
        echo "Newest agent transcript static for ${age}s (≥ ${FROZEN_SECS}s) and no new"
        echo "results in ${RESULTS_DIR} (${count} file(s)). Per workflow-fleets.md"
        echo "§ Stall watchdog this is FROZEN, not crawl — waiting buys nothing."
        echo "Action: kill the fleet, then enter the § Salvage runbook (transcripts survive)."
    fi
else
    echo "workflow-watchdog[${slug}]: ${verdict} — ${count} result(s), newest transcript ${age}s ago (${action})."
fi
exit 0
