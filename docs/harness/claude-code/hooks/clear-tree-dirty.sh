#!/usr/bin/env bash
# PreToolUse:Bash hook — clear the .claude/.tree-dirty sentinel whenever a
# `cmake --build` command is about to run. Matches the leading-word position
# so `MSYS2_PATH_TYPE=inherit cmake --build …`, `cd foo && cmake --build …`,
# and bare `cmake --build …` all reset the sentinel before the build starts.
#
# Never blocks the bash invocation; always exits 0.

set -u

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
INPUT="$(cat || true)"
[[ -z "$INPUT" ]] && exit 0

if command -v jq >/dev/null 2>&1; then
    CMD="$(printf '%s' "$INPUT" | jq -r '.tool_input.command // empty')"
else
    CMD="$(printf '%s' "$INPUT" | sed -n 's/.*"command"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
fi
[[ -z "$CMD" ]] && exit 0

# Word-boundary match: `cmake` preceded by start-of-string or any non-word
# character, followed by whitespace then `--build`. Catches env-var prefixes
# and shell chains; rejects substring noise like `mycmake --build`.
if printf '%s' "$CMD" | grep -qE '(^|[^A-Za-z0-9_])cmake[[:space:]]+--build'; then
    rm -f "$PROJ_DIR/.claude/.tree-dirty" 2>/dev/null || true
fi

exit 0
