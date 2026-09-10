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
# TWO MORE HOLES THIS CHECK NOW COVERS (plan agent-surface-extraction-repo,
# Phase A rows 4c + 5e). Once the agent surface is a submodule mounted at
# agent-layer/, two states are just as silent as the #913 one and, until now,
# just as invisible here — this script had ZERO content checks and could detect
# only the guard hook's total absence:
#
#   (i)  EMPTY LAYER. `git clone` without --recurse-submodules, or a CI checkout
#        without `submodules: recursive`, leaves agent-layer/ an empty directory.
#        Every `agents/...` path still "exists" as a string and nothing exists on
#        disk. That gets its OWN exit code (3) rather than reusing 1: a caller
#        must be able to tell "run setup-harness" from "run submodule update",
#        because running the wrong one of those fixes nothing.
#
#   (ii) STALE HARDLINKS. On Windows the adapter is provisioned with hardlinks,
#        which share an inode with the canonical file. `git submodule update`
#        does NOT edit in place — it checks out NEW files — so advancing the
#        submodule leaves every pre-existing .claude/agents/*.md hardlink on the
#        OLD inode, still serving YESTERDAY'S agent definition. The link is
#        present, the count is right, and the session silently runs stale rules.
#        Only a content comparison can see it, which is why the ordering at every
#        call site is submodule-update THEN setup-harness, never the reverse, and
#        why this check compares bytes instead of trusting existence.
#
# Usage:
#   check-harness-provisioned.sh [--quiet] [TREE]   # TREE defaults to git toplevel / cwd
#   check-harness-provisioned.sh --selftest
#
# Exit codes:
#   0 — guard hook present (provisioned) and agent links current, OR --selftest passed.
#   1 — guard hook ABSENT (unprovisioned) — warning printed to stderr.
#   2 — usage error / --selftest failed.
#   3 — agent LAYER missing/empty, or .claude/agents content is STALE relative to
#       the layer — the submodule was never initialised, or was advanced without
#       re-running setup-harness. Distinct from 1: the remedy is different.
#
# Warn-only by contract: it never mutates anything; callers decide whether the
# non-zero exit blocks. See docs/harness/SETUP.md § Concurrent-session HEAD-drift
# guard and docs/agent-rules/process-rules.md § Concurrent interactive sessions.
set -euo pipefail

# Dual-root bootstrap (row 3a): a location-relative climb, correct pre- and
# post-flip. Sourcing is best-effort — this script is documented as a bootstrap
# probe run on trees that may not be provisioned at all, so a missing or failing
# config must degrade to the original guard-hook-only behaviour rather than
# abort. AGENT_LAYER_ROOT then falls back to the script's own climb.
_chp_self_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
# shellcheck source=scripts/dev/project-config.sh
. "$_chp_self_root/scripts/dev/project-config.sh" 2>/dev/null || true
AGENT_LAYER_ROOT="${AGENT_LAYER_ROOT:-$_chp_self_root}"

# The content probe is itself LAYER content, so it can be missing for exactly the
# reason this script now reports. Source it when present and record its absence
# otherwise — a missing probe must surface as the layer state, never as a
# silently-skipped check (a dead net that always reads green).
_chp_have_probe=0
if [ -f "$_chp_self_root/agents/scripts/core/lib/agents-dir-current.sh" ]; then
    # shellcheck source=agents/scripts/core/lib/agents-dir-current.sh
    . "$_chp_self_root/agents/scripts/core/lib/agents-dir-current.sh"
    _chp_have_probe=1
fi

GUARD_REL=".claude/hooks/guard-head-drift.sh"
EXIT_LAYER=3

usage() {
    cat <<'EOF'
Usage: check-harness-provisioned.sh [--quiet] [TREE]
       check-harness-provisioned.sh --selftest

Warn when TREE's concurrent-session HEAD-drift guard
(.claude/hooks/guard-head-drift.sh) is not wired, or when TREE's agent layer is
missing/empty or its .claude/agents links have gone stale. TREE defaults to the
git toplevel (or the current directory).

Exit 0 wired and current, 1 guard hook absent, 2 usage error, 3 layer
missing/empty or agent links stale.
EOF
}

