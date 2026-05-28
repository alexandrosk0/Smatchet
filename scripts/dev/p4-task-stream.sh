#!/usr/bin/env bash
# p4-task-stream.sh — allocate a per-agent Perforce task stream + workspace.
#
# Creates `//smatchet/task-<agent-id>` as a task stream parented to
# `//smatchet/main`, plus a client `task_<agent-id>` rooted at
# `<repo>/.claude/streams/<agent-id>/`, then syncs the workspace to HEAD.
# (Single-segment depot name because `//smatchet` has `StreamDepth: //smatchet/1`;
# override via `P4_TASK_PREFIX` if you rebuild the depot deeper.)
# Idempotent — if stream + client already exist, only the sync runs.
# Idempotent re-populate — if the task stream exists but holds zero revs
# (e.g. a prior populate aborted mid-flight), re-running populates it.
#
# This is the Perforce sibling of `git worktree add .claude/worktrees/<id>`
# used by git-only sessions. Triggered by orchestrators that opt into the
# dual-VCS layer with `SMATCHET_AGENT_VCS=p4`.
#
# Phase 2 Step 2 of `docs/design/archive/git-to-perforce-migration.md`.
#
# Usage:
#   bash scripts/dev/p4-task-stream.sh <agent-id>
#
# Arguments:
#   agent-id — kebab-case identifier, [a-z0-9][a-z0-9-]{0,63}
#
# Required environment (from `bash scripts/dev/setup-p4-env.sh` or persisted
# user-scope env vars — see docs/perforce/SETUP.md § 1):
#   P4PORT   — server addr (e.g. localhost:1666)
#   P4USER   — Perforce user name
#
# Optional environment:
#   P4_STREAM_PARENT  — parent stream (default: //smatchet/main)
#   P4_TASK_PREFIX    — task-stream path prefix INCLUDING trailing separator
#                       (default: //smatchet/task-). Names are single-segment
#                       because depot //smatchet has StreamDepth: //smatchet/1
#                       — a `/` separator would make 2 segments and fail.
#   P4_CLIENT_PREFIX  — client name prefix (default: task)
#   P4_BIN            — p4 executable (default: `p4` from PATH)
#
# Output:
#   stdout — absolute path to the workspace root (caller `cd`s into it)
#   stderr — progress lines
#
# Exit codes:
#   0 — workspace ready; path printed to stdout
#   1 — p4 server unreachable / spec submission failed
#   2 — argument / environment error

set -euo pipefail

usage() {
    echo "usage: bash scripts/dev/p4-task-stream.sh <agent-id>" >&2
    exit 2
}

[ "$#" -eq 1 ] || usage
agent_id="$1"

if ! printf '%s' "$agent_id" | grep -qE '^[a-z0-9][a-z0-9-]{0,63}$'; then
    echo "p4-task-stream: invalid agent-id '$agent_id' — must match [a-z0-9][a-z0-9-]{0,63}" >&2
    exit 2
fi

# --- env resolution -------------------------------------------------------
: "${P4PORT:?p4-task-stream: P4PORT not set; see docs/perforce/SETUP.md § 1}"
: "${P4USER:?p4-task-stream: P4USER not set; see docs/perforce/SETUP.md § 1}"
export P4PORT P4USER

p4="${P4_BIN:-p4}"
parent="${P4_STREAM_PARENT:-//smatchet/main}"
# Trailing separator included in prefix; default is hyphen because depot depth=1
# forbids `/` segmentation here. Override env to e.g. `//smatchet/tasks/` if you
# rebuilt the depot at higher StreamDepth.
task_prefix="${P4_TASK_PREFIX:-//smatchet/task-}"
client_prefix="${P4_CLIENT_PREFIX:-task}"

stream="${task_prefix}${agent_id}"
client="${client_prefix}_${agent_id}"

# --- repo + workspace path resolution -------------------------------------
repo_root="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "p4-task-stream: not inside a git checkout (need repo root for workspace placement)" >&2
    exit 2
}

ws_dir="${repo_root}/.claude/streams/${agent_id}"
mkdir -p "$ws_dir"

