#!/usr/bin/env bash
# check-harness-provisioned.sh — warn when a tree's concurrent-session
# HEAD-drift guard is NOT wired (the #913 fresh-clone bootstrap hole).
#
# The guard (docs/harness/claude-code/hooks/guard-head-drift.sh) — the
# PreToolUse hook that blocks an Edit/Write/commit when another session, your
# terminal, or a janitor moves the shared HEAD under you — lives in the
# GITIGNORED .claude/ adapter. Nothing in a fresh clone wires it: .claude/ is
# provisioned ONLY by `setup-harness.sh claude-code`. So on a brand-new clone
# the "protected even if you forget a worktree" guarantee (PR #913) does not
# hold until setup-harness has run once. This check surfaces that hole loudly so
# it is not silently relied upon. The guard cannot self-bootstrap (a fresh clone
# has no hooks to fire), which is why this is a separate, manually/launcher-run
# probe.
#
# Usage:
#   check-harness-provisioned.sh [--quiet] [TREE]   # TREE defaults to git toplevel / cwd
#   check-harness-provisioned.sh --selftest
#
# Exit codes:
#   0 — guard hook present (provisioned), OR --selftest passed.
#   1 — guard hook ABSENT (unprovisioned) — warning printed to stderr.
#   2 — usage error / --selftest failed.
#
# Warn-only by contract: it never mutates anything; callers decide whether the
# non-zero exit blocks. See docs/harness/SETUP.md § Concurrent-session HEAD-drift
# guard and docs/agent-rules/process-rules.md § Concurrent interactive sessions.
set -euo pipefail

GUARD_REL=".claude/hooks/guard-head-drift.sh"

usage() {
    cat <<'EOF'
Usage: check-harness-provisioned.sh [--quiet] [TREE]
       check-harness-provisioned.sh --selftest

Warn when TREE's concurrent-session HEAD-drift guard
(.claude/hooks/guard-head-drift.sh) is not wired. TREE defaults to the git
toplevel (or the current directory). Exit 1 when unprovisioned, 0 when wired.
EOF
}

# Core predicate + warning. Returns 0 when $1's guard hook is present, else 1.
check_tree() {
    local tree="$1" quiet="$2"
    if [ -f "$tree/$GUARD_REL" ]; then
        [ "$quiet" = "1" ] || echo "check-harness-provisioned: OK — HEAD-drift guard wired in $tree"
        return 0
    fi
    {
        echo "⚠ harness NOT provisioned: $tree/$GUARD_REL is missing."
        echo "  The concurrent-session HEAD-drift guard is INACTIVE here — a sibling"
        echo "  session, your terminal, or a janitor moving HEAD will NOT be blocked,"
        echo "  so an Edit/commit can land on the wrong branch (PR #913 bootstrap hole)."
        echo "  Fix once per clone:"
        echo "      bash agents/scripts/core/setup-harness.sh claude-code"
        echo "  Details: docs/harness/SETUP.md § Concurrent-session HEAD-drift guard."
    } >&2
    return 1
}

selftest() {
    local tmp rc=0
    tmp="$(mktemp -d)"
    # Unprovisioned tree → non-zero.
    if check_tree "$tmp" 1 >/dev/null 2>&1; then
        echo "selftest FAIL: unprovisioned tree should return non-zero" >&2
        rc=1
    fi
    # Provisioned tree → zero.
    mkdir -p "$tmp/.claude/hooks"
    : > "$tmp/$GUARD_REL"
    if ! check_tree "$tmp" 1 >/dev/null 2>&1; then
        echo "selftest FAIL: provisioned tree should return zero" >&2
        rc=1
    fi
    rm -rf "$tmp"
    if [ "$rc" -eq 0 ]; then echo "check-harness-provisioned: selftest OK"; fi
    return "$rc"
}

QUIET=0
TREE=""
while [ $# -gt 0 ]; do
    case "$1" in
        --quiet)    QUIET=1 ;;
        --selftest) if selftest; then exit 0; else exit 2; fi ;;
        -h|--help)  usage; exit 0 ;;
        --*)        usage >&2; exit 2 ;;
        *)          TREE="$1" ;;
    esac
    shift
done

if [ -z "$TREE" ]; then
    TREE="$(git rev-parse --show-toplevel 2>/dev/null || pwd)"
fi

check_tree "$TREE" "$QUIET"
