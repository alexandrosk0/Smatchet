#!/usr/bin/env bash
# scripts/dev/worktree.sh — isolated-session worktree lifecycle for concurrent
# Claude Code sessions.
#
# Bash port of the retired worktree.ps1.
#
# Each concurrent session should live in its OWN git worktree (own HEAD / index /
# working tree) so a branch switch, pull, or reset in one session cannot corrupt
# another. The main clone (C:/Dev/Smatchet) is the stable INTEGRATION tree —
# pinned to develop, used for pull/merge/review, never for feature work.
#
# See docs/agent-rules/process-rules.md § Concurrent interactive sessions.
#
# Subcommands:
#   new <slug>     Create a worktree at <trees-root>/<slug> on feat/<slug> off
#                  origin/develop, wire its .claude/ adapter, print the launch line.
#   resync         Re-baseline every session entry rooted in THIS tree to current
#                  HEAD — the recovery path after a HEAD-drift block (accept
#                  "I'm on this branch now" without switching the shared HEAD).
#   list           Show all worktrees + branch / dirty / live-session count.
#   rm <slug>      Remove the worktree (and optionally its branch).
#   prune          git worktree prune + report worktrees already merged to develop.
#
# The trees root defaults to C:/Dev/trees (override: SMATCHET_TREES_ROOT).
#
# Examples:
#   bash scripts/dev/worktree.sh new vsync-toggle
#   bash scripts/dev/worktree.sh list
#   bash scripts/dev/worktree.sh rm vsync-toggle --delete-branch

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
# scripts/dev/worktree.sh -> repo root is two levels up. When run from a worktree
# this is that worktree (its .git points at the shared object store, so
# `worktree add` still registers globally).
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
TREES_ROOT="${SMATCHET_TREES_ROOT:-C:/Dev/trees}"

die() { echo "worktree: $*" >&2; exit 1; }

usage() {
    cat <<'EOF'
Usage: bash scripts/dev/worktree.sh <command> [<slug>] [options]

Commands:
  new <slug>      Create a worktree on feat/<slug> off origin/develop and wire .claude/
  resync          Re-baseline this tree's session entries to the current HEAD
  list            List worktrees with branch / dirty / live-session count
  rm <slug>       Remove a worktree
  prune           Prune stale admin entries; report merged worktrees

Options:
  --base <ref>      Base branch for `new` (default: develop)
  --branch <name>   Branch name override (default: feat/<slug>)
  --force           Force `rm` (and use `git branch -D`)
  --delete-branch   Also delete the branch on `rm`
  -h, --help        Show this help
EOF
}

COMMAND=""
SLUG=""
BASE="develop"
BRANCH=""
FORCE=0
DELETE_BRANCH=0

# `shift 2` fails WITHOUT shifting when the option value is missing, which
# would spin this loop forever (no `set -e` here). Reject that up front.
need_value() {
    [ "$1" -ge 2 ] || { echo "worktree: $2 requires a value" >&2; exit 2; }
}

while [ $# -gt 0 ]; do
    case "$1" in
        --base)          need_value $# "$1"; BASE="$2"; shift 2 ;;
        --base=*)        BASE="${1#*=}"; shift ;;
        --branch)        need_value $# "$1"; BRANCH="$2"; shift 2 ;;
        --branch=*)      BRANCH="${1#*=}"; shift ;;
        --force)         FORCE=1; shift ;;
        --delete-branch) DELETE_BRANCH=1; shift ;;
        -h|--help)       usage; exit 0 ;;
        -*)              echo "worktree: unknown option: $1" >&2; usage >&2; exit 2 ;;
        *)
            if [ -z "$COMMAND" ]; then COMMAND="$1"
            elif [ -z "$SLUG" ]; then SLUG="$1"
            else echo "worktree: unexpected argument: $1" >&2; exit 2
            fi
            shift
            ;;
    esac
done

[ -n "$COMMAND" ] || { usage >&2; exit 2; }

# Guard against path traversal / shell-odd slugs before they reach paths +
# branch names: letters, digits, dot, underscore, hyphen only.
assert_safe_slug() {
    case "$1" in
        .|..) die "Invalid slug '$1' — use only letters, digits, dot, underscore, hyphen." ;;
    esac
    printf '%s' "$1" | grep -Eq '^[A-Za-z0-9._-]+$' \
        || die "Invalid slug '$1' — use only letters, digits, dot, underscore, hyphen (no spaces or path separators)."
}

