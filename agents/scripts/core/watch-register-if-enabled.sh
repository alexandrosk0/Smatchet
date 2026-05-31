#!/usr/bin/env bash
# watch-register-if-enabled.sh <pr>
# ----------------------------------------------------------------------------
# Ship-loop hook: auto-register a freshly-opened PR with smatchet-merge-watcher
# IFF SMATCHET_WATCH_ALL_PRS is set truthy. Default (unset) is a no-op.
#
# Why opt-in, not always-on: registering a PR with the watcher IS the explicit
# authorization to auto-merge it (per AGENTS.md merge-gates + docs/agent-rules/
# ship-loops.md — the watcher runs the gate-check + REST-squash-merge on
# GATES_PASSED). Auto-registering EVERY PR would auto-merge everything with no
# per-PR review gate. This flag lets a user opt the whole session into
# hands-off ("I don't want to keep poking it") without weakening the default
# explicit-authorization model. The orchestrator calls this right after
# `gh pr create`; with the flag unset it prints a one-liner and returns 0, so
# the post-ship menu (or in-session "merge when green") still governs.
#
# Truthy values (case-insensitive): 1, true, yes, on. Anything else = off.
#
# Exit codes:
#   0 — registered, OR no-op (flag off), OR already-registered (benign)
#   2 — usage error (missing <pr>) or registration error (e.g. not a git repo)
# ----------------------------------------------------------------------------

set -uo pipefail

PR="${1:-}"
if [ -z "$PR" ]; then
    echo "usage: watch-register-if-enabled.sh <pr>" >&2
    exit 2
fi

flag="$(printf '%s' "${SMATCHET_WATCH_ALL_PRS:-}" | tr '[:upper:]' '[:lower:]')"
case "$flag" in
    1 | true | yes | on) ;;
    *)
        echo "watch-register: SMATCHET_WATCH_ALL_PRS not set — leaving PR #${PR} unregistered (explicit-authorization model in effect; use the post-ship menu or 'merge when green')."
        exit 0
        ;;
esac

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON="${PYTHON:-python}"

"$PYTHON" "$HERE/merge-watcher-cli.py" register "$PR"
rc=$?

# register exits 1 only on "already registered" (cmd_register) — benign for a
# ship-time auto-register that may re-run on a re-pushed branch. Other failures
# (e.g. cwd not a git repo) surface as main() exit 2 and propagate.
if [ "$rc" -eq 1 ]; then
    echo "watch-register: PR #${PR} already registered — nothing to do."
    exit 0
fi
exit "$rc"
