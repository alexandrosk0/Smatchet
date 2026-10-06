#!/usr/bin/env bash
# test-hook-layer-paths.sh — fail when a harness hook or hook template names an
# agent-layer path from the project dir.
#
# A hook runs with the PROJECT as its root ($CLAUDE_PROJECT_DIR, or the git top
# level for Codex). Before the flip the layer is that same tree, so a hook that
# writes "$CLAUDE_PROJECT_DIR/agents/scripts/…" works; after it the layer is the
# agent-layer/ submodule and the same path is not there. Nothing fails loudly:
# a settings.json command exits 127 at every session start, and a copied hook's
# `[ -f "$lib" ] || exit 0` turns a guard (plan-lock, shared-tree, the pre-ship
# Stop gate, the fleet preflight) into allow-all. The plan's flip probe found
# thirty of these (plan agent-surface-extraction-repo, Phase C prep 2). A hook
# reaches the layer through layer-root.sh / layer-run.sh, or, in a Codex command,
# through its own `l=` resolution; this gate keeps the old form from coming back.
#
# Scans every file under docs/harness/ in this script's own tree (the layer).
# Flags a layer path (agents/scripts|core|_shared, docs/agent-rules|harness)
# joined onto a project-dir anchor: $CLAUDE_PROJECT_DIR, $PROJ, $PROJ_DIR,
# $PROJECT_DIR, $LINT_NORM_PROJ, $ROOT, $REPO_ROOT, $root, $proj, with or without
# braces, a ${VAR:-default} fallback or JSON-escaped quotes. Lines that are comments are not flagged. A bare relative
# path after a `cd` to the project is out of its reach; the flip probe's hook rows
# (session-banner, hook-sync) and check-harness-provisioned's hook-path check run
# the hooks themselves.
#
# Usage:
#   bash agents/scripts/core/test-hook-layer-paths.sh             # scan
#   bash agents/scripts/core/test-hook-layer-paths.sh --selftest
#
# Exit: 0 clean · 1 a hit (or a --selftest failure) · 2 infra error.
#
# selftest: asserts-failure

set -uo pipefail

ANCHOR_RE='\$\{?(CLAUDE_PROJECT_DIR|PROJ|PROJ_DIR|PROJECT_DIR|LINT_NORM_PROJ|ROOT|REPO_ROOT|root|proj)(:-[^}]*)?\}?(\\?")?/(agents/(scripts|core|_shared)|docs/(agent-rules|harness))/'

# scan <dir> — print each offending file:line; 0 clean, 1 a hit.
scan() {
    local hits
    [ -d "$1" ] || { echo "test-hook-layer-paths: no such directory: $1" >&2; return 2; }
    hits="$(grep -rnE -- "$ANCHOR_RE" "$1" 2>/dev/null | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#' || true)"
    if [ -n "$hits" ]; then
        printf '%s\n' "$hits"
        return 1
    fi
    return 0
}

selftest() {
    local tmp rc=0
    tmp="$(mktemp -d "${TMPDIR:-/tmp}/test-hook-layer-paths.XXXXXX")" || return 2
    mkdir -p "$tmp/hooks"
    printf '%s\n' 'HOOK_LAYER="$PROJ"' '_lib="$HOOK_LAYER/agents/scripts/core/lib.sh"' \
        '# was: _lib="$PROJ/agents/scripts/core/lib.sh"' > "$tmp/hooks/ok.sh"
    scan "$tmp" >/dev/null || { echo "selftest: a layer-rooted hook (and a commented old form) was flagged"; rc=1; }
    printf '%s\n' '_lib="$PROJ/agents/scripts/core/lib.sh"' > "$tmp/hooks/bad.sh"
    scan "$tmp" >/dev/null && { echo "selftest: \$PROJ/agents/... was not flagged"; rc=1; }
    printf '%s\n' 'PREFLIGHT="$ROOT/agents/scripts/core/fleet-preflight.sh"' > "$tmp/hooks/bad.sh"
    scan "$tmp" >/dev/null && { echo "selftest: \$ROOT/agents/... was not flagged"; rc=1; }
    printf '%s\n' 'x="${CLAUDE_PROJECT_DIR:-$(pwd)}/agents/scripts/core/x.sh"' > "$tmp/hooks/bad.sh"
    scan "$tmp" >/dev/null && { echo "selftest: \${CLAUDE_PROJECT_DIR:-...}/agents/... was not flagged"; rc=1; }
    rm -f "$tmp/hooks/bad.sh"
    printf '%s\n' '"command": "bash \"$CLAUDE_PROJECT_DIR/agents/scripts/core/x.sh\""' > "$tmp/hooks/s.json"
    scan "$tmp" >/dev/null && { echo "selftest: a JSON-escaped \$CLAUDE_PROJECT_DIR command was not flagged"; rc=1; }
    rm -f "$tmp/hooks/s.json"
    printf '%s\n' "bash -lc 'bash \\\"\$root/docs/harness/codex/hooks/x.sh\\\"'" > "$tmp/hooks/c.json"
    scan "$tmp" >/dev/null && { echo "selftest: a Codex \$root/docs/harness command was not flagged"; rc=1; }
    rm -rf "$tmp"
    [ "$rc" -eq 0 ] && echo "test-hook-layer-paths: selftest PASS (flags \$PROJ, \$ROOT, \${VAR:-…}, JSON-escaped and Codex forms; passes a layer-rooted hook and a comment)"
    return "$rc"
}

case "${1:-}" in
    --selftest) selftest; exit $? ;;
    "") ;;
    *) echo "usage: $0 [--selftest]" >&2; exit 2 ;;
esac

LAYER="$(cd "$(dirname "$0")/../../.." && pwd)" || exit 2
out="$(scan "$LAYER/docs/harness")"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "test-hook-layer-paths: no hook names a layer path from the project dir"
    echo "Passed: 1  Failed: 0"
    exit 0
fi
[ "$rc" -eq 1 ] || exit 2
printf '%s\n' "$out"
echo "test-hook-layer-paths: the lines above name a layer path from the project dir; after the flip it is under agent-layer/. Route it through layer-root.sh (hooks), layer-run.sh (settings.json) or the Codex l= resolution."
echo "Passed: 0  Failed: 1"
exit 1
