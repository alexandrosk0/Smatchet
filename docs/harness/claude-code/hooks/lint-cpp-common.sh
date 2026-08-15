#!/usr/bin/env bash
# Shared library for the lint-cpp inline + drain hooks. Source from
# .claude/hooks/lint-cpp.sh (PostToolUse) and .claude/hooks/lint-cpp-drain.sh
# (Stop). NOT directly executable. Keeps the heavy tools (cppcheck, clang-tidy,
# dual-target syntax) defined in one place so the two hooks cannot drift.

# Resolve project directory; callers must have CLAUDE_PROJECT_DIR set.
LINT_PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
LINT_NORM_PROJ="${LINT_PROJ_DIR//\\//}"
if command -v cygpath >/dev/null 2>&1; then
    LINT_NORM_PROJ="$(cygpath -m "$LINT_NORM_PROJ" 2>/dev/null || printf '%s' "$LINT_NORM_PROJ")"
fi

# Lint toolchain (clang-format / cppcheck / clang-tidy / gcc) is installed from
# MSYS2 UCRT64 on this machine at C:\msys64\ucrt64\bin. Hook invocations
# inherit the shell PATH, which often does NOT include that directory. Without
# this prepend the hooks silently `command -v`-skip every tool. Idempotent.
# Note: this is the LINT toolchain path — the build presets use MSVC/Clang.
SMATCHET_TOOLCHAIN_BIN="${SMATCHET_TOOLCHAIN_BIN:-/c/msys64/ucrt64/bin}"
if [[ -d "$SMATCHET_TOOLCHAIN_BIN" ]]; then
    case ":$PATH:" in
        *":$SMATCHET_TOOLCHAIN_BIN:"*) : ;;
        *) PATH="$SMATCHET_TOOLCHAIN_BIN:$PATH"; export PATH ;;
    esac
fi

# lint_normalize_path <raw_file_path>
# Echoes the project-relative forward-slash path on stdout. Empty string when
# the path falls outside the project root or normalisation fails.
lint_normalize_path() {
    local raw="$1"
    [[ -z "$raw" ]] && { printf ''; return; }
    local norm="${raw//\\//}"
    if command -v cygpath >/dev/null 2>&1; then
        norm="$(cygpath -m "$norm" 2>/dev/null || printf '%s' "$norm")"
    fi
    local rel="${norm#$LINT_NORM_PROJ/}"
    # If the strip didn't happen, the file is outside the project — emit empty.
    if [[ "$rel" == "$norm" ]]; then
        printf ''
        return
    fi
    printf '%s' "$rel"
}

# lint_abs_path <raw_file_path>
# Echoes the absolute mixed-Windows form (C:/...) suitable for tool invocation.
lint_abs_path() {
    local raw="$1"
    local norm="${raw//\\//}"
    if command -v cygpath >/dev/null 2>&1; then
        norm="$(cygpath -m "$norm" 2>/dev/null || printf '%s' "$norm")"
    fi
    printf '%s' "$norm"
}

# lint_is_first_party <rel_path>
# Returns 0 if the relative path is a first-party C/C++ source/header,
# non-zero otherwise. Match list mirrors the prior inline-only hook.
# Vendored upstream code under Source/Core/ThirdParty/ is excluded so the
# pre-existing findings in third-party TUs (ImGuiColorTextEdit, etc.) don't
# block edits to small, marked Smatchet patches inside them.
lint_is_first_party() {
    case "$1" in
        Source/Core/ThirdParty/*) return 1 ;;
        Source/Core/*.cpp|Source/Core/*.h|Source/Core/**/*.cpp|Source/Core/**/*.h) return 0 ;;
        Source/Plugins/*.cpp|Source/Plugins/*.h|Source/Plugins/**/*.cpp|Source/Plugins/**/*.h) return 0 ;;
        Source/Standalone/*.cpp|Source/Standalone/*.h|Source/Standalone/**/*.cpp|Source/Standalone/**/*.h) return 0 ;;
        tests/*.cpp|tests/*.h|tests/**/*.cpp|tests/**/*.h) return 0 ;;
        *) return 1 ;;
    esac
}

