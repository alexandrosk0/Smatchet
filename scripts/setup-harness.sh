#!/usr/bin/env bash
# Set up a per-harness adapter directory by linking into the canonical
# `agents/` tree and copying small adapter templates from
# `docs/harness/<name>/`. Idempotent.
#
# Supported harnesses: claude-code | codex | cursor
#
# Why links: keeps `agents/*.md` the single source of truth — edits to canonical
# agent definitions are visible to the harness immediately, no sync step.
# Templates (`settings.json`, hook shell scripts) are copies so per-machine
# tweaks don't propagate back to the tracked template.
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

HARNESS="${1:-}"
if [[ -z "$HARNESS" || "$HARNESS" == "-h" || "$HARNESS" == "--help" ]]; then
  cat <<'EOF'
Usage: bash scripts/setup-harness.sh <harness>

Harnesses:
  claude-code   Generate .claude/ with junctions/symlinks into agents/ +
                copies of settings.json, CLAUDE.md, lint hooks.
  codex         No local setup needed — Codex reads AGENTS.md directly per
                the agents.md spec. Run for confirmation.
  cursor        Generate .cursor/rules/agents.mdc.

Idempotent — re-running is safe. Edits to .claude/settings.json (or other
templates) survive: the script skips copies if the local file differs from
the tracked template.

After clone, run the script for the harness you use. See
docs/harness/SETUP.md for the full mapping.
EOF
  exit 0
fi

# --- Detect OS for link primitives ------------------------------------------
OS="$(uname -s 2>/dev/null || echo Windows)"
case "$OS" in
  MINGW*|MSYS*|CYGWIN*|Windows*) IS_WINDOWS=1 ;;
  *)                              IS_WINDOWS=0 ;;
esac

# --- Helpers ----------------------------------------------------------------

to_win_path() {
  if [[ "$IS_WINDOWS" == "1" ]]; then
    cygpath -w "$1" 2>/dev/null || echo "$1"
  else
    echo "$1"
  fi
}

# Idempotent dir link.
link_dir() {
  local link="$1" target="$2"
  if [[ -L "$link" || -d "$link" ]]; then
    return 0
  fi
  mkdir -p "$(dirname "$link")"
  if [[ "$IS_WINDOWS" == "1" ]]; then
    local link_win target_win
    link_win="$(to_win_path "$link")"
    target_win="$(to_win_path "$ROOT/$target")"
    cmd.exe //c mklink //J "$link_win" "$target_win" >/dev/null
  else
    ln -s "$target" "$link"
  fi
  echo "  link-dir   $link -> $target"
}

# Idempotent file link.
link_file() {
  local link="$1" target="$2"
  if [[ -e "$link" ]]; then
    return 0
  fi
  mkdir -p "$(dirname "$link")"
  if [[ "$IS_WINDOWS" == "1" ]]; then
    local link_win target_win
    link_win="$(to_win_path "$link")"
    target_win="$(to_win_path "$ROOT/$target")"
    if ! cmd.exe //c mklink "$link_win" "$target_win" >/dev/null 2>&1; then
      cmd.exe //c mklink //H "$link_win" "$target_win" >/dev/null
    fi
  else
    ln -s "$(realpath --relative-to="$(dirname "$link")" "$target" 2>/dev/null || echo "$ROOT/$target")" "$link"
  fi
  echo "  link-file  $link -> $target"
}

copy_template() {
  local src="$1" dst="$2"
  mkdir -p "$(dirname "$dst")"
  if [[ -e "$dst" ]]; then
    if cmp -s "$src" "$dst"; then
      return 0
    fi
    echo "  skip-copy  $dst (user-modified — not overwriting)"
    return 0
  fi
  cp "$src" "$dst"
  case "$src" in *.sh) chmod +x "$dst" ;; esac
  echo "  copy       $dst"
}

# --- Per-harness setup ------------------------------------------------------

