#!/usr/bin/env bash
# test-agent-discovery-fixture.sh — Gate #1 for the agents/ subdir split.
#
# Phase B will move canonical agent defs into agents/core/ + agents/project/.
# Claude Code (and the codex/cursor adapters) discover agents flatly. This test
# proves — on a throwaway FIXTURE, before any real move — that flat discovery
# symlinks generated from subdirs resolve correctly, so harness discovery keeps
# working after the split. It does NOT touch the live agents/ tree.
#
# Mechanism under test (the one setup-harness.sh will adopt in Phase B):
#   for each agents/{core,project}/<name>.md  ->  .claude/agents/<name>.md symlink
#
# Exit 0 = mechanism sound. Run in CI (doc-validation) + locally.
set -uo pipefail

fail() { echo "FAIL: $*" >&2; echo "Passed: 0  Failed: 1"; exit 1; }

work="$(mktemp -d 2>/dev/null || echo "${TMPDIR:-/tmp}/agentfix.$$")"
mkdir -p "$work" || fail "cannot create work dir"
cleanup() { rm -rf "$work"; }
trap cleanup EXIT

cd "$work" || fail "cannot cd work dir"
mkdir -p agents/core agents/project .claude/agents

# Fixture: two core agents (one with a dotted/dashed name), one project agent.
printf -- '---\nname: code-review\n---\nbody\n'      > agents/core/code-review.md
printf -- '---\nname: p4-janitor\n---\nbody\n'        > agents/core/p4-janitor.md
printf -- '---\nname: tracker-backend\n---\nbody\n'   > agents/project/tracker-backend.md

# --- the flattening logic (mirror of what setup-harness.sh will do) ----------
flatten() {
  local dest="$1"; shift
  local src
  for src in "$@"; do
    [ -d "$src" ] || continue
    for f in "$src"/*.md; do
      [ -e "$f" ] || continue
      local base; base="$(basename "$f")"
      # relative link so the .claude/agents tree is relocatable
      ln -sf "../../$f" "$dest/$base" 2>/dev/null \
        || cp "$f" "$dest/$base"   # fallback for filesystems w/o symlinks
    done
  done
}
flatten ".claude/agents" agents/core agents/project

# --- assertions --------------------------------------------------------------
expect=(code-review p4-janitor tracker-backend)
for a in "${expect[@]}"; do
  [ -e ".claude/agents/$a.md" ] || fail "flat discovery link missing: $a.md"
  # the flat entry must resolve to a file whose frontmatter names that agent
  grep -q "name: $a" ".claude/agents/$a.md" || fail "$a.md resolves to wrong target"
done

# No extras, no collisions across subdirs.
n="$(find .claude/agents -maxdepth 1 -name '*.md' | wc -l | tr -d ' ')"
[ "$n" = "3" ] || fail "expected 3 flat links, got $n"

echo "test-agent-discovery-fixture: PASS — flat discovery from core/+project/ subdirs resolves ($n agents)."
echo "Passed: 1  Failed: 0"
