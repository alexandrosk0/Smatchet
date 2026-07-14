#!/usr/bin/env bash
# memory-drain-nudge.sh — SessionStart nudge when the auto-memory inbox needs draining.
#
# The harness auto-memory dir (~/.claude/projects/<slug>/memory/) is a transient
# inbox, NOT a durable store (see docs/agent-rules/memory-drain.md + AGENTS.md
# § Weekly memory drain). Items captured there rot — they go stale against the
# code, duplicate AGENTS.md, or stay invisible to non-Claude-Code harnesses.
#
# A remote/cloud routine cannot drain it (the dir is machine-local, outside the
# repo). So instead of a calendar job, this SessionStart hook is event-driven:
# on each session start it checks the local inbox and prints a one-line nudge to
# stdout (which Claude Code injects into session context) when EITHER
#   - the inbox holds >= SMATCHET_MEMORY_DRAIN_COUNT live items (default 5), OR
#   - any live item is older than SMATCHET_MEMORY_DRAIN_DAYS days (default 7).
# Silent (no output, exit 0) when the inbox is empty or below both thresholds —
# never nags on an already-drained inbox.
#
# Overrides (env): SMATCHET_MEMORY_DIR, SMATCHET_MEMORY_DRAIN_COUNT,
# SMATCHET_MEMORY_DRAIN_DAYS.
#
# Never blocks the user; any failure exits 0 silently.

set -euo pipefail

THRESHOLD_COUNT="${SMATCHET_MEMORY_DRAIN_COUNT:-5}"
THRESHOLD_DAYS="${SMATCHET_MEMORY_DRAIN_DAYS:-7}"
# Guard against non-numeric env overrides — a bad value must not break the
# "silent on failure" contract with an `integer expression expected` error.
case "$THRESHOLD_COUNT" in ''|*[!0-9]*) THRESHOLD_COUNT=5 ;; esac
case "$THRESHOLD_DAYS" in ''|*[!0-9]*) THRESHOLD_DAYS=7 ;; esac

# Resolve the local auto-memory dir. Explicit override wins; otherwise glob the
# harness projects dir and pick the memory dir whose project slug names this
# repo (slug formatting varies by platform, so match on substring not exact).
MEM_DIR="${SMATCHET_MEMORY_DIR:-}"
if [ -z "$MEM_DIR" ]; then
    for d in "$HOME"/.claude/projects/*/memory; do
        [ -d "$d" ] || continue
        case "$(basename "$(dirname "$d")")" in
            *[Ss]matchet*) MEM_DIR="$d"; break ;;
        esac
    done
fi
[ -n "$MEM_DIR" ] && [ -d "$MEM_DIR" ] || exit 0

# Live items = top-level *.md except the MEMORY.md index itself.
COUNT="$(find "$MEM_DIR" -maxdepth 1 -type f -name '*.md' ! -name 'MEMORY.md' 2>/dev/null | wc -l | tr -d ' ' || true)"  # find can exit non-zero (unreadable entry) under pipefail; wc still emits a count, so || true (NOT || echo 0, which would double the output). numeric case below re-guards
case "$COUNT" in ''|*[!0-9]*) exit 0 ;; esac
[ "$COUNT" -gt 0 ] || exit 0

STALE="$(find "$MEM_DIR" -maxdepth 1 -type f -name '*.md' ! -name 'MEMORY.md' -mtime +"$THRESHOLD_DAYS" 2>/dev/null | wc -l | tr -d ' ' || true)"  # find can exit non-zero under pipefail; wc still emits a count, so || true (NOT || echo 0). numeric case below re-guards
case "$STALE" in ''|*[!0-9]*) STALE=0 ;; esac

if [ "$COUNT" -ge "$THRESHOLD_COUNT" ] || [ "$STALE" -gt 0 ]; then
    printf '🧹 Memory-drain due: %s item(s) in the auto-memory inbox (%s older than %sd). ' \
        "$COUNT" "$STALE" "$THRESHOLD_DAYS"
    printf 'Drain per docs/agent-rules/memory-drain.md — triage each into implement / backlog / toss, then clear. '
    printf "Run \`/drain-memory\` or just say 'drain memory'.\n"
fi

exit 0