setup_claude_code() {
  echo "Setting up Claude Code adapter at .claude/ ..."

  copy_template "docs/harness/claude-code/CLAUDE.md.tmpl"     ".claude/CLAUDE.md"
  copy_template "docs/harness/claude-code/settings.json.tmpl" ".claude/settings.json"
  copy_template "docs/harness/claude-code/hooks/lint-cpp.sh"         ".claude/hooks/lint-cpp.sh"
  copy_template "docs/harness/claude-code/hooks/lint-cpp-common.sh"  ".claude/hooks/lint-cpp-common.sh"
  copy_template "docs/harness/claude-code/hooks/lint-cpp-drain.sh"   ".claude/hooks/lint-cpp-drain.sh"
  copy_template "docs/harness/claude-code/hooks/clear-tree-dirty.sh" ".claude/hooks/clear-tree-dirty.sh"
  copy_template "docs/harness/claude-code/hooks/vexp-guard.sh"       ".claude/hooks/vexp-guard.sh"
  copy_template "docs/harness/claude-code/hooks/lint-syntax-both.py" ".claude/hooks/lint-syntax-both.py"

  link_dir  ".claude/agents"                                  "agents"

  # Auto-link every SKILL.md package under agents/_shared/skills/. Future
  # skills get picked up with no script edit. Existing skills here today:
  # grill-with-docs, scratchpad-recall. v3 of the perf-skill-aliases plan
  # adds perf-instrument + perf-measure under this same root.
  if [[ -d "agents/_shared/skills" ]]; then
    for skill_dir in agents/_shared/skills/*/; do
      [[ -d "$skill_dir" ]] || continue
      skill_name="$(basename "$skill_dir")"
      link_dir ".claude/skills/$skill_name" "${skill_dir%/}"
    done
  fi

  # Special case: token-tracking lives at a non-conforming path
  # (agents/_shared/token-tracking/, not agents/_shared/skills/token-tracking/).
  # Until that's normalised, link the single file by hand.
  link_file ".claude/skills/agent-tokens/SKILL.md"            "agents/_shared/token-tracking/SKILL.md"
  link_file ".claude/hooks/agent-token-log.py"                "agents/_shared/token-tracking/agent-token-log.py"
  link_file ".claude/hooks/agents-statusline.py"              "agents/_shared/token-tracking/agents-statusline.py"

  echo "Done. .claude/ ready for Claude Code."
}

setup_codex() {
  echo "Codex / OpenAI Agents reads AGENTS.md + agents/*.md directly per the"
  echo "agents.md spec — no local adapter required."
  echo
  echo "Verify:"
  if [[ -f "AGENTS.md" ]]; then echo "  OK  AGENTS.md present at repo root"; else echo "  FAIL AGENTS.md missing"; exit 1; fi
  local count
  count="$(find agents -maxdepth 1 -name '*.md' -not -name 'README.md' | wc -l | tr -d ' ')"
  echo "  OK  agents/*.md = $count files"
}

setup_cursor() {
  echo "Setting up Cursor adapter at .cursor/ ..."
  # Cursor accepts .cursor/rules as either a single file or a dir of .mdc rules.
  # Some tools (e.g. vexp) write to .cursor/rules as a single file. Detect that
  # and bail with a fix-it message rather than clobbering user state.
  if [[ -f ".cursor/rules" ]]; then
    echo "  error: .cursor/rules exists as a file (not a directory)." >&2
    echo "         Move it aside so this script can use the .mdc-rules layout:" >&2
    echo "           mv .cursor/rules .cursor/rules.bak" >&2
    echo "         Then re-run: bash scripts/setup-harness.sh cursor" >&2
    exit 1
  fi
  copy_template "docs/harness/cursor/rules/agents.mdc" ".cursor/rules/agents.mdc"
  echo "Done. .cursor/rules/agents.mdc points Cursor at AGENTS.md + agents/."
}

case "$HARNESS" in
  claude-code) setup_claude_code ;;
  codex)       setup_codex ;;
  cursor)      setup_cursor ;;
  *)
    echo "error: unknown harness '$HARNESS'. Supported: claude-code | codex | cursor" >&2
    echo "Run: bash scripts/setup-harness.sh --help" >&2
    exit 1
    ;;
esac
