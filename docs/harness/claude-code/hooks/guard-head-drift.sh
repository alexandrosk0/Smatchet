#!/usr/bin/env bash
# guard-head-drift.sh — PreToolUse(Edit|Write|Bash|PowerShell) HEAD-drift guard.
#
# THE primary fix for the concurrent-session "wrong-branch commit" incident: when
# another actor (a sibling session, the user's terminal, a janitor) moves the
# shared HEAD under this session, the session's next Edit/Write/commit would land
# against the wrong branch. This hook compares live HEAD to the baseline this
# session recorded at SessionStart (sha-level, so it also catches same-branch
# pull/reset) and DENIES the write/commit. It also blocks direct commits to
# develop/main in the integration tree.
#
# When already drifted it ALSO denies further HEAD-moving git ops (pull/reset/
# merge/rebase/checkout/switch) — recovery is `worktree.sh resync` or a new
# worktree, never another shared-tree move. This is what makes resync-head-
# baseline.sh safe: a HEAD-moving op only succeeds from a clean baseline, so the
# PostToolUse re-baseline can never mask a pre-existing external drift.
#
# Registry-INDEPENDENT of sibling liveness: it only reads THIS session's own
# baseline, so it fires even against a sibling that predates these hooks.
#
# Allow = exit 0 (no stdout). Deny = the PreToolUse permissionDecision JSON.
# Override: SMATCHET_ACK_BRANCH_DRIFT=1 (you accept the current branch).
#
# See docs/agent-rules/process-rules.md - Concurrent interactive sessions.

set -u

# Minimal JSON string escape (backslash + double-quote) so Windows paths (C:\...)
# and branch names with quotes can't produce invalid JSON and silently fail open.
# Reasons are single-line static text + git refs/paths, so control-char escaping
# beyond this is unnecessary.
json_escape() {
  local s="$1"
  s="${s//\\/\\\\}"
  s="${s//\"/\\\"}"
  printf '%s' "$s"
}

deny() {
  printf '{"hookSpecificOutput":{"hookEventName":"PreToolUse","permissionDecision":"deny","permissionDecisionReason":"%s"}}' "$(json_escape "$1")"
  exit 0
}

[ "${SMATCHET_ACK_BRANCH_DRIFT:-}" = "1" ] && exit 0

PROJ="${CLAUDE_PROJECT_DIR:-$(pwd)}"
INPUT="$(cat || true)"
[ -n "$INPUT" ] || exit 0

