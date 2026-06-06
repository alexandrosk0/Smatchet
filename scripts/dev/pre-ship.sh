#!/usr/bin/env bash
# scripts/dev/pre-ship.sh — one-shot pre-push gate for a feature branch.
#
# Closes the recurring "build-green != lint-green" gap (self-improvement
# process/P2): a targeted `cmake --build` / `ctest` does NOT run the comment-noise,
# function-size, or strict-zone delta gates — only test-all.sh and CI do. Authors
# (and decomposition subagents) repeatedly shipped build-green branches that failed
# CI on a bare-// comment-blank-run or a clang-format-reflowed comment.
#
# This wrapper enforces the correct ORDER: format FIRST, then run the exact delta
# gate CI runs — so a comment that clang-format reflows into a flagged shape is
# caught locally, not three minutes later in CI.
#
# Steps, over the first-party C++ files changed vs the base ref:
#   1. clang-format -i   (apply formatting in place)
#   2. test-lint-rules.sh --diff <base>   (comment-noise + function-size + strict-zone)
#
# It deliberately does NOT build or run tests — that is the caller's job and is
# already covered once-per-slice. This is purely the lint half that CI gates on
# but local builds skip.
#
# Usage:
#   bash scripts/dev/pre-ship.sh [<base-ref>]   # default base: origin/develop
#   bash scripts/dev/pre-ship.sh --help
#
# Exit codes:
#   0 — formatting applied (if any) and the delta lint gate passed.
#   1 — the delta lint gate reported a failure (fix before pushing).
#   2 — usage error, or a required tool (git / clang-format) is unavailable.
set -euo pipefail

usage() {
    cat <<'USAGE'
pre-ship.sh — format changed C++ then run the CI delta lint gate locally.

  bash scripts/dev/pre-ship.sh [<base-ref>]   default base: origin/develop
  bash scripts/dev/pre-ship.sh --help

Runs, over first-party C++ changed vs <base-ref>:
  1. clang-format -i
  2. agents/scripts/project/test-lint-rules.sh --diff <base-ref>
USAGE
}

base_ref="origin/develop"
case "${1:-}" in
    --help | -h)
        usage
        exit 0
        ;;
    "") ;;
    *) base_ref="$1" ;;
esac

repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "pre-ship: not inside a git work tree" >&2
    exit 2
}
cd "$repo_root"

if ! command -v clang-format >/dev/null 2>&1; then
    echo "pre-ship: clang-format not found on PATH" >&2
    exit 2
fi

# First-party C++ changed vs the merge-base with <base-ref> (staged, unstaged, and
# committed-on-branch). Restrict to the trees the gate scans; skip deletions.
mapfile -t changed < <(
    git diff --name-only --diff-filter=d "$base_ref"...HEAD -- \
        'Source/Core/*.cpp' 'Source/Core/*.h' \
        'Source/Plugins/*.cpp' 'Source/Plugins/*.h' \
        'Source/Standalone/*.cpp' 'Source/Standalone/*.h' \
        'tests/*.cpp' 'tests/*.h' 2>/dev/null
    git diff --name-only --diff-filter=d -- \
        'Source/Core/*.cpp' 'Source/Core/*.h' \
        'Source/Plugins/*.cpp' 'Source/Plugins/*.h' \
        'Source/Standalone/*.cpp' 'Source/Standalone/*.h' \
        'tests/*.cpp' 'tests/*.h' 2>/dev/null
)

# Deduplicate while preserving order, keeping only paths that still exist.
declare -A seen
fmt_targets=()
for f in "${changed[@]}"; do
    [ -n "$f" ] || continue
    [ -n "${seen[$f]:-}" ] && continue
    seen[$f]=1
    [ -f "$f" ] && fmt_targets+=("$f")
done

if [ "${#fmt_targets[@]}" -eq 0 ]; then
    echo "pre-ship: no changed first-party C++ vs $base_ref — formatting skipped"
else
    echo "pre-ship: clang-format -i on ${#fmt_targets[@]} changed file(s)"
    clang-format -i "${fmt_targets[@]}"
fi

echo "pre-ship: running delta lint gate vs $base_ref"
if ! bash agents/scripts/project/test-lint-rules.sh --diff "$base_ref"; then
    echo "pre-ship: FAIL — fix the delta lint findings above before pushing." >&2
    exit 1
fi

# Markdown style lint (MD028 etc.) — docs are not covered by the C++ delta gate,
# so without this a markdown issue only surfaced as a post-push CodeRabbit finding.
echo "pre-ship: running markdown lint (md_lint.py --all)"
if ! python3 agents/scripts/core/md_lint.py --all; then
    echo "pre-ship: FAIL — fix the markdown findings above before pushing." >&2
    exit 1
fi

# Test-list consistency (build-quality-velocity-hardening #5): a new
# tests/Core/*.test.cpp not added to tests/CMakeLists.txt is silently uncompiled
# (false green). The configure-time assert in tests/CMakeLists.txt catches it in
# CI; run the same check here so it surfaces before push, not at configure time.
echo "pre-ship: running test-list consistency check"
if ! bash agents/scripts/core/check-test-list.sh --check; then
    echo "pre-ship: FAIL — add the unreferenced test(s) to tests/CMakeLists.txt before pushing." >&2
    exit 1
fi

echo "pre-ship: PASS — formatted + delta lint gate + markdown lint + test-list clean. Safe to push."
exit 0
