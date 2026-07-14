#!/usr/bin/env bash
# Set up a per-harness adapter directory by linking into the canonical
# `agents/` tree and copying small adapter templates from
# `docs/harness/<name>/`. Idempotent.
#
# Supported harnesses: claude-code | codex | cursor | pi
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
  codex         Generate .codex/ config/hooks/custom agents from tracked
                templates + verify AGENTS.md and repo-owned git hooks.
  cursor        Generate .cursor/rules/agents.mdc.
  pi            Generate .pi/agents/*.md (pi-native, from agents/{core,project}/)
                + install the subagent extension into .pi/extensions/ with
                project-local agents enabled (trusted-repo defaults).

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

git_cmd() {
  if git "$@" 2>/dev/null; then
    return 0
  fi
  if command -v git.exe >/dev/null 2>&1; then
    git.exe "$@"
    return $?
  fi
  return 1
}

find_python() {
  local cand path
  for cand in python3 python py; do
    path="$(command -v "$cand" 2>/dev/null)" || continue
    if "$path" -c "" >/dev/null 2>&1; then
      echo "$path"
      return 0
    fi
  done
  return 1
}

# Idempotent dir link.
link_dir() {
  local link="$1" target="$2"
  if [[ -L "$link" && ! -e "$link" ]]; then
    rm -f "$link"
  elif [[ -L "$link" || -d "$link" ]]; then
    return 0
  fi
  mkdir -p "$(dirname "$link")"
  if [[ "$IS_WINDOWS" == "1" ]]; then
    local link_win target_win
    link_win="$(to_win_path "$link")"
    target_win="$(to_win_path "$ROOT/$target")"
    cmd.exe //c mklink //J "$link_win" "$target_win" >/dev/null
  else
    ln -s "$(realpath --relative-to="$(dirname "$link")" "$target" 2>/dev/null || echo "$ROOT/$target")" "$link"
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

# Generate gitignored CLAUDE.md shims beside each subsystem leaf AGENTS.md.
# Claude Code lazy-loads nested CLAUDE.md (not nested AGENTS.md) when a file in
# that dir is touched; the one-line `@AGENTS.md` import pulls in the leaf rules.
# The shim is gitignored (`.gitignore` Source/Core/src/**/CLAUDE.md) so the
# committed tree stays AGENTS.md-only (portable to Codex/Cursor, which read
# AGENTS.md natively). Registry of leaves: root CONTEXT-MAP.md.
gen_subsystem_claude_shims() {
  local count=0 agents shim
  while IFS= read -r agents; do
    [[ -z "$agents" ]] && continue
    shim="$(dirname "$agents")/CLAUDE.md"
    if [[ ! -f "$shim" ]] || ! grep -q '^@AGENTS.md' "$shim" 2>/dev/null; then
      printf '@AGENTS.md\n\n<!-- Generated gitignored shim (setup-harness.sh). Claude Code lazy-loads nested CLAUDE.md, not AGENTS.md; this imports the sibling leaf rules. Edit AGENTS.md, not this file. -->\n' > "$shim"
      count=$((count + 1))
    fi
  done < <(git_cmd ls-files 'Source/Core/src/*/AGENTS.md' 2>/dev/null || true)
  echo "  subsystem-shims  $count new CLAUDE.md shim(s) beside Source/Core/src/*/AGENTS.md (gitignored)"
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
  # Heal an already-provisioned (user-modified) settings.json that copy_template
  # skipped: additively bring in any NEW template hooks (e.g. postmortem-owed
  # --nudge) without clobbering the user's permissions/customisations.
  bash agents/scripts/core/sync-settings-hooks.sh \
    "docs/harness/claude-code/settings.json.tmpl" ".claude/settings.json" || true
  copy_template "docs/harness/claude-code/hooks/lint-cpp.sh"         ".claude/hooks/lint-cpp.sh"
  copy_template "docs/harness/claude-code/hooks/lint-cpp-common.sh"  ".claude/hooks/lint-cpp-common.sh"
  # lint-cpp-common.sh invokes .claude/hooks/lint-catch-all.py (its empty-catch
  # scanner); without this copy the invocation silently no-ops (|| true swallows
  # the missing-file error) and the catch-all lint never runs (HP-02).
  copy_template "docs/harness/claude-code/hooks/lint-catch-all.py"   ".claude/hooks/lint-catch-all.py"
  copy_template "docs/harness/claude-code/hooks/lint-cpp-drain.sh"   ".claude/hooks/lint-cpp-drain.sh"
  copy_template "docs/harness/claude-code/hooks/lint-portable-purity.sh" ".claude/hooks/lint-portable-purity.sh"
  copy_template "docs/harness/claude-code/hooks/pre-ship-stop-gate.sh" ".claude/hooks/pre-ship-stop-gate.sh"
  copy_template "docs/harness/claude-code/hooks/clear-tree-dirty.sh" ".claude/hooks/clear-tree-dirty.sh"
  copy_template "docs/harness/claude-code/hooks/lint-syntax-both.py" ".claude/hooks/lint-syntax-both.py"
  copy_template "docs/harness/claude-code/hooks/autoregister-pr.sh"  ".claude/hooks/autoregister-pr.sh"
  copy_template "docs/harness/claude-code/hooks/guard-head-drift.sh"     ".claude/hooks/guard-head-drift.sh"
  copy_template "docs/harness/claude-code/hooks/guard-plan-lock.sh"      ".claude/hooks/guard-plan-lock.sh"
  copy_template "docs/harness/claude-code/hooks/resync-head-baseline.sh" ".claude/hooks/resync-head-baseline.sh"
  copy_template "docs/harness/claude-code/hooks/guard-shared-tree.sh"    ".claude/hooks/guard-shared-tree.sh"
  copy_template "docs/harness/claude-code/hooks/capture-intent.sh"       ".claude/hooks/capture-intent.sh"
  # Wiring doctor (pr-intent-capture-hardening #1): confirm the capture hook is
  # actually live — registered on UserPromptSubmit AND a python interpreter
  # resolves — so a silently-unwired hook (the gap that left no evidence capture
  # ever ran in a live session) surfaces once here instead of never.
  # Probe-EXECUTE each candidate (not just command -v): on Windows the python3
  # Store app-execution-alias stub satisfies command -v but errors on run, so a
  # name-only check would report WIRED while the hook fail-safes to writing
  # nothing. Mirrors the interpreter resolver in capture-intent.sh.
  _intent_py=""
  for _cand in python3 python py; do
    if command -v "$_cand" >/dev/null 2>&1 && "$_cand" -c "import sys" >/dev/null 2>&1; then
      _intent_py="$_cand"; break
    fi
  done
  if [ -n "$_intent_py" ]; then
    # Structural check (Bugbot e4c20652): capture-intent.sh must be registered as a
    # command UNDER hooks.UserPromptSubmit — not merely present somewhere in the file
    # next to the word "UserPromptSubmit". Two independent greps would false-WIRE a
    # hand-edited / mis-synced settings.json where capture-intent.sh hangs off a
    # DIFFERENT event (e.g. SessionStart). Parse the JSON and walk the actual list.
    if "$_intent_py" - ".claude/settings.json" >/dev/null 2>&1 <<'PY'
import json, sys
try:
    with open(sys.argv[1], encoding="utf-8") as _f:
        _d = json.load(_f)
except Exception:
    sys.exit(1)
for _grp in (_d.get("hooks") or {}).get("UserPromptSubmit") or []:
    for _h in (_grp.get("hooks") or []):
        if "capture-intent.sh" in (_h.get("command") or ""):
            sys.exit(0)
sys.exit(1)
PY
    then
      echo "  prompt-intent capture: WIRED (UserPromptSubmit -> capture-intent.sh)"
    else
      echo "  prompt-intent capture: NOT WIRED — capture-intent.sh not registered under .claude/settings.json hooks.UserPromptSubmit (re-run setup / check sync-settings-hooks.sh)"
    fi
  else
    echo "  prompt-intent capture: INERT — no working python3/python/py on PATH (hook fail-safes to writing nothing)"
  fi

  link_agents

  gen_subsystem_claude_shims

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

  # Auto-link every saved Workflow under agents/_shared/workflows/ into the
  # gitignored .claude/workflows/ so the Workflow tool resolves them by name
  # (Workflow({name: '<base>'})). Workflows are single .js files (link_file,
  # not link_dir like skills). Future workflows get picked up with no script
  # edit. See docs/agent-rules/workflow-orchestration.md.
  if [[ -d "agents/_shared/workflows" ]]; then
    for wf in agents/_shared/workflows/*.js; do
      [[ -e "$wf" ]] || continue
      link_file ".claude/workflows/$(basename "$wf")" "$wf"
    done
  fi
  # Same, for PROJECT-scoped workflows under agents/project/workflows/ — those
  # that embed project literals (paths, subsystem names) and so cannot live in
  # the portable, purity-gated agents/_shared/workflows/. Both link into the
  # same .claude/workflows/, so all resolve by name (Workflow({name: '<base>'})).
  if [[ -d "agents/project/workflows" ]]; then
    for wf in agents/project/workflows/*.js; do
      [[ -e "$wf" ]] || continue
      link_file ".claude/workflows/$(basename "$wf")" "$wf"
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
# tracked pre-commit / pre-push guards fire locally. Only acts when the
# current core.hooksPath is unset or already equal to scripts/git-hooks
# (don't trample a user-set custom hooks path).
#
# Plan: docs/plans/shipped/process-backlog-tighten-1-2-3-9-11-12.md § Slice 3
install_git_hooks() {
  local target="scripts/git-hooks"
  local current normalized_current
  current="$(git_cmd config --local --get core.hooksPath 2>/dev/null || echo '')"
  normalized_current="${current//\\//}"

  if [[ -z "$current" || "$normalized_current" == ".git/hooks" || "$normalized_current" == */.git/hooks ]]; then
    git_cmd config --local core.hooksPath "$target"
    echo "  git-hooks  core.hooksPath set to $target"
  elif [[ "$current" == "$target" ]]; then
    echo "  git-hooks  core.hooksPath already $target"
  else
    echo "  git-hooks  WARNING: core.hooksPath is '$current' (not '$target'). Skipping."
    echo "             To opt in, run: git config --local core.hooksPath $target"
  fi
}

