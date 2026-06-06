#!/usr/bin/env bash
# Stop hook: run the CI delta-lint gate (the pre-ship.sh format-then-gate's GATE
# half) at end-of-turn so a comment-noise / function-size / strict-zone violation
# is caught locally BEFORE it reaches CI — closing the recurring "build-green but
# CI-red" gap that the comment-noise gate is the #1 cause of
# (build-quality-velocity-hardening #7).
#
# Why the Stop hook (not push-time): clang-format -i happens inline at edit-time
# (PostToolUse lint-cpp.sh) and the heavy-lint drain (lint-cpp-drain.sh) runs
# FIRST in this same Stop event, so by the time we run, files are already
# formatted — i.e. the pre-ship "format FIRST, then gate" ORDER is preserved by
# hook ordering. We run the GATE only (test-lint-rules.sh --diff), never
# clang-format -i, because mutating files in a Stop hook would dirty an otherwise
# clean tree and fight check-main-repo-clean.sh.
#
# Non-trapping by construction — only fires in the "ready to push/PR" state:
#   * skip if SMATCHET_SKIP_STOP_GATE=1                (hard per-session escape)
#   * skip on develop/main or detached HEAD            (only feature branches)
#   * skip if the working tree is DIRTY                (mid-development; revisit
#                                                       once committed)
#   * skip if origin/develop is unresolvable           (offline / fresh clone)
#   * skip if HEAD is not ahead of origin/develop      (nothing to ship)
# In the clean-tree-ahead state a clean diff exits 0 silently; only a REAL gate
# failure blocks (exit 2) — exactly what CI would block — with a fix pointer.
# Loop terminates naturally: block -> you edit (tree dirties -> skip) -> commit
# the fix (gate re-checks -> passes -> stop succeeds).
#
# A genuine false-positive escapes via the existing per-rule grammar
# (SMATCHET_DEVIATION(rule=<id>; ...)) or SMATCHET_SKIP_STOP_GATE=1 for the session.

set -u

[ "${SMATCHET_SKIP_STOP_GATE:-0}" = "1" ] && exit 0

PROJ_DIR="${CLAUDE_PROJECT_DIR:-$(pwd)}"
cd "$PROJ_DIR" 2>/dev/null || exit 0

# Must be inside a git work tree.
git rev-parse --is-inside-work-tree >/dev/null 2>&1 || exit 0

branch="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
case "$branch" in
    develop | main | master | HEAD | "") exit 0 ;;
esac

# Mid-development (uncommitted changes) -> skip so we never trap an active edit loop.
[ -n "$(git status --porcelain 2>/dev/null)" ] && exit 0

# Need a resolvable origin/develop to diff against.
git rev-parse --verify --quiet origin/develop >/dev/null 2>&1 || exit 0

# Only when HEAD actually has commits beyond origin/develop (i.e. there is
# something to ship). `--count` of the right side of the symmetric range.
ahead="$(git rev-list --count origin/develop..HEAD 2>/dev/null || echo 0)"
[ "${ahead:-0}" -gt 0 ] || exit 0

LINT="$PROJ_DIR/agents/scripts/project/test-lint-rules.sh"
[ -f "$LINT" ] || exit 0

# Run the EXACT delta gate CI runs. Its exit code mirrors CI's verdict:
#   0 = clean (WARN-only is still 0), non-zero = a real blocking finding.
out="$(bash "$LINT" --diff origin/develop 2>&1)"
rc=$?
[ "$rc" -eq 0 ] && exit 0

{
    echo "pre-ship-stop-gate: the CI delta-lint gate FAILED on the committed branch diff —"
    echo "fix before pushing (this is the build-green/CI-red gap; #7)."
    echo "$out"
    echo "---"
    echo "Fix locally:  bash scripts/dev/pre-ship.sh        (formats + re-runs the gate)"
    echo "Per-finding escape: a // SMATCHET_DEVIATION(rule=<id>; reason=...; owner=...; revisit=...) above the line."
    echo "Session escape:     export SMATCHET_SKIP_STOP_GATE=1"
} >&2
exit 2
