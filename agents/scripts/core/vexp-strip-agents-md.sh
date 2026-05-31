#!/bin/bash
# vexp-strip-agents-md: remove the vexp tool's auto-regenerated MCP block from
# AGENTS.md (which is the harness-agnostic root per the agents.md spec).
#
# The vexp installer / updater writes the same block into THREE places:
#   1. AGENTS.md                                      <- WRONG: tracked, cross-harness noise
#   2. docs/harness/claude-code/CLAUDE.md.tmpl        <- tracked, source for .claude/CLAUDE.md
#   3. .claude/CLAUDE.md                              <- gitignored, regenerated locally
#
# Only #2 + #3 are correct (Claude-Code-specific harness surface). This script
# strips the block from #1. Re-run on every SessionStart to handle re-regen.
#
# Idempotent: exit 0 always. No-op when block is absent.
#
# Block format:
#   ## vexp <!-- vexp vX.Y.Z -->
#   ... 30+ lines ...
#   <!-- /vexp -->
#
# Trigger: SessionStart hook in `.claude/settings.json` (wired by
# `docs/harness/claude-code/settings.json.tmpl`).
#
# Related: docs/plans/shipped/unblock-external-blockers-2-3-4.md § Slice 2.

set -euo pipefail

AGENTS="${CLAUDE_PROJECT_DIR:-.}/AGENTS.md"

# Fast exit: file missing or block absent.
[ -f "$AGENTS" ] || exit 0
if ! grep -q '<!-- vexp v' "$AGENTS"; then
    exit 0
fi

# awk-strip: delete from "## vexp <!-- vexp v..." through "<!-- /vexp -->" inclusive.
# Also swallow the preceding blank line for tidy output.
awk '
    # Buffer one blank line; emit on next non-blank.
    /^[[:space:]]*$/ {
        if (skipping) next
        buf = buf $0 ORS
        next
    }
    # Block opener: drop buffered blank, enter skip mode.
    /^## vexp <!-- vexp v/ {
        skipping = 1
        buf = ""
        next
    }
    # Block closer: leave skip mode, drop the line itself.
    skipping && /<!-- \/vexp -->/ {
        skipping = 0
        next
    }
    # Inside block: drop everything.
    skipping { next }
    # Normal line: flush buffered blanks, then emit.
    {
        if (buf != "") {
            printf "%s", buf
            buf = ""
        }
        print
    }
    END {
        # Suppress trailing blank-line buffer to keep file tidy.
    }
' "$AGENTS" > "$AGENTS.tmp"

mv "$AGENTS.tmp" "$AGENTS"

echo "vexp-strip-agents-md: removed vexp block from AGENTS.md" >&2
exit 0