# --- pi (earendil-works/pi-coding-agent) -----------------------------------
# Two pieces, both written under the gitignored .pi/ (regenerated, never
# committed — mirrors .claude/ / .codex/ / .cursor/):
#   1. .pi/agents/*.md   pi-native agent files transformed from the canonical
#                        agents/{core,project}/ tree (gen-pi-agents.py).
#   2. .pi/extensions/subagent/  the stock subagent example, copied from the
#                        installed pi package and PATCHED so project-local
#                        agents load without per-call opt-in (agentScope "both",
#                        confirmProjectAgents false). Only safe because this is a
#                        trusted repo; the relaxed default stays scoped to .pi/.
# Resolve the installed pi package (for the subagent example source).
_pi_pkg_dir() {
    local cand
  for cand in \
      "${PI_PACKAGE_DIR:-}" \
      "$(npm root -g 2>/dev/null)/@earendil-works/pi-coding-agent" \
      "$APPDATA/npm/node_modules/@earendil-works/pi-coding-agent" \
      "$HOME/.npm-global/lib/node_modules/@earendil-works/pi-coding-agent" \
      "/usr/local/lib/node_modules/@earendil-works/pi-coding-agent"; do
    [[ -n "$cand" && -d "$cand/examples/extensions/subagent" ]] && { echo "$cand"; return 0; }
  done
  return 1
}

