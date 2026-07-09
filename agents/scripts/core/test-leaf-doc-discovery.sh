#!/usr/bin/env bash
# test-leaf-doc-discovery.sh — regression net for per-subsystem leaf-doc
# nearest-wins discovery (docs/plans/shipped/per-subsystem-agent-docs.md).
#
# Two halves:
#   default (static, offline, auto-enrolled via the test-*.sh glob) — when the
#     Claude Code harness has been provisioned (any gitignored CLAUDE.md shim
#     exists under Source/Core/src/*/), assert a `@AGENTS.md` shim sits beside
#     EVERY leaf AGENTS.md, so setup-harness.sh's gen_subsystem_claude_shims()
#     regression-checks itself. Unprovisioned checkout (CI) -> skip, exit 0.
#   --live (opt-in, spends real agent tokens; never run by test-all) — headless
#     probe: spawn `claude -p` cd'd into Source/Core/src/Tracker/, ask for the
#     rule source of a Tracker invariant, assert the answer names the LEAF
#     AGENTS.md — proving the rules actually reach an agent touching that dir.
#
# Codex (native nearest-AGENTS.md) and Cursor (CONTEXT-MAP.md pointer) have no
# headless fixture here — documented residue in docs/harness/SETUP.md.
#
# Exit codes: 0 pass/skip · 1 failure · 2 required tool missing (--live only).

set -uo pipefail
cd "$(git rev-parse --show-toplevel)" || exit 2

MODE="static"
case "${1:-}" in
    --live) MODE="live" ;;
    --static|"") ;;
    *) echo "usage: test-leaf-doc-discovery.sh [--static|--live]" >&2; exit 2 ;;
esac

PASS=0; FAIL=0; NOTE=""

static_check() {
    local leaves shim have_any=0 leaf
    leaves="$(git ls-files 'Source/Core/src/*/AGENTS.md')"
    if [ -z "$leaves" ]; then
        NOTE="  (no leaf AGENTS.md files tracked — nothing to check)"
        return 0
    fi
    while IFS= read -r leaf; do
        [ -f "$(dirname "$leaf")/CLAUDE.md" ] && have_any=1
    done <<< "$leaves"
    if [ "$have_any" -eq 0 ]; then
        NOTE="  (skipped — no CLAUDE.md shims; run setup-harness.sh claude-code to provision)"
        return 0
    fi
    while IFS= read -r leaf; do
        shim="$(dirname "$leaf")/CLAUDE.md"
        if [ -f "$shim" ] && grep -q '^@AGENTS.md' "$shim"; then
            echo "  ok   shim beside $leaf"
            PASS=$((PASS + 1))
        else
            echo "  FAIL missing/malformed shim beside $leaf — re-run setup-harness.sh claude-code" >&2
            FAIL=$((FAIL + 1))
        fi
    done <<< "$leaves"
}

live_probe() {
    local dir="Source/Core/src/Tracker" out prompt
    command -v claude >/dev/null 2>&1 || {
        echo "leaf-doc-discovery: 'claude' CLI not on PATH — live probe unavailable." >&2
        echo "Passed: 0  Failed: 0  (skipped — claude missing)"
        exit 2
    }
    if ! grep -q '^@AGENTS.md' "$dir/CLAUDE.md" 2>/dev/null; then
        echo "leaf-doc-discovery: $dir/CLAUDE.md shim absent — run setup-harness.sh claude-code first." >&2
        echo "Passed: 0  Failed: 1"
        exit 1
    fi
    echo "leaf-doc-discovery: live probe (spawns a real headless session — token cost)"
    prompt="Read the file FieldCatalogCache.cpp in this directory. Then answer: which repo markdown file states the subsystem rule that all HTTP in this directory must route through TrackerHttpClient / TrackerHttpUtils? Reply with the single repo-relative file path only."
    out="$(cd "$dir" && claude -p "$prompt" --max-turns 8 2>/dev/null)" || {
        echo "  FAIL probe session errored" >&2
        echo "Passed: 0  Failed: 1"
        exit 1
    }
    printf 'probe answer: %s\n' "$out"
    if printf '%s' "$out" | grep -q 'Source/Core/src/Tracker/AGENTS\.md'; then
        echo "  ok   leaf AGENTS.md cited as the rule source"
        PASS=$((PASS + 1))
    else
        echo "  FAIL leaf AGENTS.md not cited (nearest-wins discovery broken?)" >&2
        FAIL=$((FAIL + 1))
    fi
}

case "$MODE" in
    static) static_check ;;
    live)   live_probe ;;
esac

echo "Passed: $PASS  Failed: $FAIL$NOTE"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
