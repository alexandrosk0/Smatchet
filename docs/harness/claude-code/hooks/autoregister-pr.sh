#!/usr/bin/env bash
# autoregister-pr.sh — PostToolUse(Bash) hook (Claude Code).
#
# When a Bash tool call created a GitHub PR (`gh pr create`), auto-register the
# new PR with smatchet-merge-watcher so the daemon WATCHES it — gate-polling,
# stuck-nudges, escalation. Closes the "shipped a PR but forgot to register it
# with the watcher" gap in the post-ship protocol (AGENTS.md § Autonomous
# ship-loop). Idempotent: `merge-watch register` no-ops if already registered.
#
# WATCH-ONLY — this hook must NEVER pass `--authorized`. Registration is not
# merge consent: an unauthorized entry is polled and nudged but never flipped
# ready and never merged. Adding `--authorized` here would auto-authorize every
# agent-opened PR at creation time and make the post-ship 4-option question
# decorative — the exact bypass this split fixed (2026-08-16 P1
# docs/self-improvement/categories/process/
# 2026-08-16-watcher-autoregister-bypasses-merge-consent.md, observed on
# #2027/#2031). Merge rights come only from an explicit user act:
# `merge-watch authorize <pr>` / `register --authorized` via post-ship option 3,
# in-session "merge when green", or the opt-in watch-all-PRs ship-time helper
# (agents/scripts/core/watch-register-if-enabled.sh).
#
# Copied into .claude/hooks/ by `bash agents/scripts/core/setup-harness.sh claude-code` and
# wired as a PostToolUse(Bash) hook by docs/harness/claude-code/settings.json.tmpl.
#
# Behaviour: no-op for any Bash command that isn't `gh pr create`; no-op when no
# PR number can be parsed; no-op if agents/scripts/core/merge-watcher-cli.py or python is
# absent. Always exits 0 — never blocks the turn. jq is NOT assumed on PATH.

set -uo pipefail

input="$(cat)"

# Only act on PR creation.
case "$input" in
    *"gh pr create"*) ;;
    *) exit 0 ;;
esac

# `gh pr create` prints the new PR URL (…/pull/<N>); grep the hook payload for it.
pr="$(printf '%s' "$input" | grep -oE 'pull/[0-9]+' | grep -oE '[0-9]+' | head -1)"
[ -n "$pr" ] || exit 0

proj="${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel 2>/dev/null || true)}"
[ -n "$proj" ] && cd "$proj" 2>/dev/null || exit 0

cli="agents/scripts/core/merge-watcher-cli.py"
[ -f "$cli" ] || exit 0
# shellcheck source=agents/scripts/core/lib/resolve-py.sh
. "agents/scripts/core/lib/resolve-py.sh" 2>/dev/null || exit 0
py="$(resolve_py)" || exit 0

out="$("$py" "$cli" register "$pr" 2>&1 || true)"
# Sanitize for embedding in a JSON string: backslashes (Windows clone paths like
# C:\…) and double-quotes would otherwise produce invalid JSON. Map \ -> / and
# drop " and CR, then clip. (CR #509.)
msg="$(printf '%s' "$out" | head -1 | tr '\\' '/' | tr -d '\r"' | cut -c1-80)"
printf '{"systemMessage": "merge-watcher: auto-registered PR #%s WATCH-ONLY (not authorized to merge; `merge-watch authorize %s` to grant) — %s"}\n' "$pr" "$pr" "$msg"
exit 0
