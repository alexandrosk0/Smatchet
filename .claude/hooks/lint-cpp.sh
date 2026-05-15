#!/usr/bin/env bash
# PostToolUse hook: lint C++ after Edit/Write.
# - Filters to first-party .cpp/.h files only.
# - Auto-formats in place via clang-format.
# - Reports cppcheck + clang-tidy issues on stderr (exit 2 surfaces them to Claude).
# - Degrades gracefully if any tool is missing.

set -u

# --- Read tool input JSON from stdin ---------------------------------------
INPUT="$(cat || true)"
if [[ -z "$INPUT" ]]; then exit 0; fi

# Extract file_path. Prefer jq when present; fall back to sed.
if command -v jq >/dev/null 2>&1; then
    FILE_PATH="$(printf '%s' "$INPUT" | jq -r '.tool_input.file_path // empty')"
else
    FILE_PATH="$(printf '%s' "$INPUT" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -n1)"
fi
[[ -z "$FILE_PATH" ]] && exit 0

# --- Normalize to a forward-slash relative path under the project ----------
PROJECT_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
# Convert Windows backslashes for matching.
NORM_FILE="${FILE_PATH//\\//}"
NORM_PROJ="${PROJECT_DIR//\\//}"
# Under MSYS2 / Cygwin, PROJECT_DIR arrives as /c/... while FILE_PATH arrives
# as C:/... — normalise both to mixed-Windows form (C:/...) so the prefix
# strip matches. cygpath -m is idempotent across /c/..., C:/..., and C:\... .
if command -v cygpath >/dev/null 2>&1; then
    NORM_PROJ="$(cygpath -m "$NORM_PROJ" 2>/dev/null || printf '%s' "$NORM_PROJ")"
    NORM_FILE="$(cygpath -m "$NORM_FILE" 2>/dev/null || printf '%s' "$NORM_FILE")"
fi
REL="${NORM_FILE#$NORM_PROJ/}"

# --- Filter: first-party C/C++ only ----------------------------------------
case "$REL" in
    Source_Core/*.cpp|Source_Core/*.h|Source_Core/**/*.cpp|Source_Core/**/*.h) ;;
    Plugins/*.cpp|Plugins/*.h|Plugins/**/*.cpp|Plugins/**/*.h) ;;
    Target_Standalone/*.cpp|Target_Standalone/*.h|Target_Standalone/**/*.cpp|Target_Standalone/**/*.h) ;;
    *) exit 0 ;;
esac

ABS_FILE="$NORM_FILE"
[[ -f "$ABS_FILE" ]] || exit 0

ISSUES=""
add_issues() { ISSUES+="$1"$'\n'; }
format_issues() {
    local max_lines="${SMATCHET_LINT_MAX_LINES:-120}"
    printf '%s' "$ISSUES" | awk -v max="$max_lines" '
        NF && !seen[$0]++ { lines[++n] = $0 }
        END {
            limit = (max ~ /^[0-9]+$/ && max > 0) ? max : n
            for (i = 1; i <= n && i <= limit; ++i) print lines[i]
            if (n > limit) {
                printf("lint-cpp: truncated %d duplicate-filtered diagnostic lines; rerun hook manually for full output.\n", n - limit)
            }
        }'
}

# --- 1) clang-format -i (apply in place) -----------------------------------
if command -v clang-format >/dev/null 2>&1; then
    clang-format -i "$ABS_FILE" 2>/dev/null || true
fi

# --- 2) cppcheck (report only) ---------------------------------------------
if command -v cppcheck >/dev/null 2>&1; then
    CPPCHECK_OUT="$(cppcheck \
        --enable=warning,style,performance,portability \
        --inconclusive \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --std=c++14 \
        --quiet \
        --template='cppcheck: {file}:{line}: [{severity}] {id}: {message}' \
        "$ABS_FILE" 2>&1 || true)"
    [[ -n "$CPPCHECK_OUT" ]] && add_issues "$CPPCHECK_OUT"
fi

# --- 3) clang-tidy (report only, needs compile_commands.json) --------------
BUILD_DIR="$NORM_PROJ/build/ninja-iter-msys2"
if [[ -f "$BUILD_DIR/compile_commands.json" ]] && command -v clang-tidy >/dev/null 2>&1; then
    TIDY_OUT="$(clang-tidy -p "$BUILD_DIR" --quiet "$ABS_FILE" 2>/dev/null || true)"
    # Strip clang-tidy's "N warnings generated." footer noise when there are no actual diagnostics.
    TIDY_FILTERED="$(printf '%s' "$TIDY_OUT" | grep -E '(warning|error):' || true)"
    [[ -n "$TIDY_FILTERED" ]] && add_issues "$TIDY_FILTERED"
fi

# --- 4) Both-target syntax check (catches DX12 regressions instantly) ------
# Runs only for .cpp files; needs python + compile_commands.json. Header edits
# are validated via dependent .cpp edits.
if [[ "$REL" == *.cpp ]] && command -v python >/dev/null 2>&1; then
    SYNTAX_OUT="$(python "$NORM_PROJ/.claude/hooks/lint-syntax-both.py" "$ABS_FILE" 2>&1 || true)"
    [[ -n "$SYNTAX_OUT" ]] && add_issues "$SYNTAX_OUT"
fi

# --- Surface issues back to Claude -----------------------------------------
if [[ -n "$ISSUES" ]]; then
    {
        echo "lint-cpp: issues found in $REL — fix before responding."
        format_issues
    } >&2
    exit 2
fi

exit 0
