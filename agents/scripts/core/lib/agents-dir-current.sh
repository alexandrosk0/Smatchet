#!/usr/bin/env bash
# agents-dir-current.sh — shared ".claude/agents is current" probe (sourced, not run).
# ----------------------------------------------------------------------------
# WHY THIS EXISTS (plan agent-surface-extraction-repo, Phase A rows 4c + 5e)
#   The canonical agent definitions live in the agent LAYER (agents/{core,project}/);
#   the flat dir harnesses actually scan, .claude/agents/, is HOST-side and is
#   materialised by setup-harness.sh as one HARDLINK per agent. A hardlink shares
#   an inode, so an edit to the canonical file is visible through the link — that
#   is the whole point of the design.
#
#   It is also the hazard. `git submodule update` does not edit files in place;
#   it checks out NEW ones. Once the layer is a submodule, advancing it leaves
#   every pre-existing .claude/agents/*.md hardlink pointing at the OLD inode,
#   still holding YESTERDAY'S agent definition, with nothing to say so. The
#   session comes up and runs stale rules. POSIX symlinks resolve by path and are
#   unaffected; Windows — the primary dev environment — is where this bites.
#
#   Only a CONTENT comparison can see that state. setup-harness.sh already had
#   one, as its private idempotency probe; check-harness-provisioned.sh had none
#   at all and could detect only total absence. Rather than give the checker a
#   second copy of the same cmp loop — the DRY failure the plan's own § Existing
#   utilities reused warns about — the probe is lifted here and sourced by both.
#
# CONTRACT — agents_dir_current <dest> <layer_root>
#   Returns 0 when <dest> is a real directory (not a junction or symlink) holding
#   exactly the canonical agent set found under <layer_root>/agents/{core,project}/,
#   with every entry byte-identical to its source. Returns 1 otherwise — a
#   junction/symlink at <dest>, a count mismatch (an agent added or removed), or
#   any content drift (a stale cp-fallback copy, or a hardlink left behind by a
#   submodule update). It reads only; it never mutates <dest>.
#
#   <layer_root> is passed explicitly rather than read from $AGENT_LAYER_ROOT so
#   the function stays callable from a script that has not sourced
#   project-config.sh, and so a caller can probe a tree other than its own.
#
#   The two callers read the SAME return for opposite purposes: setup-harness.sh
#   uses 0 to skip a rebuild (idempotency), check-harness-provisioned.sh uses 1
#   to report drift. Keep it a pure predicate — a caller that needs a message
#   prints its own.

# shellcheck shell=bash
agents_dir_current() {
  local dest="$1" layer_root="$2" f base exp=0 got=0
  [ -d "$dest" ] && [ ! -L "$dest" ] || return 1
  for f in "$layer_root"/agents/core/*.md "$layer_root"/agents/project/*.md; do
    [ -e "$f" ] && exp=$((exp + 1))
  done
  for f in "$dest"/*.md; do [ -e "$f" ] && got=$((got + 1)); done
  [ "$got" -eq "$exp" ] || return 1
  for f in "$layer_root"/agents/core/*.md "$layer_root"/agents/project/*.md; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    cmp -s "$f" "$dest/$base" || return 1
  done
  return 0
}