git_out() { git "$@" 2>/dev/null | tr -d '\r'; }

has_ref() { git -C "$REPO_ROOT" rev-parse --verify --quiet "$1" >/dev/null 2>&1; }

# --- Per-tree session registry (same format the bash hooks write) ------------
registry_dir() { printf '%s/.claude/.active-sessions' "$1"; }

# A session entry counts as live when its heartbeat is fresh (<30 min) or its
# recorded pid still exists.
live_session_count() {
    local dir entry ts pid now live=0
    dir="$(registry_dir "$1")"
    [ -d "$dir" ] || { printf '0'; return; }
    now="$(date +%s)"
    for entry in "$dir"/*; do
        [ -f "$entry" ] || continue
        ts="$(sed -n 's/^ts=\([0-9][0-9]*\).*/\1/p' "$entry" | head -1)"
        pid="$(sed -n 's/^ppid=\([0-9][0-9]*\).*/\1/p' "$entry" | head -1)"
        if [ -n "${ts:-}" ] && [ "$((now - ts))" -lt 1800 ]; then
            live=$((live + 1))
            continue
        fi
        if [ -n "${pid:-}" ] && [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
            live=$((live + 1))
        fi
    done
    printf '%s' "$live"
}

cmd_new() {
    [ -n "$SLUG" ] || die "new requires a <slug>: worktree.sh new <slug>"
    assert_safe_slug "$SLUG"
    local branch_name path
    branch_name="${BRANCH:-feat/$SLUG}"
    path="$TREES_ROOT/$SLUG"
    [ -e "$path" ] && die "Path already exists: $path"
    mkdir -p "$TREES_ROOT" || die "cannot create trees root: $TREES_ROOT"

    # First-run bootstrap: close the #913 fresh-clone hole. The HEAD-drift guard
    # lives in the gitignored .claude/, wired only by setup-harness. If THIS tree
    # (the integration clone the launcher runs from) has never been provisioned,
    # its guard is INACTIVE — so a concurrent session in the main clone is
    # unprotected until someone remembers to run setup-harness. Provision it now,
    # before spinning the worktree. Idempotent; non-fatal.
    # See docs/harness/SETUP.md § Concurrent-session HEAD-drift guard.
    if [ ! -f "$REPO_ROOT/.claude/hooks/guard-head-drift.sh" ]; then
        echo "First run: provisioning .claude/ in the integration tree ($REPO_ROOT) so its HEAD-drift guard is active ..."
        bash "$REPO_ROOT/agents/scripts/core/setup-harness.sh" claude-code \
            || echo "  WARNING: could not provision the integration tree's .claude/ — run 'bash agents/scripts/core/setup-harness.sh claude-code' there manually." >&2
    fi

    echo "Fetching origin/$BASE ..."
    git -C "$REPO_ROOT" fetch --quiet origin "$BASE" || echo "  (fetch failed — using local $BASE)" >&2

    local base_ref="$BASE"
    has_ref "origin/$BASE" && base_ref="origin/$BASE"

    if has_ref "refs/heads/$branch_name"; then
        echo "Branch $branch_name exists — attaching worktree."
        git -C "$REPO_ROOT" worktree add "$path" "$branch_name" || die "worktree add failed"
    else
        git -C "$REPO_ROOT" worktree add -b "$branch_name" "$path" "$base_ref" || die "worktree add failed"
    fi

    # Wire the worktree's OWN .claude/ adapter (hooks incl. the drift guard) by
    # running the worktree's copy of setup-harness — it resolves its root from
    # its own location, so this provisions $path, not the main clone.
    echo "Wiring .claude/ adapter in the new worktree ..."
    local wired=1
    bash "$path/agents/scripts/core/setup-harness.sh" claude-code || wired=0

    echo ""
    if [ "$wired" -eq 1 ] && [ -f "$path/.claude/settings.json" ]; then
        echo "Worktree ready: $path  (branch $branch_name)"
        echo "Launch a session there:"
        echo "  cd \"$path\" && claude"
    else
        echo "WARNING: worktree created at $path (branch $branch_name) but .claude/ is NOT wired —" >&2
        echo "         the HEAD-drift guard is INACTIVE there. Finish setup before relying on isolation:" >&2
        echo "         bash \"$path/agents/scripts/core/setup-harness.sh\" claude-code" >&2
    fi
}

cmd_resync() {
    local tree cur_branch cur_sha dir now entry pid count=0
    tree="$(git_out -C "$REPO_ROOT" rev-parse --show-toplevel)"
    [ -n "$tree" ] || tree="$REPO_ROOT"
    cur_branch="$(git_out -C "$tree" symbolic-ref --short HEAD)"
    cur_sha="$(git_out -C "$tree" rev-parse HEAD)"
    { [ -n "$cur_branch" ] && [ -n "$cur_sha" ]; } \
        || die "Cannot resolve current HEAD (detached or mid-rebase?) in $tree"

    dir="$(registry_dir "$tree")"
    [ -d "$dir" ] || { echo "No session registry in this tree — nothing to resync."; return 0; }
    now="$(date +%s)"
    for entry in "$dir"/*; do
        [ -f "$entry" ] || continue
        pid="$(sed -n 's/^ppid=\([0-9][0-9]*\).*/\1/p' "$entry" | head -1)"
        printf 'branch=%s\nsha=%s\nppid=%s\nts=%s\n' \
            "$cur_branch" "$cur_sha" "${pid:-0}" "$now" > "$entry"
        count=$((count + 1))
    done
    echo "Re-baselined $count session entry(ies) in $tree to $cur_branch@${cur_sha:0:8}."
}

# Walk `git worktree list --porcelain`, calling back with path + branch label.
# Emits "<path>\t<branch-or-(detached)>" lines.
worktree_pairs() {
    local wt="" line
    while IFS= read -r line; do
        line="${line%$'\r'}"
        case "$line" in
            worktree\ *) wt="${line#worktree }" ;;
            branch\ *)
                [ -n "$wt" ] && printf '%s\t%s\n' "$wt" "${line#branch refs/heads/}"
                wt=""
                ;;
            detached)
                [ -n "$wt" ] && printf '%s\t%s\n' "$wt" "(detached)"
                wt=""
                ;;
        esac
    done < <(git -C "$REPO_ROOT" worktree list --porcelain 2>/dev/null)
}

