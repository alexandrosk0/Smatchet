#!/usr/bin/env bash
# clear-session-context.sh — archive + truncate .session-context.md at SessionStart.
#
# Wired to the SessionStart hook in .claude/settings.json. Before resetting,
# the prior scratchpad is moved to .session-context.archive/<ts>-<sid8>.md
# whenever it carries at least one `## ` heading from a SubagentStop append
# (banner-only scratchpads are skipped — nothing worth keeping).
#
# After archival, writes a fresh banner with the session id (passed by Claude
# Code in stdin JSON, falls back to env CLAUDE_SESSION_ID, then "unknown")
# and the start timestamp.
#
# Subagents do not write to .session-context.md directly. The SubagentStop
# hook (agent-token-log.py) appends a header block when the agent's report
# carries a `## Session context append` section.
#
# Cross-session recall: see agents/_shared/skills/scratchpad-recall/SKILL.md.
#
# Silent on success; never blocks the user.

set -euo pipefail

PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
SCRATCHPAD="$PROJECT_DIR/.session-context.md"
ARCHIVE_DIR="$PROJECT_DIR/.session-context.archive"

# Try to read session id from stdin JSON if Claude Code provides one.
SESSION_ID="${CLAUDE_SESSION_ID:-unknown}"
if [ ! -t 0 ]; then
    RAW="$(cat || true)"
    if [ -n "$RAW" ]; then
        # crude extract: "session_id":"<value>"
        EXTRACTED="$(printf '%s' "$RAW" \
            | sed -n 's/.*"session_id"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
            | head -n 1 || true)"
        if [ -n "$EXTRACTED" ]; then
            SESSION_ID="$EXTRACTED"
        fi
    fi
fi

TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

# Archive prior scratchpad when it has at least one `## ` heading
# (which only SubagentStop appends produce — banner has no such line).
if [ -f "$SCRATCHPAD" ] && grep -qE '^## ' "$SCRATCHPAD" 2>/dev/null; then
    mkdir -p "$ARCHIVE_DIR" 2>/dev/null || true  # never block SessionStart on a read-only/full FS
    # Extract prior session id from the banner's `_Session: <id> ·` line.
    PRIOR_SID="$(sed -n 's/^_Session:[[:space:]]*\([^[:space:]·]*\).*/\1/p' \
        "$SCRATCHPAD" | head -n 1)"
    [ -z "$PRIOR_SID" ] && PRIOR_SID="unknown"
    # Short 8-char prefix keeps filenames bounded.
    SID8="$(printf '%s' "$PRIOR_SID" | cut -c1-8)"
    # Filename-safe timestamp (colons -> dashes).
    TS_FS="$(printf '%s' "$TS" | tr ':' '-')"
    ARCHIVE_PATH="$ARCHIVE_DIR/${TS_FS}-${SID8}.md"
    mv -f "$SCRATCHPAD" "$ARCHIVE_PATH" 2>/dev/null || true
fi

cat > "$SCRATCHPAD" <<EOF || true
# Session context