setup_pi() {
  echo "Setting up pi adapter at .pi/ ..."

  # 1. Generate the pi-native agent files. Resolve via find_python (exec-validating)
  # rather than a bare `command -v python3`, which matches the Windows WinStore
  # alias that passes command -v but exits 49 on run (core-scripts-bash-08); mirrors
  # the codex branch below.
  local py
  if py="$(find_python)"; then
    "$py" agents/scripts/core/gen-pi-agents.py "$ROOT" "$ROOT/.pi/agents"
  else
    echo "  error: python not found — needed to generate .pi/agents/*.md" >&2
    exit 1
  fi

  # 2. Install + patch the subagent extension.
  local pkg dst=".pi/extensions/subagent"
  if pkg="$(_pi_pkg_dir)"; then
    mkdir -p "$dst"
    copy_template "$pkg/examples/extensions/subagent/index.ts"  "$dst/index.ts"
    copy_template "$pkg/examples/extensions/subagent/agents.ts" "$dst/agents.ts"
    # Patch the two security defaults so this trusted repo's project agents
    # load with no per-call opt-in. Idempotent (the markers vanish after the
    # first patch, so a re-run is a no-op).
    if grep -q 'params.agentScope ?? "user"' "$dst/index.ts"; then
      sed -i 's/params.agentScope ?? "user"/params.agentScope ?? "both"/' "$dst/index.ts"
      sed -i 's/params.confirmProjectAgents ?? true/params.confirmProjectAgents ?? false/' "$dst/index.ts"
      # Cosmetic: keep the renderCall scope label honest (display-only path).
      sed -i 's/args.agentScope ?? "user"/args.agentScope ?? "both"/' "$dst/index.ts"
      echo "  patch      $dst/index.ts (agentScope=both, confirmProjectAgents=false)"
    else
      echo "  patch      $dst/index.ts already patched (skip)"
    fi
  else
    echo "  WARN  pi package not found — set PI_PACKAGE_DIR to its path and re-run." >&2
    echo "        .pi/agents/*.md were still generated; install the subagent" >&2
    echo "        extension manually per docs/harness/pi/README.md." >&2
  fi

  echo "Done. .pi/ ready for pi. See docs/harness/pi/README.md for usage."
}