cmd_list() {
    local any=0 path branch dirty live
    printf '%-44s %-26s %-6s %s\n' "PATH" "BRANCH" "DIRTY" "LIVE"
    while IFS=$'\t' read -r path branch; do
        any=1
        if [ "$branch" = "(detached)" ]; then
            dirty="-"
        elif [ -z "$(git_out -C "$path" status --short)" ]; then
            dirty="clean"
        else
            dirty="DIRTY"
        fi
        live="$(live_session_count "$path")"
        printf '%-44s %-26s %-6s %s\n' "$path" "$branch" "$dirty" "$live"
    done < <(worktree_pairs)
    [ "$any" -eq 1 ] || echo "No worktrees."
}

cmd_rm() {
    [ -n "$SLUG" ] || die "rm requires a <slug>: worktree.sh rm <slug>"
    local path branch_name flag
    if [ -d "$SLUG" ]; then
        path="$SLUG"
    else
        assert_safe_slug "$SLUG"
        path="$TREES_ROOT/$SLUG"
    fi
    if [ "$FORCE" -eq 1 ]; then
        git -C "$REPO_ROOT" worktree remove --force "$path" || die "worktree remove failed"
    else
        git -C "$REPO_ROOT" worktree remove "$path" || die "worktree remove failed"
    fi
    echo "Removed worktree: $path"

    if [ "$DELETE_BRANCH" -eq 1 ]; then
        branch_name="${BRANCH:-feat/$SLUG}"
        flag="-d"
        [ "$FORCE" -eq 1 ] && flag="-D"
        if git -C "$REPO_ROOT" branch "$flag" "$branch_name"; then
            echo "Deleted branch: $branch_name"
        else
            echo "  branch $branch_name not deleted (unmerged? use --force)" >&2
        fi
    fi
}

cmd_prune() {
    local path branch
    git -C "$REPO_ROOT" worktree prune || die "worktree prune failed"
    echo "Pruned stale worktree admin entries."
    while IFS=$'\t' read -r path branch; do
        case "$branch" in develop|main|"(detached)") continue ;; esac
        if git -C "$REPO_ROOT" merge-base --is-ancestor "$branch" origin/develop >/dev/null 2>&1; then
            echo "  MERGED -> safe to remove: $path ($branch)"
        fi
    done < <(worktree_pairs)
}

case "$COMMAND" in
    new)    cmd_new ;;
    resync) cmd_resync ;;
    list)   cmd_list ;;
    rm)     cmd_rm ;;
    prune)  cmd_prune ;;
    *)      echo "worktree: unknown command: $COMMAND" >&2; usage >&2; exit 2 ;;
esac