_Session: ${SESSION_ID} · started: ${TS}_
_Append-only. The SubagentStop hook (agent-token-log.py) writes one header block per subagent when its report carries \`## Session context append\`. Never edit prior entries directly; the orchestrator reads this file to seed downstream delegations. Prior sessions are archived to \`.session-context.archive/\` — recall via \`scratchpad-recall\` skill._

EOF

# --- Deferred-lint hook state: discard prior-session leftovers --------------
# .lint-queue.<pid> / .lint-queue.lock are written by the inline + drain hooks
# (lint-cpp.sh / lint-cpp-drain.sh). Any orphaned entries from a crashed
# session are stale — discard rather than re-lint at startup. .tree-dirty is
# advisory and likewise stale across sessions.
rm -f "$PROJECT_DIR"/.claude/.lint-queue.* 2>/dev/null || true
rm -f "$PROJECT_DIR/.claude/.lint-queue.lock" 2>/dev/null || true
rm -f "$PROJECT_DIR/.claude/.tree-dirty" 2>/dev/null || true

# --- Sync deployed hooks from canonical sources ----------------------------
# .claude/ is gitignored and populated by setup-harness.sh, but a git pull or
# cherry-pick can update docs/harness/claude-code/hooks/* without a follow-up
# setup-harness.sh run. Silently copy on every SessionStart to prevent stale
# deployed hooks from masking improvements to the canonical templates.
HOOKS_SRC="$PROJECT_DIR/docs/harness/claude-code/hooks"
HOOKS_DST="$PROJECT_DIR/.claude/hooks"
if [ -d "$HOOKS_SRC" ] && [ -d "$HOOKS_DST" ]; then
    for src_file in "$HOOKS_SRC"/*.sh "$HOOKS_SRC"/*.py; do
        [ -f "$src_file" ] || continue
        dst_file="$HOOKS_DST/$(basename "$src_file")"
        # Only overwrite when the canonical differs to avoid spurious mtimes.
        if ! cmp -s "$src_file" "$dst_file" 2>/dev/null; then
            cp -f "$src_file" "$dst_file" 2>/dev/null || true
        fi
    done
fi

# --- Sync deployed settings.json hooks from the template -------------------
# Same staleness gap as the hook-script loop above, but for settings.json: a
# git pull can add a NEW governance hook to the template that an already-
# provisioned (user-modified) .claude/settings.json never receives — because
# setup-harness.sh's copy_template refuses to overwrite a user-modified file.
# Additively heal the missing hooks here (never touches permissions / existing
# hooks / order). Silent when already in sync. See sync-settings-hooks.sh.
SETTINGS_TMPL="$PROJECT_DIR/docs/harness/claude-code/settings.json.tmpl"
SETTINGS_DST="$PROJECT_DIR/.claude/settings.json"
if [ -f "$SETTINGS_TMPL" ] && [ -f "$SETTINGS_DST" ]; then
    bash "$PROJECT_DIR/agents/scripts/core/sync-settings-hooks.sh" \
        "$SETTINGS_TMPL" "$SETTINGS_DST" >/dev/null || true
fi

# --- P4-mode banner: surface SMATCHET_AGENT_VCS=p4 at session start --------
# Per AGENTS.md § Autonomous ship-loop default § P4-gated variant: when the
# user opts into p4-mode via the env var, the orchestrator MUST follow the
# P4-gated ship-loop (docs/agent-rules/ship-loops.md § P4-gated ship-loop) for
# ALL subsequent task-loops in this session, even when the user's prompt is
# git-flavoured (PR numbers, gh URLs, etc.). Emit a banner so the orchestrator
# can't miss the mode at boot. p4 info failure routes through the
# ship-loops.md exit-2 banner per § P4-gated ship-loop.
if [ "${SMATCHET_AGENT_VCS:-}" = "p4" ]; then
    # Gate on `p4 info`'s exit status, not stdout content — a failing p4 server
    # can still emit text to stdout ("Perforce client error:" banner) and the
    # prior `[ -n "$P4_INFO" ]` form would misroute that as ACTIVE.
    if P4_INFO="$(p4 info 2>/dev/null)"; then
        P4_CLIENT="$(printf '%s\n' "$P4_INFO" | sed -n 's/^Client name:[[:space:]]*//p' | head -n 1)"
        P4_PORT="$(printf '%s\n' "$P4_INFO" | sed -n 's/^Server address:[[:space:]]*//p' | head -n 1)"
        cat >> "$SCRATCHPAD" <<P4EOF || true

## === p4-mode ACTIVE ===

client: ${P4_CLIENT:-unknown}
server: ${P4_PORT:-unknown}

Orchestrator MUST follow the P4-gated ship-loop per
\`docs/agent-rules/ship-loops.md\` § P4-gated ship-loop. Ask the sub-variant
question (small-change vs task-stream) before any slice subagent. Avoid
\`git worktree add\` — use \`agents/scripts/project/p4-task-stream.sh\` instead. This
overrides prompt-driven mode inference even when the user mentions PR
numbers or gh URLs.

P4EOF
    else
        cat >> "$SCRATCHPAD" <<P4EOF || true

## === p4-mode REQUESTED but UNREACHABLE ===

\`SMATCHET_AGENT_VCS=p4\` set, but \`p4 info\` failed. Follow
\`docs/perforce/SETUP.md\` to bootstrap, OR fall back to default git ship-loop
for this session (orchestrator MUST ask the user via \`AskUserQuestion\` —
never silently downgrade).

P4EOF
    fi
fi

# --- Loop-mode + auto-merge banner: surface the active governance modes --------
# Per AI_POLICY.md § Two loop modes: the human selects human-on-the-loop
# (action-biased, autonomous) or human-in-the-loop (execute only within an
# approved plan; pause at undocumented decisions). Resolution order for each
# mode:
#   1. the matching env var (SMATCHET_LOOP_MODE / SMATCHET_AUTOMERGE) — per-session
#      override, else
#   2. project.config.json governance.<mode>  (committed operator default), else
#   3. the conservative fallback (loop_mode `in` / auto_merge `off`).
# Mirrors the p4-mode banner above so the orchestrator can't miss the modes at boot.
_gov() { python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["governance"].get(sys.argv[2],sys.argv[3]))' "$PROJECT_DIR/project.config.json" "$1" "$2" 2>/dev/null || echo "$2"; }
_loop_mode="${SMATCHET_LOOP_MODE:-$(_gov loop_mode in)}"
_auto_merge="${SMATCHET_AUTOMERGE:-$(_gov auto_merge off)}"
if [ "$_loop_mode" = "on" ]; then
    cat >> "$SCRATCHPAD" <<LOOPEOF || true

## === loop-mode: on ===

human-on-the-loop (action-biased). Commit / push / open-PR autonomously;
resolve reversible forks with a default + surface them; pause only on the
enumerated ship-loop exceptions (incl. (6) cannot-autonomously-validate /
cost-unbounded — escalate). See \`AI_POLICY.md\` § Two loop modes.

LOOPEOF
else
    cat >> "$SCRATCHPAD" <<LOOPEOF || true

## === loop-mode: in ===

human-in-the-loop (prerelease default). Execute ONLY within an approved plan;
pause at each decision point the plan does not cover; do not improvise scope.
Escalate (don't assume) on anything not autonomously validatable —
ship-loop exception (6). See \`AI_POLICY.md\` § Two loop modes + § Escalate.

LOOPEOF
fi

if [ "$_auto_merge" = "on" ]; then
    cat >> "$SCRATCHPAD" <<'LOOPEOF' || true

## === auto-merge: on ===

Standing grant (project.config.json § governance.auto_merge). After a feature PR
is opened READY (not draft) and the full merge-gates poll passes, squash-merge it
WITHOUT the per-PR post-ship merge prompt — do not ask the user to mark-ready or
to authorise the merge. The gates still bind (CI + CodeRabbit + Bugbot + user
comments); only the asking is removed. A rate-limited / usage-capped CR or Bugbot
is an out-of-band condition, not a finding: apply the named `cr-out-of-band` /
`bugbot-out-of-band` label and proceed. STILL halt + escalate on a real blocker
(red required check, a genuine CR/Bugbot finding, an unresolved user comment) and
on the standard ship-loop pause exceptions.

LOOPEOF
fi

exit 0
