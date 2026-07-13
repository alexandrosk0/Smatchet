#!/usr/bin/env bash
# agents/scripts/core/smatchet-notify.sh
# ----------------------------------------------------------------------------
# Notification dispatcher for smatchet-merge-watcher Phase 4a (per
# `docs/plans/shipped/smatchet-merge-watcher.md`).
#
# Tries three channels in order:
#   1. Smatchet in-app toast via localhost HTTP POST (Phase 4b — currently
#      no-op since the endpoint hasn't been built yet; we still attempt the
#      POST so wiring is in place + retries become free once 4b lands).
#   2. Windows native toast via `smatchet-notify-windows.ps1` (BurntToast).
#   3. File log under $LOCALAPPDATA/Smatchet/merge-watch/notifications.log
#      (or $XDG_STATE_HOME/smatchet/merge-watch/...) — always succeeds when
#      the FS is writable; gives the user a paper trail when neither
#      Smatchet nor BurntToast is available.
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

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PR=""
STATE=""
MESSAGE=""
PR_URL=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --pr) PR="$2"; shift 2 ;;
        --pr=*) PR="${1#--pr=}"; shift ;;
        --state) STATE="$2"; shift 2 ;;
        --state=*) STATE="${1#--state=}"; shift ;;
        --message) MESSAGE="$2"; shift 2 ;;
        --message=*) MESSAGE="${1#--message=}"; shift ;;
        --pr-url) PR_URL="$2"; shift 2 ;;
        --pr-url=*) PR_URL="${1#--pr-url=}"; shift ;;
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
    if curl -fsS --max-time 3 --connect-timeout 2 \
            -X POST -H "Content-Type: application/json" \
            -d "$body" "$NOTIFY_URL" >/dev/null 2>&1; then
        echo "smatchet-notify: in-app toast dispatched to ${NOTIFY_URL}"
        success=1
    fi
fi

# Channel 2: Windows native via BurntToast (foreground OS notification).
# Always attempted as a fallback — Smatchet may not be running, or the
# in-app toast may have been missed. Set SMATCHET_NOTIFY_NO_WINDOWS_TOAST=1 to
# opt out (users who don't want OS toasts; deterministic channel-skip in tests
# where BurntToast may or may not be installed).
if [[ -z "${SMATCHET_NOTIFY_NO_WINDOWS_TOAST:-}" \
      && ( "${OSTYPE:-}" == "msys"* || "${OSTYPE:-}" == "cygwin"* || -n "${WINDIR:-}" ) ]]; then
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
            echo "smatchet-notify: Windows native toast failed. Set up once: powershell -ExecutionPolicy Bypass -File agents/scripts/core/merge-watcher-notify-setup.ps1" >&2
        fi
    fi
fi

# Channel 3: persistent file log. Always succeeds — gives the user a paper
# trail when neither Smatchet nor BurntToast is available. Without this,
# the daemon spams `notify: failed (exit 1)` every cycle on hosts that
# haven't installed BurntToast (the recommended PS module that ships
# Windows native toasts; `Install-Module BurntToast -Scope CurrentUser`).
NOTIFY_LOG_DIR=""
if [[ -n "${LOCALAPPDATA:-}" ]]; then
    NOTIFY_LOG_DIR="$LOCALAPPDATA/Smatchet/merge-watch"
elif [[ -n "${XDG_STATE_HOME:-}" ]]; then
    NOTIFY_LOG_DIR="$XDG_STATE_HOME/smatchet/merge-watch"
elif [[ -n "${HOME:-}" ]]; then
    NOTIFY_LOG_DIR="$HOME/.local/state/smatchet/merge-watch"
fi
if [[ -n "$NOTIFY_LOG_DIR" ]]; then
    mkdir -p "$NOTIFY_LOG_DIR" 2>/dev/null || true
    NOTIFY_LOG="$NOTIFY_LOG_DIR/notifications.log"
    ts=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
    # Gate the dispatch echo on the printf's own exit status — not on
    # the cumulative `success` flag, which may already be 1 from
    # Channel 1/2. Without this, the echo prints "file-log dispatched"
    # even when printf silently failed (permissions, disk full, etc.).
    # CR finding on PR #392.
    if printf '[%s] PR#%s %s — %s%s\n' \
            "$ts" "$PR" "$STATE" "$MESSAGE" \
            "${PR_URL:+ ($PR_URL)}" >> "$NOTIFY_LOG" 2>/dev/null; then
        success=1
        echo "smatchet-notify: file-log dispatched to ${NOTIFY_LOG}"
    fi
fi

if [[ $success -eq 0 ]]; then
    # Truly last resort — no LOCALAPPDATA / HOME, no file-log channel.
    # Write to stderr so the daemon's log captures something.
    echo "smatchet-notify: ALL channels failed for PR #${PR} state=${STATE}: ${MESSAGE}" >&2
    exit 1
fi
exit 0
