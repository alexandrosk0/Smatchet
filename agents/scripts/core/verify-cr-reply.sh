#!/usr/bin/env bash
# verify-cr-reply.sh — confirm CR-cited file changes actually landed on origin.
#
# Closes the silent-drop class documented in:
#   docs/self-improvement/categories/applied.md
#     2026-05-28 process P1 — Mid-task `git stash` + branch switch silently
#     drops uncommitted CR-fix work
#   commit 639e2a9 — recovery push after CR triage reply cited code that
#   origin never received.
#
# The orchestrator (or human) calls this before posting a CR-triage reply that
# claims "fixes shipped in <commit>". The script diffs each cited file between
# origin/<branch> and origin/develop. If a cited file is byte-identical to
# develop, the fix did NOT land on the PR branch — exit non-zero.
#
# Usage:
#   bash scripts/dev/verify-cr-reply.sh <branch> <file> [<file>...]
#
# Examples:
#   bash scripts/dev/verify-cr-reply.sh feat/my-pr-branch agents/scripts/core/merge-gates.sh
#   bash scripts/dev/verify-cr-reply.sh fix/foo Source/Core/src/Foo.cpp Source/Core/include/Foo.h
#
# Exit codes:
#   0 — every cited file differs between origin/<branch> and origin/develop (good)
#   1 — at least one cited file is identical to origin/develop (CR fix didn't land)
#   2 — usage error / git not available / branch not found on origin

set -euo pipefail

if ! command -v git >/dev/null 2>&1; then
    echo "verify-cr-reply: git not on PATH" >&2
    exit 2
fi

if [ $# -lt 2 ]; then
    echo "Usage: $0 <branch> <file> [<file>...]" >&2
    echo "  Verifies each <file> on origin/<branch> differs from origin/develop." >&2
    exit 2
fi

branch="$1"
shift

# Refresh remote so we don't compare against a stale local cache.
git fetch --quiet origin "$branch" develop 2>/dev/null || {
    echo "verify-cr-reply: failed to fetch origin/$branch and origin/develop" >&2
    exit 2
}

# Confirm the branch actually exists on origin.
if ! git rev-parse --verify "origin/$branch" >/dev/null 2>&1; then
    echo "verify-cr-reply: origin/$branch not found" >&2
    exit 2
fi

failed=0
checked=0

for file in "$@"; do
    checked=$((checked + 1))

    # Use git diff exit code: 0 = identical, 1 = differs.
    if git diff --quiet "origin/develop" "origin/$branch" -- "$file"; then
        echo "FAIL: $file is identical on origin/$branch and origin/develop — CR fix did NOT land"
        failed=$((failed + 1))
    else
        echo "OK:   $file differs between origin/$branch and origin/develop"
    fi
done

echo ""
echo "Checked: $checked  Failed: $failed"

if [ "$failed" -gt 0 ]; then
    echo ""
    echo "Do NOT post the CR-triage reply until the failing files are committed + pushed." >&2
    exit 1
fi

exit 0
