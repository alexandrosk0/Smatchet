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

cd "$(dirname "$0")/../../.."
ROOT="$(pwd)"

HARNESS="${1:-}"
if [[ -z "$HARNESS" || "$HARNESS" == "-h" || "$HARNESS" == "--help" ]]; then
  cat <<'EOF'
Usage: bash agents/scripts/core/setup-harness.sh <harness>

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

# Agent discovery. Canonical defs live under agents/{core,project}/ (the
# portable/project split), but harnesses scan .claude/agents/*.md FLATLY — so
# materialise a flat dir of per-agent links. Flat (not a junction-to-subdirs)
# keeps discovery independent of whether the harness recurses.
#
# Links are HARDLINKS, created directly (no symbolic-link attempt): on Windows a
# symbolic mklink can prompt for elevation and intermittently HANG; mklink //H
# needs no elevation and is reliable. Edits to the canonical file are visible
# through the hardlink (shared inode), same as before.
# Idempotency probe: true when $dest is already a real dir holding exactly the
# canonical agent set, each entry byte-identical to its source. Lets link_agents
# no-op (and stay silent) on an unchanged re-run — matching link_file's contract.
# A junction/symlink, a count mismatch (agent added/removed), or any content
# drift (e.g. a stale cp-fallback copy) fails the probe and forces a rebuild.
_agents_dir_current() {
  local dest="$1" f base exp=0 got=0
  [[ -d "$dest" && ! -L "$dest" ]] || return 1
  for f in agents/core/*.md agents/project/*.md; do [[ -e "$f" ]] && exp=$((exp + 1)); done
  for f in "$dest"/*.md; do [[ -e "$f" ]] && got=$((got + 1)); done
  [[ "$got" -eq "$exp" ]] || return 1
  for f in agents/core/*.md agents/project/*.md; do
    [[ -e "$f" ]] || continue
    base="$(basename "$f")"
    cmp -s "$f" "$dest/$base" || return 1
  done
  return 0
}

link_agents() {
  local dest=".claude/agents" f base
  # Idempotent: skip the clear+relink (and the link-agents echo) when already current.
  _agents_dir_current "$dest" && return 0
  _clear_dir_link "$dest"
  mkdir -p "$dest"
  for f in agents/core/*.md agents/project/*.md; do
    [[ -e "$f" ]] || continue
    base="$(basename "$f")"
    # $dest was just cleared, so an existing link here means a same-run basename
    # collision across core/+project/ — warn loudly (the agent would otherwise
    # vanish from discovery silently).
    if [[ -e "$dest/$base" ]]; then
      echo "  WARN  agent basename collision: '$base' in two tiers; '$f' NOT linked (won't be discoverable). Rename one." >&2
      continue
    fi
    if [[ "$IS_WINDOWS" == "1" ]]; then
      cmd.exe //c mklink //H "$(to_win_path "$dest/$base")" "$(to_win_path "$ROOT/$f")" >/dev/null 2>&1 \
        || cp "$ROOT/$f" "$dest/$base"
    else
      ln -s "$(realpath --relative-to="$dest" "$f" 2>/dev/null || echo "$ROOT/$f")" "$dest/$base"
    fi
  done
  echo "  link-agents  .claude/agents/*.md (flat hardlinks) <- agents/{core,project}/"
}

# Safely remove an existing .claude/agents whether it's a Windows junction, a
# symlink, or a real (possibly partial) directory — WITHOUT recursing into a
# junction's target (which rm -rf would do, deleting the canonical agents/).
_clear_dir_link() {
  local dest="$1"
  [[ -e "$dest" || -L "$dest" ]] || return 0
  if [[ "$IS_WINDOWS" == "1" ]]; then
    # cmd rmdir removes a junction without touching the target; on a real
    # non-empty dir it fails, so fall back to rm -rf (safe: real files only).
    cmd.exe //c rmdir "$(to_win_path "$dest")" >/dev/null 2>&1 || rm -rf "$dest"
  elif [[ -L "$dest" ]]; then
    rm -f "$dest"
  else
    rm -rf "$dest"
  fi
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
  copy_template "docs/harness/claude-code/hooks/autoregister-pr.sh"  ".claude/hooks/autoregister-pr.sh"

  link_agents

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
  link_file ".claude/hooks/skill-load-log.py"                 "agents/_shared/token-tracking/skill-load-log.py"

  install_git_hooks

  echo "Done. .claude/ ready for Claude Code."
}

# install_git_hooks: point core.hooksPath at scripts/git-hooks/ so the
# tracked pre-push merged-PR guard fires on every push. Only acts when the
# current core.hooksPath is unset or already equal to scripts/git-hooks
# (don't trample a user-set custom hooks path).
#
# Plan: docs/plans/shipped/process-backlog-tighten-1-2-3-9-11-12.md § Slice 3
install_git_hooks() {
  local target="scripts/git-hooks"
  local current
  current="$(git config --local --get core.hooksPath 2>/dev/null || echo '')"

  if [[ -z "$current" ]]; then
    git config --local core.hooksPath "$target"
    echo "  git-hooks  core.hooksPath set to $target"
  elif [[ "$current" == "$target" ]]; then
    echo "  git-hooks  core.hooksPath already $target"
  else
    echo "  git-hooks  WARNING: core.hooksPath is '$current' (not '$target'). Skipping."
    echo "             To opt in, run: git config --local core.hooksPath $target"
  fi
}

setup_codex() {
  echo "Codex / OpenAI Agents reads AGENTS.md + agents/*.md directly per the"
  echo "agents.md spec — no local adapter required."
  echo
  echo "Verify:"
  if [[ -f "AGENTS.md" ]]; then echo "  OK  AGENTS.md present at repo root"; else echo "  FAIL AGENTS.md missing"; exit 1; fi
  local count
  count="$(find agents/core agents/project -maxdepth 1 -name '*.md' | wc -l | tr -d ' ')"
  echo "  OK  agents/{core,project}/*.md = $count files"
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
    echo "         Then re-run: bash agents/scripts/core/setup-harness.sh cursor" >&2
    exit 1
  fi
  copy_template "docs/harness/cursor/rules/agents.mdc" ".cursor/rules/agents.mdc"
  echo "Done. .cursor/rules/agents.mdc points Cursor at AGENTS.md + agents/."
}

# Required-tools pre-check — fires before the harness-specific setup so a
# missing dep surfaces at setup time, not at the next high-stakes moment
# (merge-gates poll, lint flush, etc.). Non-fatal — the harness setup still
# proceeds even if a required tool is missing, so the user can finish the
# adapter wiring + install the missing tool separately. See
# docs/self-improvement/categories/tooling.md "Required CLI tools must be
# discoverable + verified at first-setup time" for the motivating incident.
if [[ -x "$ROOT/scripts/dev/check-required-tools.sh" ]]; then
  bash "$ROOT/scripts/dev/check-required-tools.sh" || {
    echo "" >&2
    echo "note: continuing setup despite missing required tool(s). Install the flagged" >&2
    echo "      tools above before running build/test/poller commands. Re-run" >&2
    echo "      'bash scripts/dev/check-required-tools.sh' to confirm." >&2
    echo "" >&2
  }
fi

# Dual-VCS opt-in check (per AGENTS.md § Dual-VCS topology + Phase 5/6 of
# docs/plans/shipped/git-to-perforce-migration.md). Non-fatal — only warns when the
# session has opted into the p4 layer (`SMATCHET_AGENT_VCS=p4`) but the p4
# client is unconfigured. Sessions on the default `git` backend see nothing.
if [[ "${SMATCHET_AGENT_VCS:-git}" == "p4" ]]; then
  echo "" >&2
  echo "dual-VCS: SMATCHET_AGENT_VCS=p4 set — verifying p4 client config:" >&2
  if ! command -v p4 >/dev/null 2>&1; then
    echo "  WARN  p4 binary not on PATH. Install Helix Core client per docs/perforce/SETUP.md." >&2
  elif [[ -z "${P4PORT:-}" ]]; then
    echo "  WARN  P4PORT not set. Export per docs/perforce/SETUP.md § Decisions locked." >&2
  elif ! p4 info >/dev/null 2>&1; then
    echo "  WARN  p4 info failed against P4PORT=${P4PORT}. Server unreachable or auth missing." >&2
  else
    echo "  OK    p4 client reaches ${P4PORT}." >&2
  fi
fi

case "$HARNESS" in
  claude-code) setup_claude_code ;;
  codex)       setup_codex ;;
  cursor)      setup_cursor ;;
  *)
    echo "error: unknown harness '$HARNESS'. Supported: claude-code | codex | cursor" >&2
    echo "Run: bash agents/scripts/core/setup-harness.sh --help" >&2
    exit 1
    ;;
esac
