#!/usr/bin/env bash
# test-adapter-drift.sh — assert each .claude/agents/<name>.md is byte-identical
# to its canonical agents/{core,project}/<name>.md.
#
# setup-harness.sh hardlinks the adapter copies to the canonical files, but
# editing a canonical via Edit/Write rewrites it with a NEW inode → the hardlink
# breaks → the adapter keeps the OLD content, so the harness spawns the stale
# model/prompt (confirmed: the code-review sonnet→opus/high change kept spawning
# sonnet for ~a day off a stale adapter copy — tooling self-improvement
# 2026-06-08 P2). test-agent-contract checks the CANONICAL banner↔frontmatter
# parity (both update together → it passes) but NOT adapter staleness; this gate
# closes that hole.
#
# LOCAL gate: the .claude adapter is gitignored / absent in CI, so this SKIPS
# cleanly when .claude/agents is absent. Auto-enrolled by scripts/dev/test-all.sh
# (the local full suite), NOT a GitHub job.
#
# Override root (testing): SMATCHET_ADAPTER_ROOT.
#
# Usage:
#   bash agents/scripts/core/test-adapter-drift.sh            # check the tree
#   bash agents/scripts/core/test-adapter-drift.sh --selftest
#
# Exit: 0 — every adapter copy in-sync (or adapter absent) · 1 — drift / a
#       missing adapter copy (or --selftest failure) · 2 — infra error.
#
# selftest: asserts-failure

set -uo pipefail

# Compare canonical agents/{core,project}/*.md under $root against
# $root/.claude/agents/<basename>. Echo drift/missing lines; 0 in-sync /
# 1 drift / returns 0 (skip) when the adapter dir is absent.
_check_adapter() {
    local root="$1"
    local adapter="$root/.claude/agents"
    [ -d "$adapter" ] || { echo "__SKIP__"; return 0; }
    local f base copy drift=0
    while IFS= read -r f; do
        [ -n "$f" ] || continue
        base="$(basename "$f")"
        [ "$base" = "README.md" ] && continue
        copy="$adapter/$base"
        if [ ! -f "$copy" ]; then
            echo "MISSING: $copy (canonical $f has no adapter copy — re-run setup-harness.sh)"
            drift=1
        elif ! cmp -s "$f" "$copy"; then
            echo "DRIFT: $f vs $copy differ (stale adapter — re-run setup-harness.sh)"
            drift=1
        fi
    done < <(find "$root/agents/core" "$root/agents/project" -maxdepth 1 -name '*.md' 2>/dev/null | sort)
    return "$drift"
}

if [ "${1:-}" = "--selftest" ]; then
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
    mkdir -p "$tmp/agents/core" "$tmp/.claude/agents"
    printf 'version: 5\nbanner\n' > "$tmp/agents/core/code-review.md"
    cp "$tmp/agents/core/code-review.md" "$tmp/.claude/agents/code-review.md"
    # In-sync -> pass.
    if ! _check_adapter "$tmp" >/dev/null; then
        echo "test-adapter-drift --selftest: FAIL — in-sync tree flagged"; exit 1
    fi
    # Edit the canonical (new content) -> adapter now stale -> MUST fail.
    printf 'version: 4\nstale\n' > "$tmp/agents/core/code-review.md"
    if _check_adapter "$tmp" >/dev/null; then
        echo "test-adapter-drift --selftest: FAIL — stale adapter not caught"; exit 1
    fi
    # Adapter dir absent -> SKIP (exit 0).
    rm -rf "$tmp/.claude"
    if [ "$(_check_adapter "$tmp")" != "__SKIP__" ]; then
        echo "test-adapter-drift --selftest: FAIL — absent adapter did not skip"; exit 1
    fi
    echo "test-adapter-drift --selftest: PASS — in-sync passes, stale fails, absent skips."
    exit 0
fi

ROOT="${SMATCHET_ADAPTER_ROOT:-}"
if [ -z "$ROOT" ]; then
    ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
fi

out="$(_check_adapter "$ROOT")"; rc=$?
if [ "$out" = "__SKIP__" ]; then
    echo "test-adapter-drift: SKIP — .claude/agents absent (harness not set up locally)."
    exit 0
fi
if [ "$rc" -eq 0 ]; then
    echo "test-adapter-drift: PASS — every .claude/agents/*.md matches its canonical."
    exit 0
fi
echo "test-adapter-drift: FAIL — adapter drift (re-run \`bash agents/scripts/core/setup-harness.sh claude-code\`):" >&2
printf '  %s\n' "$out" >&2
exit 1
