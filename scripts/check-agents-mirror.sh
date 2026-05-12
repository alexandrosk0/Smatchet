#!/usr/bin/env bash
# Verify .claude/agents/*.md is in sync with agents/*.md.
# Exit 0 on match, non-zero on drift. CI / pre-commit friendly.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Generate a fresh mirror in tempdir using the same logic as sync-agents.sh,
# then diff against the committed mirror.
mkdir -p "$TMP/.claude/agents"
mkdir -p "$TMP/agents"
cp "$ROOT/agents/"*.md "$TMP/agents/"

# Run sync logic against the temp copy.
(
  cd "$TMP"
  for src in agents/*.md; do
    name="$(basename "$src")"
    dst=".claude/agents/$name"
    awk -v name="$name" '
      NR == 1 && $0 == "---" {
        print "---"
        print "# AUTO-GENERATED MIRROR of ../../agents/" name " — DO NOT EDIT."
        print "# Run scripts/sync-agents.sh to regenerate."
        next
      }
      { print }
    ' "$src" > "$dst"
  done
)

# Diff the temp mirror against the committed mirror.
if diff -q -r "$TMP/.claude/agents" "$ROOT/.claude/agents" > /dev/null; then
  echo "agents mirror is in sync"
  exit 0
else
  echo "agents mirror is OUT OF SYNC with canonical agents/" >&2
  echo "diff:" >&2
  diff -r "$TMP/.claude/agents" "$ROOT/.claude/agents" >&2 || true
  echo >&2
  echo "fix: run scripts/sync-agents.sh and commit the result" >&2
  exit 1
fi
