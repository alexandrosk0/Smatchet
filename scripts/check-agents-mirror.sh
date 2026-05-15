#!/usr/bin/env bash
# Verify .claude/ mirror is in sync with canonical sources under agents/.
# Exit 0 on match, non-zero on drift. CI / pre-commit friendly.
#
# Covers all sync targets:
#   agents/*.md                              -> .claude/agents/*.md
#   agents/_shared/token-tracking/*.py       -> .claude/hooks/agent-token-log.py
#                                            -> .claude/hooks/agents-statusline.py
#   agents/_shared/token-tracking/SKILL.md   -> .claude/skills/agent-tokens/SKILL.md
#   agents/_shared/skills/<skill>/*.md       -> .claude/skills/<skill>/*.md (every subdir)
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Stage a minimal copy: canonical sources + the sync script. Run the script
# inside the tempdir; diff its mirror output against the committed mirror.
mkdir -p "$TMP/agents" "$TMP/scripts"
cp -r "$ROOT/agents/." "$TMP/agents/"
cp "$ROOT/scripts/sync-agents.sh" "$TMP/scripts/sync-agents.sh"

(
  cd "$TMP"
  bash scripts/sync-agents.sh >/dev/null
)

DRIFT=0

# 1) agents/*.md mirror.  README.md is intentional + ignored.
if ! diff -q -r --exclude=README.md "$TMP/.claude/agents" "$ROOT/.claude/agents" >/dev/null; then
  echo "agents mirror OUT OF SYNC (.claude/agents/)" >&2
  diff -r --exclude=README.md "$TMP/.claude/agents" "$ROOT/.claude/agents" >&2 || true
  DRIFT=1
fi

# 2) token-tracking hook mirrors.  Compare only the files we manage; other
#    files under .claude/hooks/ (lint-cpp.sh, vexp-guard.sh, ...) are
#    intentionally out of scope.
for stem in agent-token-log agents-statusline; do
  exp="$TMP/.claude/hooks/${stem}.py"
  act="$ROOT/.claude/hooks/${stem}.py"
  if [[ -f "$exp" && -f "$act" ]]; then
    if ! diff -q "$exp" "$act" >/dev/null; then
      echo ".claude/hooks/${stem}.py OUT OF SYNC" >&2
      diff "$exp" "$act" >&2 || true
      DRIFT=1
    fi
  elif [[ -f "$exp" && ! -f "$act" ]]; then
    echo ".claude/hooks/${stem}.py MISSING (run scripts/sync-agents.sh)" >&2
    DRIFT=1
  fi
done

# 3) token-tracking SKILL.md mirror.
exp="$TMP/.claude/skills/agent-tokens/SKILL.md"
act="$ROOT/.claude/skills/agent-tokens/SKILL.md"
if [[ -f "$exp" && -f "$act" ]]; then
  if ! diff -q "$exp" "$act" >/dev/null; then
    echo ".claude/skills/agent-tokens/SKILL.md OUT OF SYNC" >&2
    diff "$exp" "$act" >&2 || true
    DRIFT=1
  fi
elif [[ -f "$exp" && ! -f "$act" ]]; then
  echo ".claude/skills/agent-tokens/SKILL.md MISSING (run scripts/sync-agents.sh)" >&2
  DRIFT=1
fi

# 4) Every shared-skills subdir mirror.
SHARED_SKILLS_EXP_ROOT="$TMP/.claude/skills"
SHARED_SKILLS_ACT_ROOT="$ROOT/.claude/skills"
SHARED_SKILL_NAMES=()
if [[ -d "$TMP/agents/_shared/skills" ]]; then
  for skill_dir in "$TMP/agents/_shared/skills"/*/; do
    [[ -d "$skill_dir" ]] || continue
    skill_name="$(basename "$skill_dir")"
    SHARED_SKILL_NAMES+=("$skill_name")
    exp="$SHARED_SKILLS_EXP_ROOT/$skill_name"
    act="$SHARED_SKILLS_ACT_ROOT/$skill_name"
    if [[ -d "$exp" ]]; then
      if [[ ! -d "$act" ]]; then
        echo ".claude/skills/${skill_name}/ MISSING (run scripts/sync-agents.sh)" >&2
        DRIFT=1
      elif ! diff -q -r "$exp" "$act" >/dev/null; then
        echo ".claude/skills/${skill_name}/ OUT OF SYNC" >&2
        diff -r "$exp" "$act" >&2 || true
        DRIFT=1
      fi
    fi
  done
fi

if [[ $DRIFT -ne 0 ]]; then
  echo >&2
  echo "fix: run scripts/sync-agents.sh and commit the result" >&2
  exit 1
fi

if [[ ${#SHARED_SKILL_NAMES[@]} -gt 0 ]]; then
  SHARED_LIST="$(IFS=,; echo "${SHARED_SKILL_NAMES[*]}")"
  echo "agents + token-tracking + shared skills ($SHARED_LIST) mirrors in sync"
else
  echo "agents + token-tracking mirrors in sync"
fi
exit 0
