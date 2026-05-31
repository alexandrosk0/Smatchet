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

# --- the discovery mechanism (mirror of setup-harness.sh link_agents) --------
# FLAT per-agent links in .claude/agents/<name>.md from the core/+project/
# subdirs. Flat keeps discovery independent of harness subdir-recursion (the
# canonical .claude/agents/*.md layout). Here we use plain links to stand in for
# the hardlinks setup-harness creates on Windows.
mkdir -p .claude/agents
for f in agents/core/*.md agents/project/*.md; do
  base="$(basename "$f")"
  ln "$f" ".claude/agents/$base" 2>/dev/null \
    || ln -s "../../$f" ".claude/agents/$base" 2>/dev/null \
    || cp "$f" ".claude/agents/$base"
done

# --- assertions: each agent resolves FLATLY at .claude/agents/<name>.md ------
# name:tier pairs (no associative array — keeps this portable to bash 3.2/macOS).
for pair in "code-review:core" "p4-janitor:core" "tracker-backend:project"; do
  a="${pair%%:*}"
  p=".claude/agents/$a.md"
  [ -e "$p" ] || fail "agent not discoverable (flat): $p"
  grep -q "name: $a" "$p" || fail "$p resolves to wrong target"
done

# Exactly the agents from both tiers, flat — no extras, no collisions.
n="$(find -L .claude/agents -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
[ "$n" = "3" ] || fail "expected 3 flat agent links, got $n"

echo "test-agent-discovery-fixture: PASS — flat .claude/agents/*.md from core/+project/ resolves ($n)."
echo "Passed: 1  Failed: 0"