# p4d on Windows wants a native Windows path (C:\...). MSYS2/Git Bash needs cygpath.
if command -v cygpath >/dev/null 2>&1; then
    ws_dir_native="$(cygpath -w "$ws_dir")"
else
    ws_dir_native="$ws_dir"
fi

# --- p4 server reachability ----------------------------------------------
if ! "$p4" info >/dev/null 2>&1; then
    echo "p4-task-stream: cannot reach p4 server at ${P4PORT}" >&2
    exit 1
fi

# --- create / verify the task stream --------------------------------------
stream_was_created=0
if "$p4" streams "${stream}" 2>/dev/null | grep -q "Stream ${stream}"; then
    echo "p4-task-stream: stream ${stream} already exists, reusing" >&2
else
    echo "p4-task-stream: creating task stream ${stream} parented to ${parent}" >&2
    spec=$(cat <<EOF
Stream:     ${stream}
Owner:      ${P4USER}
Name:       ${agent_id}
Parent:     ${parent}
Type:       task
Options:    allsubmit unlocked toparent fromparent mergedown
ParentView: inherit
Paths:      share ...
EOF
)
    printf '%s\n' "$spec" | "$p4" stream -i >&2
    stream_was_created=1
fi

# --- create / verify the client workspace ---------------------------------
if "$p4" clients -u "$P4USER" 2>/dev/null | grep -q "Client ${client} "; then
    echo "p4-task-stream: client ${client} already exists, reusing" >&2
else
    echo "p4-task-stream: creating client ${client} rooted at ${ws_dir_native}" >&2
    host="$(hostname 2>/dev/null || echo unknown)"
    spec=$(cat <<EOF
Client:        ${client}
Owner:         ${P4USER}
Host:          ${host}
Description:   Smatchet task-stream workspace for agent ${agent_id} (Phase 2).
Root:          ${ws_dir_native}
Options:       noallwrite noclobber nocompress unlocked nomodtime normdir
SubmitOptions: submitunchanged
LineEnd:       local
Stream:        ${stream}
EOF
)
    printf '%s\n' "$spec" | "$p4" client -i >&2
fi

# --- seed the task stream from parent (one-time on creation OR recovery) -
# Task streams hold zero revs until something is branched into them.
# `p4 populate` lazy-branches every parent file into the task stream's
# depot path so the next `p4 sync` actually finds something to fetch.
# Without this, `p4 sync` returns "No such file(s)." against a virgin task
# stream — silently giving the caller an empty workspace.
#
# Re-populate when the stream exists but has zero files (e.g. a prior
# allocate aborted between `p4 stream -i` and `p4 populate`, leaving an
# unpopulated husk). `p4 populate` is naturally idempotent against an
# already-populated stream — it returns "all revision(s) already
# integrated" and exits non-zero, which we ignore.
needs_populate="$stream_was_created"
if [ "$needs_populate" = 0 ]; then
    # Match `//depot/path#<rev>` (real file entries) — bare `^//` would also
    # match `p4 files` errors like `//smatchet/task-foo/... - no such file(s).`
    # which would falsely report "stream has files" against a virgin stream.
    stream_file_count=$("$p4" files -m1 "${stream}/..." 2>&1 | grep -cE '^//.*#[0-9]+' || true)
    if [ "${stream_file_count:-0}" = "0" ]; then
        echo "p4-task-stream: ${stream} exists but holds zero revs — re-populating" >&2
        needs_populate=1
    fi
fi
if [ "$needs_populate" = 1 ]; then
    echo "p4-task-stream: seeding ${stream} from ${parent} via p4 populate" >&2
    "$p4" populate -d "seed task stream ${agent_id} from parent ${parent}" \
        "${parent}/..." "${stream}/..." >&2 || \
        echo "p4-task-stream: populate returned non-zero (already integrated?) — continuing" >&2
fi

# --- sync workspace to HEAD ----------------------------------------------
echo "p4-task-stream: syncing client view (head) into ${ws_dir_native}" >&2
P4CLIENT="$client" "$p4" sync >&2

# --- emit workspace path on stdout (caller cd's into it) ------------------
printf '%s\n' "$ws_dir_native"
