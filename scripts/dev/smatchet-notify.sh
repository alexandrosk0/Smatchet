#!/usr/bin/env bash
# scripts/dev/smatchet-notify.sh
# ----------------------------------------------------------------------------
# Notification dispatcher for smatchet-merge-watcher Phase 4a (per
# `docs/design/smatchet-merge-watcher.md`).
#
# Tries two channels in order:
#   1. Smatchet in-app toast via localhost HTTP POST (Phase 4b — currently
#      no-op since the endpoint hasn't been built yet; we still attempt the
#      POST so wiring is in place + retries become free once 4b lands).
#   2. Windows native toast via `smatchet-notify-windows.ps1` (BurntToast).
#
# Inputs (env or args):
#   --pr <N>           PR number
#   --state <STATE>    one of CI_FAIL, CONFLICT, USER_COMMENT, TIMEOUT_NO_CR,
#                      TRIAGE_BUDGET_EXHAUSTED
#   --message <text>   human-readable detail
#   --pr-url <url>     optional PR URL for click-through
#
# Exit: 0 if at least one channel succeeded; non-zero if all failed.
# ----------------------------------------------------------------------------

set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PR=""
STATE=""
MESSAGE=""
PR_URL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pr) PR="$2"; shift 2 ;;
        --state) STATE="$2"; shift 2 ;;
        --message) MESSAGE="$2"; shift 2 ;;
        --pr-url) PR_URL="$2"; shift 2 ;;
        --help|-h)
            grep -E '^# ' "$0" | head -25 | sed 's/^# //'
            exit 0
            ;;
        *)
            echo "smatchet-notify: unknown arg: $1" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$PR" || -z "$STATE" ]]; then
    echo "smatchet-notify: --pr and --state are required" >&2
    exit 2
fi
[[ -z "$MESSAGE" ]] && MESSAGE="(no message)"

SMATCHET_NOTIFY_HOST="${SMATCHET_NOTIFY_HOST:-127.0.0.1}"
SMATCHET_NOTIFY_PORT="${SMATCHET_NOTIFY_PORT:-7679}"
NOTIFY_URL="http://${SMATCHET_NOTIFY_HOST}:${SMATCHET_NOTIFY_PORT}/merge-watch/notify"

success=0

# Channel 1: Smatchet in-app toast (Phase 4b endpoint).
# We probe with a 2s connect timeout — if Smatchet isn't running OR the
# endpoint isn't built yet, curl exits non-zero fast + we fall through.
if command -v curl >/dev/null 2>&1; then
    body=$(printf '{"pr":%s,"state":"%s","message":"%s","pr_url":"%s"}' \
        "$PR" "$STATE" "${MESSAGE//\"/\\\"}" "$PR_URL")
    if curl -sS --max-time 3 --connect-timeout 2 \
            -X POST -H "Content-Type: application/json" \
            -d "$body" "$NOTIFY_URL" >/dev/null 2>&1; then
        echo "smatchet-notify: in-app toast dispatched to ${NOTIFY_URL}"
        success=1
    fi
fi

# Channel 2: Windows native via BurntToast (foreground OS notification).
# Always attempted as a fallback — Smatchet may not be running, or the
# in-app toast may have been missed.
if [[ "${OSTYPE:-}" == "msys"* || "${OSTYPE:-}" == "cygwin"* || -n "${WINDIR:-}" ]]; then
    PS_SCRIPT="$SCRIPT_DIR/smatchet-notify-windows.ps1"
    if [[ -f "$PS_SCRIPT" ]]; then
        # Quote MESSAGE for PowerShell single-quoted string by doubling embedded single-quotes.
        ps_msg="${MESSAGE//\'/\'\'}"
        ps_url="${PR_URL//\'/\'\'}"
        if powershell -NoProfile -ExecutionPolicy Bypass -File "$PS_SCRIPT" \
                -PR "$PR" -State "$STATE" -Message "$ps_msg" -PrUrl "$ps_url" 2>/dev/null; then
            echo "smatchet-notify: Windows native toast dispatched"
            success=1
        else
            echo "smatchet-notify: Windows native toast failed (BurntToast module missing? Install: Install-Module BurntToast -Scope CurrentUser)" >&2
        fi
    fi
fi

if [[ $success -eq 0 ]]; then
    # Last resort: write to stderr so the daemon's log captures something.
    echo "smatchet-notify: ALL channels failed for PR #${PR} state=${STATE}: ${MESSAGE}" >&2
    exit 1
fi
exit 0