# lint_format_issues <issues_blob>
# Reads multi-line issue text on stdin (preferred) or as $1; dedups identical
# lines, caps output at SMATCHET_LINT_MAX_LINES (default 120; 0 = unlimited).
# Trailer added when truncation kicks in. Output to stdout.
lint_format_issues() {
    local issues
    if [[ $# -gt 0 ]]; then
        issues="$1"
    else
        issues="$(cat)"
    fi
    local max_lines="${SMATCHET_LINT_MAX_LINES:-120}"
    printf '%s' "$issues" | awk -v max="$max_lines" '
        NF && !seen[$0]++ { lines[++n] = $0 }
        END {
            limit = (max ~ /^[0-9]+$/ && max > 0) ? max : n
            for (i = 1; i <= n && i <= limit; ++i) print lines[i]
            if (n > limit) {
                printf("lint-cpp: truncated %d duplicate-filtered diagnostic lines; rerun hook manually for full output.\n", n - limit)
            }
        }'
}

# lint_changed_lines <abs_file>
# Echoes the line numbers of <abs_file> that differ from the delta baseline
# (default origin/develop), one per line — the added/modified side of each diff
# hunk. Returns NON-ZERO only when the baseline can't be resolved (no git / no
# origin/develop) so the caller fails OPEN; a resolvable baseline on an UNCHANGED
# file returns 0 with EMPTY output (so every finding grandfathers).
lint_changed_lines() {
    local abs="$1" base="${LINT_DELTA_BASE:-origin/develop}" rel
    command -v git >/dev/null 2>&1 || return 1
    git -C "$LINT_NORM_PROJ" rev-parse --verify --quiet "$base" >/dev/null 2>&1 || return 1
    rel="${abs#"$LINT_NORM_PROJ"/}"   # repo-relative pathspec for git
    # -U0 → hunk ranges are exactly the changed lines. Each `@@ -a,b +c,d @@`
    # adds lines c..c+d-1 (d defaults to 1 when the `,d` is omitted).
    git -C "$LINT_NORM_PROJ" diff -U0 "$base" -- "$rel" 2>/dev/null \
      | awk '/^@@/ {
            h = $0; sub(/^@@ -[0-9,]+ \+/, "", h); sub(/ @@.*$/, "", h)
            split(h, p, ","); start = p[1] + 0; cnt = (p[2] == "" ? 1 : p[2] + 0)
            for (i = 0; i < cnt; i++) print start + i
        }'
}

# lint_filter_delta <abs_file>   (findings on stdin)
# Passes through only findings whose `:<line>:` is among <abs_file>'s changed-vs-
# baseline lines — mirrors the merge-gate's grandfathering of pre-existing code
# so the Stop-hook drain stops re-flagging untouched functions / a post-switch
# working tree (categories/tooling.md: lint-cpp-drain delta-awareness). Fails
# OPEN (everything through) when the baseline is unavailable, so a non-git
# checkout keeps the old behaviour rather than dropping real new findings. A
# line-less finding (no `:<n>:`) is always kept — it can't be delta-located.
lint_filter_delta() {
    local abs="$1" lines
    if ! lines="$(lint_changed_lines "$abs")"; then cat; return 0; fi
    awk -v L="$lines" '
        BEGIN { n = split(L, a, "\n"); for (i = 1; i <= n; i++) if (a[i] != "") keep[a[i]] = 1 }
        { if (match($0, /:[0-9]+:/)) { ln = substr($0, RSTART + 1, RLENGTH - 2); if (ln in keep) print }
          else print }'
}

# lint_run_cppcheck <abs_file>
# Echoes cppcheck findings (empty on clean), DELTA-FILTERED to the session's
# changed lines. Skipped silently if the tool is missing.
lint_run_cppcheck() {
    local abs="$1"
    command -v cppcheck >/dev/null 2>&1 || return 0
    { cppcheck \
        --enable=warning,style,performance,portability \
        --inconclusive \
        --inline-suppr \
        --suppress=missingIncludeSystem \
        --suppress=unusedFunction \
        --suppress=unusedStructMember \
        --language=c++ \
        --std=c++14 \
        --quiet \
        --template='cppcheck: {file}:{line}: [{severity}] {id}: {message}' \
        "$abs" 2>&1 || true; } | lint_filter_delta "$abs"
}

# lint_run_clang_tidy <abs_file> <rel_path>
# Echoes clang-tidy findings (empty on clean). Picks the test build dir for
# tests/* sources, the iter build dir otherwise. Prefers ninja-iter-clang over
# ninja-iter-msvc so clang-tidy resolves system headers via clang-native paths.
# Skipped silently when no compile_commands.json is found in any candidate dir.
# Headers (.h) are skipped — they are checked transitively when their .cpp TUs
# are linted, and running clang-tidy directly on headers produces spurious
# 'file not found' errors for standard headers because no TU context is injected.
lint_run_clang_tidy() {
    local abs="$1" rel="$2"
    command -v clang-tidy >/dev/null 2>&1 || return 0
    # Skip header files; they are covered transitively via their including TUs.
    [[ "$rel" == *.h ]] && return 0
    local build_dir
    if [[ "$rel" == tests/* ]]; then
        # For tests prefer the clang test preset; fall back to MSVC test preset.
        if [[ -f "$LINT_NORM_PROJ/build/ninja-test-clang/compile_commands.json" ]]; then
            build_dir="$LINT_NORM_PROJ/build/ninja-test-clang"
        else
            build_dir="$LINT_NORM_PROJ/build/ninja-test-msvc"
        fi
    else
        # Prefer clang iter preset; fall back to MSVC iter preset.
        if [[ -f "$LINT_NORM_PROJ/build/ninja-iter-clang/compile_commands.json" ]]; then
            build_dir="$LINT_NORM_PROJ/build/ninja-iter-clang"
        else
            build_dir="$LINT_NORM_PROJ/build/ninja-iter-msvc"
        fi
    fi
    [[ -f "$build_dir/compile_commands.json" ]] || return 0
    local out
    out="$(clang-tidy -p "$build_dir" --quiet "$abs" 2>/dev/null || true)"
    # Filter clang-diagnostic-error (PCH mismatch / missing system headers when
    # the linting toolchain differs from the build toolchain). Real compile
    # errors are caught by cmake --build; the lint hook surfaces tidy checks only.
    # Delta-filter to the session's changed lines (same grandfathering as cppcheck).
    printf '%s' "$out" | grep -E '(warning|error):' | grep -v '\[clang-diagnostic-error\]' \
        | lint_filter_delta "$abs" || true
}

# lint_run_catch_all <abs_file>
# Echoes lint-catch-all.py findings. Flags empty catch(...) blocks and
# catch(...) without LOG_ calls unless suppressed by // catch-all-ok:.
lint_run_catch_all() {
    local abs="$1" py
    # shellcheck source=agents/scripts/core/lib/resolve-py.sh
    . "$LINT_NORM_PROJ/agents/scripts/core/lib/resolve-py.sh" 2>/dev/null || return 0
    py="$(resolve_py)" || return 0
    "$py" "$LINT_NORM_PROJ/.claude/hooks/lint-catch-all.py" "$abs" 2>&1 || true
}

# lint_run_dual_target <abs_file> <rel_path>
# Echoes lint-syntax-both.py findings. Only runs for .cpp outside tests/.
lint_run_dual_target() {
    local abs="$1" rel="$2"
    [[ "$rel" == *.cpp && "$rel" != tests/* ]] || return 0
    local py
    # shellcheck source=agents/scripts/core/lib/resolve-py.sh
    . "$LINT_NORM_PROJ/agents/scripts/core/lib/resolve-py.sh" 2>/dev/null || return 0
    py="$(resolve_py)" || return 0
    "$py" "$LINT_NORM_PROJ/.claude/hooks/lint-syntax-both.py" "$abs" 2>&1 || true
}
