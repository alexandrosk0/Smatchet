#!/usr/bin/env bash
# Sync canonical agent definitions at agents/*.md into the Claude Code
# auto-discovery mirror at .claude/agents/*.md.
#
# Canonical: edit files under agents/ only. Run this script after any
# canonical edit. The mirror gets a YAML-comment banner warning humans
# not to edit it directly.
#
# Idempotent — running twice produces no diff.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

CANONICAL_DIR="$ROOT/agents"
MIRROR_DIR="$ROOT/.claude/agents"

if [[ ! -d "$CANONICAL_DIR" ]]; then
  echo "error: canonical agents/ dir not found at $CANONICAL_DIR" >&2
  exit 1
fi

mkdir -p "$MIRROR_DIR"

# Write each mirror file: inject a YAML-comment warning right after the
# opening `---` so frontmatter parsers (Claude Code) still see valid YAML.
for src in "$CANONICAL_DIR"/*.md; do
  name="$(basename "$src")"
  dst="$MIRROR_DIR/$name"

  awk -v name="$name" '
    NR == 1 && $0 == "---" {
      print "---"
      print "# AUTO-GENERATED MIRROR of ../../agents/" name " — DO NOT EDIT."
      print "# Run scripts/sync-agents.sh to regenerate."
      injected = 1
      next
    }
    { print }
    END {
      if (!injected) {
        # File had no opening `---` — unusual but not fatal. Mirror as-is.
      }
    }
  ' "$src" > "$dst"
done

# Drop stale mirrors that no longer have a canonical source.
for dst in "$MIRROR_DIR"/*.md; do
  [[ -e "$dst" ]] || continue
  name="$(basename "$dst")"
  if [[ ! -f "$CANONICAL_DIR/$name" ]]; then
    echo "removing stale mirror: $dst" >&2
    rm "$dst"
  fi
done

echo "synced $(ls "$CANONICAL_DIR"/*.md | wc -l | tr -d ' ') agents to $MIRROR_DIR/"
