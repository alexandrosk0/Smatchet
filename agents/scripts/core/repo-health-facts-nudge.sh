#!/usr/bin/env bash
# repo-health-facts-nudge.sh — SessionStart nudge when the session-maintained
# half of the repo-health dashboard (tools/repo-health/facts.json: CI lane
# statuses, open-PR gate states, campaign verdicts) has gone stale.
#
# Why: generate.py recomputes tree-derivable metrics every run, but facts.json
# is refreshed only by a session with GitHub MCP access — its own README admits
# the rot risk. A dashboard rendering weeks-old gate states as current is worse
# than no dashboard for the human-on-the-loop visibility role AI_POLICY.md
# assigns it, so this nags (like followup-due-nudge.sh) until someone refreshes.
#
# Staleness signal: git-commit age of facts.json (robust to checkout mtime
# resets, same approach as followup-due-nudge.sh's file-age trigger). The
# per-section `updated` stamps inside facts.json drive the dashboard's own
# freshness badge; a refresh touches both, so one signal here suffices.
#
# Modes: --nudge (default; silent when fresh) / --list (always prints a line).
# Env: REPO_HEALTH_FACTS_FILE (default tools/repo-health/facts.json),
#      REPO_HEALTH_FACTS_MAX_AGE_DAYS (default 7),
#      REPO_HEALTH_NOW (epoch override, for deterministic tests).
#
# Advisory — exit 0 always; untracked file / no git → silent skip.

set -uo pipefail
cd "$(dirname "$0")/../../.." || exit 0

MODE="nudge"
case "${1:-}" in
    --list) MODE="list" ;;
    --nudge|"") MODE="nudge" ;;
    *) echo "usage: repo-health-facts-nudge.sh [--list|--nudge]" >&2; exit 2 ;;
esac

FACTS="${REPO_HEALTH_FACTS_FILE:-tools/repo-health/facts.json}"
MAX_DAYS="${REPO_HEALTH_FACTS_MAX_AGE_DAYS:-7}"
case "$MAX_DAYS" in ''|*[!0-9]*) exit 0 ;; esac

[ -f "$FACTS" ] || exit 0
last="$(git log -1 --format='%ct' -- "$FACTS" 2>/dev/null || true)"
case "$last" in ''|*[!0-9]*) exit 0 ;; esac

now="${REPO_HEALTH_NOW:-$(date +%s 2>/dev/null || echo 0)}"
case "$now" in ''|*[!0-9]*) exit 0 ;; esac
age=$(( (now - last) / 86400 ))

if [ "$age" -lt "$MAX_DAYS" ]; then
    [ "$MODE" = "list" ] && echo "repo-health-facts-nudge: fresh — $FACTS refreshed ${age}d ago (threshold ${MAX_DAYS}d)."
    exit 0
fi

printf '🔔 === repo-health facts stale (%sd ≥ %sd) ===\n' "$age" "$MAX_DAYS"
printf '  %s (CI lanes / open PRs / campaign verdicts) was last refreshed %s days ago;\n' "$FACTS" "$age"
printf '  the dashboard renders those as current. Refresh via GitHub MCP per\n'
printf '  tools/repo-health/README.md § Keeping facts.json fresh, stamp the `updated`\n'
printf '  dates, re-run generate.py, and republish to the artifact URL.\n'
exit 0