setup_codex() {
  echo "Setting up Codex / OpenAI Agents repo-owned wiring ..."
  echo "Codex reads AGENTS.md natively; setup adds gitignored .codex/"
  echo "config/hooks/custom-agent files generated from tracked templates."
  echo
  copy_template "docs/harness/codex/config.toml.tmpl" ".codex/config.toml"
  copy_template "docs/harness/codex/hooks.json.tmpl" ".codex/hooks.json"

  local py
  if py="$(find_python)"; then
    "$py" agents/scripts/core/gen-codex-agents.py "$ROOT" "$ROOT/.codex/agents"
  else
    echo "  FAIL python not found - needed to generate .codex/agents/*.toml" >&2
    exit 1
  fi

  echo "Verify:"
  if [[ -f "AGENTS.md" ]]; then
    echo "  OK  AGENTS.md present at repo root"
  else
    echo "  FAIL AGENTS.md missing"
    exit 1
  fi

  local core_count project_count count duplicate_basenames leaf_count
  core_count="$(find agents/core -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
  project_count="$(find agents/project -maxdepth 1 -name '*.md' 2>/dev/null | wc -l | tr -d ' ')"
  count=$((core_count + project_count))
  if [[ "$count" -eq 0 ]]; then
    echo "  FAIL agents/{core,project}/*.md has no agent definitions"
    exit 1
  fi
  echo "  OK  agents/{core,project}/*.md = $count files ($core_count core, $project_count project)"

  local codex_agent_count
  codex_agent_count="$(find .codex/agents -maxdepth 1 -name '*.toml' 2>/dev/null | wc -l | tr -d ' ')"
  if [[ "$codex_agent_count" -eq "$count" ]]; then
    echo "  OK  .codex/agents/*.toml = $codex_agent_count generated custom agent(s)"
  else
    echo "  FAIL .codex/agents/*.toml = $codex_agent_count generated custom agent(s), expected $count"
    exit 1
  fi

  duplicate_basenames="$(
    find agents/core agents/project -maxdepth 1 -name '*.md' -exec basename {} \; 2>/dev/null \
      | sort | uniq -d
  )"
  if [[ -n "$duplicate_basenames" ]]; then
    echo "  WARN duplicate agent basenames across core/project:"
    echo "$duplicate_basenames" | sed 's/^/        /'
  else
    echo "  OK  no duplicate agent basenames across core/project"
  fi

  leaf_count="$(git_cmd ls-files 'Source/Core/src/*/AGENTS.md' 2>/dev/null | wc -l | tr -d ' ' || true)"
  echo "  OK  Codex native nearest-AGENTS leaf rules = $leaf_count file(s)"

  if [[ -f ".codex/config.toml" && -f ".codex/hooks.json" ]]; then
    echo "  OK  Codex-native hooks/config installed under .codex/ (trust locally before use)"
  else
    echo "  FAIL .codex/config.toml or .codex/hooks.json missing"
    exit 1
  fi

  install_git_hooks

  cat <<'EOF'

Codex parity report:
  OK   Agent/rule discovery is native: AGENTS.md + agents/{core,project}/*.md.
  OK   Project specialist agents are generated as .codex/agents/*.toml.
  OK   Safe SessionStart/Stop command hooks are installed in .codex/hooks.json.
  OK   Stable repo hooks are wired through scripts/git-hooks when core.hooksPath allows it.
  NOTE Payload-dependent Claude Code hooks remain intentionally unwired in Codex:
       PreToolUse edit/HEAD guards, PostToolUse edit lint, Bash PR
       autoregistration, and SubagentStop token telemetry need Codex payload
       validation before they can block safely. See
       docs/harness/codex/hooks-equivalent.md.
EOF
}

setup_cursor() {
  echo "Setting up Cursor adapter at .cursor/ ..."
  # Cursor accepts .cursor/rules as either a single file or a dir of .mdc rules.
  # Some tools write to .cursor/rules as a single file. Detect that
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
  pi)          setup_pi ;;
  *)
    echo "error: unknown harness '$HARNESS'. Supported: claude-code | codex | cursor | pi" >&2
    echo "Run: bash agents/scripts/core/setup-harness.sh --help" >&2
    exit 1
    ;;
esac