json_field() { # $1 = jq filter, $2 = sed key
  if command -v jq >/dev/null 2>&1; then
    printf '%s' "$INPUT" | jq -r "$1 // empty" 2>/dev/null
  else
    printf '%s' "$INPUT" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\\([^\"]*\\)\".*/\\1/p" | head -n1
  fi
}

SID="$(json_field '.session_id' 'session_id')"
[ -n "$SID" ] || SID="${CLAUDE_SESSION_ID:-}"
[ -n "$SID" ] || exit 0

ENTRY="$PROJ/.claude/.active-sessions/$SID"
[ -f "$ENTRY" ] || exit 0   # no baseline for this session -> allow (never false-block)

BASE_BRANCH="$(sed -n 's/^branch=//p' "$ENTRY" | head -n1)"
BASE_SHA="$(sed -n 's/^sha=//p' "$ENTRY" | head -n1)"

CUR_BRANCH="$(git -C "$PROJ" symbolic-ref --short HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_BRANCH" ] || exit 0   # detached / mid-rebase -> allow
CUR_SHA="$(git -C "$PROJ" rev-parse HEAD 2>/dev/null)" || exit 0
[ -n "$CUR_SHA" ] || exit 0

# Integration tree = the main worktree (.git is a directory; a linked worktree's
# .git is a file pointer).
IS_INTEGRATION=0
[ -d "$PROJ/.git" ] && IS_INTEGRATION=1

drifted=0
[ -n "$BASE_BRANCH" ] && [ "$CUR_BRANCH" != "$BASE_BRANCH" ] && drifted=1
[ -n "$BASE_SHA" ]    && [ "$CUR_SHA" != "$BASE_SHA" ]       && drifted=1

short_base="$(printf '%s' "$BASE_SHA" | cut -c1-8)"
short_cur="$(printf '%s' "$CUR_SHA" | cut -c1-8)"

drift_reason="HEAD moved under this session (started ${BASE_BRANCH}@${short_base}, now ${CUR_BRANCH}@${short_cur}). Re-baseline if intended: bash scripts/dev/worktree.sh resync. Or isolate: bash scripts/dev/worktree.sh new <slug>. Do NOT git switch back in a shared tree (it rug-pulls live siblings)."

TOOL="$(json_field '.tool_name' 'tool_name')"

# Shared git-invocation grammar: `git [-C path|-c k=v|--flag]* <op>`.
# Leading token accepts `git` and `git.exe` (PowerShell idiom), bare or as the
# tail of a quoted/full path (`& "C:\...\git.exe" commit`), and the boundary
# class includes `(` so subshell/substitution forms `(git …)` / `$(git …)`
# can't slip past (CR-947 hardening — these previously failed OPEN).
GIT_INVOKE_RE='(^|[;&|([:space:]"\\/])git(\.exe)?"?'
# `-C`/`-c` accept a double-QUOTED value so a worktree path with spaces
# (`-C "C:\my wt"`) doesn't truncate at the first space and drop the whole
# invocation out of the match — that was a direct-commit-to-develop fail-OPEN
# (tooling.md :328 / :175).
GIT_OPTS_RE='([[:space:]]+(-C[[:space:]]+("[^"]*"|[^[:space:]]+)|-c[[:space:]]+("[^"]*"|[^[:space:]]+)|--[^[:space:]]+))*'

# Absolute, separator-normalised git-common-dir of a git dir/worktree, or empty
# if $1 is not inside a git repo. Two paths sharing a common-dir belong to the
# SAME repository (the integration tree and its linked worktrees all resolve to
# the integration tree's `.git`); a path with a DIFFERENT common-dir is a wholly
# separate repo (a mktemp throwaway, a sibling clone) that cannot move this
# session's HEAD.
git_common_dir_abs() { # $1 = path inside a git repo
  local d="$1" c
  c="$(git -C "$d" rev-parse --git-common-dir 2>/dev/null)" || return 1
  [ -n "$c" ] || return 1
  case "$c" in [A-Za-z]:*|/*|\\\\*) ;; *) c="$d/$c" ;; esac
  ( cd "$c" 2>/dev/null && pwd ) || printf '%s' "$c"
}

# Common-dir of THIS session's protected integration tree, computed once.
PROJ_COMMON="$(git_common_dir_abs "$PROJ")"

# True (0) when EVERY git invocation in $CMD matching the op alternation carries
# a `-C <path>` whose target cannot touch this session's protected tree — either
# (a) a SEPARATE repository (its git-common-dir differs from the integration
# tree's: a mktemp throwaway repo, a sibling clone — these are unrelated to the
# protected develop/main tree and were previously false-DENIED because their
# `.git` is a DIRECTORY, not a worktree file pointer), or (b) a linked worktree
# of THIS repo (.git is a FILE pointer) sitting on a non-protected branch. Such
# ops never move the protected HEAD, so the denies below don't apply — this is
# what lets an integration-tree-rooted session ship from a worktree it created
# (tooling.md P2; the PR #916/#936 friction) AND run unrelated tmp-repo git in a
# bats fixture. Any -C-less invocation, an integration-tree -C target, or a
# same-repo worktree on a protected branch keeps the deny (conservative).
all_git_ops_target_safe_worktree() { # $1 = op alternation, e.g. 'commit'
  local ops="$1" m tgt branch tgt_common
  local matches
  matches="$(printf '%s' "$CMD" | grep -oE "${GIT_INVOKE_RE}${GIT_OPTS_RE}[[:space:]]+(${ops})(\$|[;&|)[:space:]])")"
  [ -n "$matches" ] || return 1
  while IFS= read -r m; do
    [ -n "$m" ] || continue
    # Last -C wins (matches git's own behaviour for repeated -C... close enough:
    # git actually chains relative -C, but absolute-path duplicates are the only
    # realistic agent shape and the LAST one is the effective base there).
    tgt="$(printf '%s' "$m" | sed -nE 's/.*-C[[:space:]]+("[^"]*"|[^[:space:]]+).*/\1/p')"
    tgt="${tgt%\"}"; tgt="${tgt#\"}"; tgt="${tgt%\'}"; tgt="${tgt#\'}"
    [ -n "$tgt" ] || return 1                       # no -C → targets this tree
    case "$tgt" in [A-Za-z]:*|/*|\\\\*) ;; *) tgt="$PROJ/$tgt" ;; esac
    # Separate-repository exemption: a -C target whose git-common-dir differs
    # from the protected tree's is a wholly distinct repo and cannot move this
    # session's HEAD — allow it regardless of its branch (a throwaway test repo
    # may legitimately sit on develop/main). Requires BOTH common-dirs to
    # resolve; if EITHER is empty (target not a repo, or PROJ unreadable) we
    # fall through to the conservative same-repo checks below.
    tgt_common="$(git_common_dir_abs "$tgt")"
    if [ -n "$tgt_common" ] && [ -n "$PROJ_COMMON" ] && [ "$tgt_common" != "$PROJ_COMMON" ]; then
      continue                                       # different repo → safe
    fi
    # Same repository (or undeterminable): only a linked worktree (.git is a
    # FILE pointer, never the integration tree's .git directory) on a
    # non-protected branch is exempt.
    [ -f "$tgt/.git" ] || return 1                  # not a linked worktree
    branch="$(git -C "$tgt" symbolic-ref --short HEAD 2>/dev/null)" || return 1
    case "$branch" in develop|main|"") return 1 ;; esac
  done <<MATCHES
$matches
MATCHES
  return 0
}

case "$TOOL" in
  Bash|PowerShell)
    CMD="$(json_field '.tool_input.command' 'command')"
    # git commit: `git [-C path|-c k=v|--flag]* commit`.
    if printf '%s' "$CMD" | grep -qE "${GIT_INVOKE_RE}${GIT_OPTS_RE}[[:space:]]+commit(\$|[;&|)[:space:]])"; then
      if [ "$IS_INTEGRATION" = "1" ] && { [ "$CUR_BRANCH" = "develop" ] || [ "$CUR_BRANCH" = "main" ]; } \
         && ! all_git_ops_target_safe_worktree 'commit'; then
        deny "No direct commit to ${CUR_BRANCH} in the integration tree (${PROJ}). Feature work belongs in a worktree: bash scripts/dev/worktree.sh new <slug> — then commit with an explicit \`git -C <worktree-path> commit\` (allowed from here). Pass a LITERAL absolute path, not a shell variable like \$WT — this guard reads the un-expanded command text, so a \$VAR is rejected. Override: SMATCHET_ACK_BRANCH_DRIFT=1 (must be exported before session launch)."
      fi
    fi
    # When already drifted, deny any further HEAD-moving git op so the PostToolUse
    # re-baseline can never lock in an external drift (recover via resync first).
    # Ops explicitly -C-targeted at a linked worktree are exempt — they cannot
    # move THIS tree's HEAD, drifted or not.
    if [ "$drifted" = "1" ] && printf '%s' "$CMD" | grep -qE "${GIT_INVOKE_RE}${GIT_OPTS_RE}[[:space:]]+(commit|pull|reset|merge|rebase|checkout|switch|cherry-pick|am|revert)(\$|[;&|)[:space:]])" \
       && ! all_git_ops_target_safe_worktree 'commit|pull|reset|merge|rebase|checkout|switch|cherry-pick|am|revert'; then
      deny "$drift_reason"
    fi
    exit 0
    ;;
  Edit|Write|MultiEdit|NotebookEdit)
    if [ "$drifted" = "1" ]; then
      # Worktree exemption: the drift deny exists because a stale Read of an
      # INTEGRATION-TREE file would write against the wrong branch. A write whose
      # file_path resolves to a DIFFERENT git toplevel than the integration tree
      # (a linked worktree under .claude/worktrees/<id>/ has its own toplevel, or
      # a file outside this repo entirely) cannot land against this tree's drifted
      # HEAD — so it's safe even while the integration HEAD has moved. Without this
      # an integration-tree-rooted session was frozen out of editing its OWN
      # worktree whenever a sibling moved the shared HEAD (tooling.md :70).
      FP="$(json_field '.tool_input.file_path' 'file_path')"
      # ABSOLUTE paths only — a relative path resolves against the hook's cwd,
      # which is not reliably the integration tree, so it stays conservatively
      # denied. (Claude Code's Edit/Write always pass an absolute file_path.)
      case "$FP" in
        /*|[A-Za-z]:[/\\]*|\\\\*)
          PROJ_TOP="$(git -C "$PROJ" rev-parse --show-toplevel 2>/dev/null)"
          # Walk up to the nearest EXISTING ancestor so a not-yet-created file
          # (Write to a new subdir) still resolves to its owning tree — a new
          # path under the integration tree must NOT be wrongly exempted.
          _d="$(dirname "$FP")"
          while [ -n "$_d" ] && [ ! -d "$_d" ]; do _d="$(dirname "$_d")"; done
          FILE_TOP=""
          [ -d "$_d" ] && FILE_TOP="$(git -C "$_d" rev-parse --show-toplevel 2>/dev/null)"
          [ -n "$FILE_TOP" ] && [ "$FILE_TOP" != "$PROJ_TOP" ] && exit 0
          ;;
      esac
      deny "$drift_reason Your last Read is stale and this write would land against the wrong branch."
    fi
    exit 0
    ;;
  *)
    exit 0
    ;;
esac
