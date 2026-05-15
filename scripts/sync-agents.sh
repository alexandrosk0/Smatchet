#!/usr/bin/env bash
# Sync canonical agent + agent-tooling sources into the Claude Code
# auto-discovery / hook mirror under .claude/.
#
# Two source trees are mirrored:
#
#   agents/*.md
#     -> .claude/agents/*.md
#     YAML-comment banner injected inside the `---` frontmatter so
#     Claude Code's parser still sees valid YAML.
#
#   agents/_shared/token-tracking/{*.py, SKILL.md}
#     -> .claude/hooks/agent-token-log.py
#     -> .claude/hooks/agents-statusline.py
#     -> .claude/skills/agent-tokens/SKILL.md
#     Python files get a `#`-comment banner injected right after the
#     shebang. SKILL.md gets the same YAML-comment banner used by
#     agents/*.md.
#
# Canonical: edit files under agents/ only. Mirror is auto-generated and
# never to be edited by hand.
#
# Idempotent — running twice produces no diff.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

# -----------------------------------------------------------------------------
# Section 1 — agents/*.md  ->  .claude/agents/*.md
# -----------------------------------------------------------------------------

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
  [[ -e "$src" ]] || continue
  name="$(basename "$src")"
  dst="$MIRROR_DIR/$name"

  # Extract the version: <N> line from the source frontmatter (if any) so the
  # mirror banner can surface it. Default to v? when absent.
  version="$(awk '
    /^---$/      { fm++; if (fm > 1) exit; next }
    fm == 1 && /^version:[[:space:]]*[0-9]+/ {
      sub(/^version:[[:space:]]*/, "")
      sub(/[[:space:]].*$/, "")
      print
      exit
    }
  ' "$src")"
  [[ -z "$version" ]] && version="?"

  awk -v name="$name" -v ver="$version" '
    NR == 1 && $0 == "---" {
      print "---"
      print "# AUTO-GENERATED MIRROR of ../../agents/" name "@v" ver " — DO NOT EDIT."
      print "# Run scripts/sync-agents.sh to regenerate."
      injected = 1
      next
    }
    { print }
  ' "$src" > "$dst"
done