# Layer predicate + warning. Returns 0 when $1's agent layer is populated AND
# $1/.claude/agents matches it byte-for-byte, else EXIT_LAYER (3).
#
# It is deliberately SEPARATE from the guard-hook check and reported before it:
# an empty layer makes the guard-hook remedy (`setup-harness.sh`) impossible to
# run — post-flip that script only exists once the submodule is checked out — so
# telling the caller to run it first would send them down a dead end.
#
# $layer_root is $1's OWN layer, not this script's: TREE can name a sibling
# worktree, and each worktree gets an independent submodule checkout (they do not
# share objects), so probing our own tree would report the wrong one's state.
check_layer() {
    local tree="$1" quiet="$2" layer_root="$3" agents_dest="$1/.claude/agents"

    # Empty layer. `-d` alone is not enough: an uninitialised submodule leaves the
    # mount point present and empty, which is exactly the state to catch. A
    # missing content probe (lib/agents-dir-current.sh) is the same state seen
    # from the other side — that file lives in the layer too.
    if [ "$_chp_have_probe" -eq 0 ] || [ -z "$(ls -A "$layer_root/agents/core" 2>/dev/null)" ]; then
        {
            echo "⚠ agent layer MISSING or EMPTY: $layer_root/agents/core has no agent definitions."
            echo "  The agent surface is a git submodule; a clone or CI checkout without it"
            echo "  leaves the mount point present but empty, so every agents/... path still"
            echo "  looks fine as a string and resolves to nothing on disk."
            echo "  Fix (in this order — setup-harness.sh does not exist until the first line runs):"
            echo "      git submodule update --init --recursive"
            echo "      bash agents/scripts/core/setup-harness.sh claude-code"
        } >&2
        return "$EXIT_LAYER"
    fi

    # Content freshness. Skipped when .claude/agents was never built at all —
    # that is the unprovisioned state check_tree already reports, and claiming
    # "stale" for it would name the wrong remedy.
    if [ -d "$agents_dest" ] && ! agents_dir_current "$agents_dest" "$layer_root"; then
        {
            echo "⚠ agent links STALE: $agents_dest does not match $layer_root/agents/{core,project}/."
            echo "  On Windows these are HARDLINKS, which share an inode with the canonical"
            echo "  file. \`git submodule update\` checks out NEW files rather than editing in"
            echo "  place, so advancing the layer leaves every existing link on the OLD inode,"
            echo "  still serving the previous agent definitions — present, correctly counted,"
            echo "  and silently wrong. Re-link:"
            echo "      bash agents/scripts/core/setup-harness.sh claude-code"
        } >&2
        return "$EXIT_LAYER"
    fi

    [ "$quiet" = "1" ] || echo "check-harness-provisioned: OK — agent layer populated and .claude/agents current"
    return 0
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
    local tmp rc=0 lay
    tmp="$(mktemp -d)"
    # Unprovisioned tree → non-zero.
    # selftest: asserts-failure
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

    # --- check_layer (rows 4c + 5e) -----------------------------------------
    # An EMPTY layer mount — the "clone forgot --recurse-submodules" state — must
    # be distinguishable from an absent guard hook, so assert the code, not just
    # non-zero. Reusing 1 here is the bug this row exists to prevent.
    lay="$tmp/layer"
    mkdir -p "$lay/agents/core" "$lay/agents/project"
    # selftest: asserts-failure
    check_layer "$tmp" 1 "$lay" >/dev/null 2>&1
    if [ "$?" -ne "$EXIT_LAYER" ]; then
        echo "selftest FAIL: empty layer should return $EXIT_LAYER (not 1 — different remedy)" >&2
        rc=1
    fi

    # Populated layer, no .claude/agents yet → 0. Absence is check_tree's story;
    # reporting it as "stale" here would name the wrong fix.
    printf 'v1\n' > "$lay/agents/core/a.md"
    if ! check_layer "$tmp" 1 "$lay" >/dev/null 2>&1; then
        echo "selftest FAIL: populated layer with no .claude/agents should return zero" >&2
        rc=1
    fi

    # Current links → 0.
    mkdir -p "$tmp/.claude/agents"
    cp "$lay/agents/core/a.md" "$tmp/.claude/agents/a.md"
    if ! check_layer "$tmp" 1 "$lay" >/dev/null 2>&1; then
        echo "selftest FAIL: matching .claude/agents should return zero" >&2
        rc=1
    fi

    # The row 4c case, reproduced exactly: the layer's file is REPLACED (as a
    # submodule checkout does) while the copy keeps the old bytes. Existence and
    # count are both still correct — only the content check can see it.
    printf 'v2\n' > "$lay/agents/core/a.md"
    # selftest: asserts-failure
    check_layer "$tmp" 1 "$lay" >/dev/null 2>&1
    if [ "$?" -ne "$EXIT_LAYER" ]; then
        echo "selftest FAIL: stale agent content should return $EXIT_LAYER" >&2
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

# TREE's own layer. Pre-flip that is TREE itself (agents/ is in-tree); post-flip
# the submodule mount under it. Resolved per-TREE rather than from this script's
# AGENT_LAYER_ROOT because TREE can name a sibling worktree with its own checkout.
LAYER_ROOT="$TREE"
[ -d "$TREE/agent-layer/agents" ] && LAYER_ROOT="$TREE/agent-layer"

# Layer first: an empty layer makes check_tree's remedy unrunnable, so reporting
# the guard hook ahead of it would send the caller to a script that is not there.
layer_rc=0
check_layer "$TREE" "$QUIET" "$LAYER_ROOT" || layer_rc="$?"
tree_rc=0
check_tree "$TREE" "$QUIET" || tree_rc="$?"

# Layer wins the exit code: it is the more fundamental breakage and names the
# step that must happen first.
if [ "$layer_rc" -ne 0 ]; then exit "$layer_rc"; fi
exit "$tree_rc"
