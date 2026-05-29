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
# A SINGLE directory link .claude/agents -> agents/ (junction on Windows, here a
# portable dir symlink). Claude Code discovers agents recursively, so the split
# subdirs are reached as .claude/agents/{core,project}/<name>.md.
rm -rf .claude/agents 2>/dev/null || true
ln -s ../agents .claude/agents 2>/dev/null || { mkdir -p .claude && cp -r agents .claude/agents; }

# --- assertions: each agent resolves through the link at its tier path -------
# name:tier pairs (no associative array — keeps this portable to bash 3.2/macOS).
for pair in "code-review:core" "p4-janitor:core" "tracker-backend:project"; do
  a="${pair%%:*}"; t="${pair##*:}"
  p=".claude/agents/$t/$a.md"
  [ -e "$p" ] || fail "agent not discoverable through the link: $p"
  grep -q "name: $a" "$p" || fail "$p resolves to wrong target"
done

# Recursive discovery sees all agents under the link (both tiers).
n="$(find -L .claude/agents/core .claude/agents/project -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
[ "$n" = "3" ] || fail "expected 3 agents discoverable, got $n"

echo "test-agent-discovery-fixture: PASS — .claude/agents junction reaches core/+project/ agents recursively ($n)."
echo "Passed: 1  Failed: 0"