# Drop stale mirrors that no longer have a canonical source.
# README.md is intentional documentation about the mirror itself — never managed by sync.
for dst in "$MIRROR_DIR"/*.md; do
  [[ -e "$dst" ]] || continue
  name="$(basename "$dst")"
  [[ "$name" == "README.md" ]] && continue
  if [[ ! -f "$CANONICAL_DIR/$name" ]]; then
    echo "removing stale mirror: $dst" >&2
    rm "$dst"
  fi
done

AGENTS_COUNT=$(ls "$CANONICAL_DIR"/*.md 2>/dev/null | wc -l | tr -d ' ')

# -----------------------------------------------------------------------------
# Section 2 — agents/_shared/token-tracking/  ->  .claude/{hooks,skills}/
# -----------------------------------------------------------------------------

TT_DIR="$ROOT/agents/_shared/token-tracking"
HOOKS_DIR="$ROOT/.claude/hooks"
SKILLS_DIR="$ROOT/.claude/skills/agent-tokens"

TT_COUNT=0
if [[ -d "$TT_DIR" ]]; then
  mkdir -p "$HOOKS_DIR" "$SKILLS_DIR"

  # Python files: inject a `#`-comment banner after the shebang. If no
  # shebang, prepend the banner at the top.
  for stem in agent-token-log agents-statusline; do
    src="$TT_DIR/${stem}.py"
    [[ -f "$src" ]] || continue
    dst="$HOOKS_DIR/${stem}.py"
    awk -v name="${stem}.py" '
      NR == 1 && /^#!/ {
        print
        print "# AUTO-GENERATED MIRROR of ../../agents/_shared/token-tracking/" name " — DO NOT EDIT."
        print "# Run scripts/sync-agents.sh to regenerate."
        injected = 1
        next
      }
      NR == 1 && !injected {
        print "# AUTO-GENERATED MIRROR of ../../agents/_shared/token-tracking/" name " — DO NOT EDIT."
        print "# Run scripts/sync-agents.sh to regenerate."
        injected = 1
      }
      { print }
    ' "$src" > "$dst"
    TT_COUNT=$((TT_COUNT + 1))
  done

  # SKILL.md: same YAML-comment banner pattern as agents/*.md.
  src="$TT_DIR/SKILL.md"
  if [[ -f "$src" ]]; then
    dst="$SKILLS_DIR/SKILL.md"
    awk '
      NR == 1 && $0 == "---" {
        print "---"
        print "# AUTO-GENERATED MIRROR of ../../../agents/_shared/token-tracking/SKILL.md — DO NOT EDIT."
        print "# Run scripts/sync-agents.sh to regenerate."
        injected = 1
        next
      }
      { print }
    ' "$src" > "$dst"
    TT_COUNT=$((TT_COUNT + 1))
  fi
fi

# -----------------------------------------------------------------------------
# Section 3 — agents/_shared/skills/<skill>/  ->  .claude/skills/<skill>/
# -----------------------------------------------------------------------------
# Generic loop over every subdirectory of agents/_shared/skills/. Each skill
# dir is mirrored verbatim into .claude/skills/<skill>/ with the standard
# auto-generated banner. SKILL.md (YAML-frontmatter) gets the banner injected
# inside the frontmatter; plain markdown gets an HTML-comment banner prepended.

SKILLS_SRC_ROOT="$ROOT/agents/_shared/skills"
SKILLS_DST_ROOT="$ROOT/.claude/skills"

SKILL_DIR_COUNT=0
SKILL_FILE_COUNT=0
if [[ -d "$SKILLS_SRC_ROOT" ]]; then
  for skill_src in "$SKILLS_SRC_ROOT"/*/; do
    [[ -d "$skill_src" ]] || continue
    skill_name="$(basename "$skill_src")"
    skill_dst="$SKILLS_DST_ROOT/$skill_name"
    mkdir -p "$skill_dst"
    SKILL_DIR_COUNT=$((SKILL_DIR_COUNT + 1))

    for src in "$skill_src"*.md; do
      [[ -e "$src" ]] || continue
      name="$(basename "$src")"
      dst="$skill_dst/$name"

      if head -n 1 "$src" | grep -q '^---$'; then
        # YAML-frontmatter file (SKILL.md) — inject banner inside frontmatter.
        awk -v skill="$skill_name" -v name="$name" '
          NR == 1 && $0 == "---" {
            print "---"
            print "# AUTO-GENERATED MIRROR of ../../../agents/_shared/skills/" skill "/" name " — DO NOT EDIT."
            print "# Run scripts/sync-agents.sh to regenerate."
            injected = 1
            next
          }
          { print }
        ' "$src" > "$dst"
      else
        # Plain markdown — prepend an HTML comment banner.
        {
          echo "<!-- AUTO-GENERATED MIRROR of ../../../agents/_shared/skills/$skill_name/$name — DO NOT EDIT. -->"
          echo "<!-- Run scripts/sync-agents.sh to regenerate. -->"
          cat "$src"
        } > "$dst"
      fi
      SKILL_FILE_COUNT=$((SKILL_FILE_COUNT + 1))
    done

    # Drop stale mirrors that no longer have a canonical source.
    for dst in "$skill_dst"/*.md; do
      [[ -e "$dst" ]] || continue
      name="$(basename "$dst")"
      if [[ ! -f "$skill_src$name" ]]; then
        echo "removing stale mirror: $dst" >&2
        rm "$dst"
      fi
    done
  done
fi

echo "synced $AGENTS_COUNT agents to .claude/agents/ + $TT_COUNT token-tracking files to .claude/hooks/ + .claude/skills/agent-tokens/ + $SKILL_FILE_COUNT files across $SKILL_DIR_COUNT shared skills to .claude/skills/"
