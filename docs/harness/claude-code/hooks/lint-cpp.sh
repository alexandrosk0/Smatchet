#!/usr/bin/env bash
# PostToolUse hook: inline phase of the lint-cpp pipeline.
# - Applies clang-format -i immediately (next edit reads the formatted file).
# - Appends the edited path to a per-PID queue file for the drain hook to
#   process at end-of-turn (Stop event → .claude/hooks/lint-cpp-drain.sh).
# - Marks the tree as dirty so agents reading .claude/.tree-dirty know edits
#   have happened since the last `cmake --build`.
#
# Escape hatch: SMATCHET_LINT_INLINE=1 reverts to the pre-deferred behaviour —
# clang-format + cppcheck + clang-tidy + dual-target run synchronously here.

set -u

# --- Read tool input JSON from stdin ---------------------------------------
INPUT="$(cat || true)"
[[ -z "$INPUT" ]] && exit 0

if command -v jq >/dev/null 2>&1; then
    FILE_PATH="$(printf '%s' "$INPUT" | jq -r '.tool_input.file_path // empty')"
else
    FILE_PATH="$(printf '%s' "$INPUT" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
fi
[[ -z "$FILE_PATH" ]] && exit 0

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
export CLAUDE_PROJECT_DIR="$PROJ_DIR"
# shellcheck source=lint-cpp-common.sh
. "$PROJ_DIR/.claude/hooks/lint-cpp-common.sh"

REL="$(lint_normalize_path "$FILE_PATH")"
[[ -z "$REL" ]] && exit 0
lint_is_first_party "$REL" || exit 0

ABS_FILE="$(lint_abs_path "$FILE_PATH")"
[[ -f "$ABS_FILE" ]] || exit 0

# --- 1) clang-format -i (always inline) ------------------------------------
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$ABS_FILE" 2>/dev/null || true
fi

# --- Mark tree dirty for the build slice-boundary rule ---------------------
: > "$PROJ_DIR/.claude/.tree-dirty" 2>/dev/null || true

# --- Inline-mode escape hatch ----------------------------------------------
if [[ "${SMATCHET_LINT_INLINE:-0}" == "1" ]]; then
    ISSUES=""
    cppcheck_out="$(lint_run_cppcheck "$ABS_FILE")"
    [[ -n "$cppcheck_out" ]] && ISSUES+="$cppcheck_out"$'\n'
    tidy_out="$(lint_run_clang_tidy "$ABS_FILE" "$REL")"
    [[ -n "$tidy_out" ]] && ISSUES+="$tidy_out"$'\n'
    dual_out="$(lint_run_dual_target "$ABS_FILE" "$REL")"
    [[ -n "$dual_out" ]] && ISSUES+="$dual_out"$'\n'
    catch_out="$(lint_run_catch_all "$ABS_FILE")"
    [[ -n "$catch_out" ]] && ISSUES+="$catch_out"$'\n'
    if [[ -n "$ISSUES" ]]; then
        {
            echo "lint-cpp: issues found in $REL — fix before responding."
            printf '%s' "$ISSUES" | lint_format_issues
        } >&2
        exit 2
    fi
    exit 0
fi

# --- Deferred path: append to per-PID queue, exit silently -----------------
# Each Claude Code instance (or parallel subagent) writes to its own queue
# file via $PPID. The drain hook (Stop) globs .claude/.lint-queue.* so all
# files are consumed regardless of which PID owned them.
QUEUE_ID="${PPID:-unknown}"
QUEUE_FILE="$PROJ_DIR/.claude/.lint-queue.$QUEUE_ID"

# Atomic line-append under POSIX PIPE_BUF (4096 bytes). One absolute path per
# line; dedup happens at drain time, not here.
printf '%s\n' "$ABS_FILE" >> "$QUEUE_FILE" 2>/dev/null || true

exit 0
