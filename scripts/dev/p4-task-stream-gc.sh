#!/usr/bin/env bash
# p4-task-stream-gc.sh — purge stale per-agent Perforce task streams.
#
# Lists task streams under `//smatchet/tasks/...`, filters by mtime, and
# deletes the stream + matching client + on-disk workspace for any stream
# older than the threshold. Skips any stream with pending changelists
# (safety — pending CLs may hold un-shipped agent work).
#
# Counterpart to `bash scripts/dev/p4-task-stream.sh` (the allocator).
# Phase 2 Step 4 of `docs/design/git-to-perforce-migration.md`.
#
# Usage:
#   bash scripts/dev/p4-task-stream-gc.sh [--older-than-days N] [--dry-run]
#
# Options:
#   --older-than-days N  — only purge streams whose updated-time is older
#                          than N days (default: 14)
#   --dry-run            — print what would be purged, take no action
#
# Required environment (see docs/perforce/SETUP.md § 1):
#   P4PORT, P4USER
#
# Optional environment:
#   P4_TASK_PREFIX    — task-stream path prefix INCLUDING trailing separator
#                       (default: //smatchet/task-). Must match the prefix
#                       used by `bash scripts/dev/p4-task-stream.sh`.
#   P4_CLIENT_PREFIX  — client name prefix (default: task)
#   P4_BIN            — p4 executable (default: `p4` from PATH)
#
# Exit codes:
#   0 — GC pass complete (may have deleted 0+ streams)
#   1 — p4 server unreachable
#   2 — argument / environment error

set -euo pipefail

older_than_days=14
dry_run=0

while [ "$#" -gt 0 ]; do
    case "$1" in
        --older-than-days)
            [ "$#" -ge 2 ] || { echo "p4-task-stream-gc: --older-than-days requires a value" >&2; exit 2; }
            older_than_days="$2"
            shift 2
            ;;
        --dry-run)
            dry_run=1
            shift
            ;;
        -h|--help)
            sed -n '2,28p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "p4-task-stream-gc: unknown arg '$1'" >&2
            exit 2
            ;;
    esac
done

if ! printf '%s' "$older_than_days" | grep -qE '^[0-9]+$'; then
    echo "p4-task-stream-gc: --older-than-days must be a non-negative integer" >&2
    exit 2
fi

: "${P4PORT:?p4-task-stream-gc: P4PORT not set; see docs/perforce/SETUP.md § 1}"
: "${P4USER:?p4-task-stream-gc: P4USER not set; see docs/perforce/SETUP.md § 1}"
export P4PORT P4USER

p4="${P4_BIN:-p4}"
# Trailing separator included; default is hyphen because depot depth=1.
task_prefix="${P4_TASK_PREFIX:-//smatchet/task-}"
client_prefix="${P4_CLIENT_PREFIX:-task}"

if ! "$p4" info >/dev/null 2>&1; then
    echo "p4-task-stream-gc: cannot reach p4 server at ${P4PORT}" >&2
    exit 1
fi

# Threshold as a Unix timestamp.
now_epoch=$(date +%s)
threshold_epoch=$((now_epoch - older_than_days * 86400))

scan_pattern="${task_prefix}*"
echo "p4-task-stream-gc: scanning ${scan_pattern} (threshold: ${older_than_days}d / epoch ${threshold_epoch})" >&2

# `p4 streams -F` filters server-side via Jnl-spec selector but mtime semantics
# differ across server versions. Safer: list all, then filter via `p4 stream -o`
# which exposes `Update:` in YYYY/MM/DD HH:MM:SS form.
streams_raw=$("$p4" streams "${scan_pattern}" 2>/dev/null || true)
if [ -z "$streams_raw" ]; then
    echo "p4-task-stream-gc: no streams under ${scan_pattern}; nothing to purge" >&2
    exit 0
fi

purged=0
kept=0
echo "$streams_raw" | awk '{print $2}' | while read -r stream; do
    [ -n "$stream" ] || continue

    # Resolve update epoch from the stream spec
    update_line=$("$p4" stream -o "$stream" 2>/dev/null | grep -E '^Update:' | head -1 || true)
    if [ -z "$update_line" ]; then
        echo "p4-task-stream-gc: ${stream} — no Update field; skip" >&2
        kept=$((kept + 1))
        continue
    fi
    update_str=$(printf '%s' "$update_line" | awk '{print $2" "$3}')
    # GNU date (MSYS2 bash) handles the YYYY/MM/DD HH:MM:SS form via -d
    if ! update_epoch=$(date -d "$update_str" +%s 2>/dev/null); then
        echo "p4-task-stream-gc: ${stream} — could not parse update '${update_str}'; skip" >&2
        kept=$((kept + 1))
        continue
    fi

    if [ "$update_epoch" -ge "$threshold_epoch" ]; then
        echo "p4-task-stream-gc: ${stream} — fresh (updated $update_str); keep" >&2
        kept=$((kept + 1))
        continue
    fi

    # Derive agent-id by stripping the full task_prefix (e.g. //smatchet/task-)
    # — `${stream##*/}` would leave the prefix glued on (task-probe-phase2)
    # because the prefix doesn't end in `/`. Quote the prefix so `*`/`?`/`[`
    # in P4_TASK_PREFIX (if a user picks an unusual override) aren't
    # interpreted as glob patterns during parameter expansion.
    agent_id="${stream#"${task_prefix}"}"
    client="${client_prefix}_${agent_id}"

    # Safety: refuse to GC a stream with pending CLs on its client. Two-step
    # so a pipefail in `... | wc -l` isn't masked by `|| echo 0` returning
    # a multi-line "wc-l-output\n0" value that breaks the integer test.
    if pending_out=$("$p4" changes -s pending -c "$client" 2>/dev/null); then
        pending=$(printf '%s' "$pending_out" | grep -c . || true)
    else
        pending=0
    fi
    if [ "${pending:-0}" -gt 0 ]; then
        echo "p4-task-stream-gc: ${stream} — has ${pending} pending CL(s) on ${client}; keep" >&2
        kept=$((kept + 1))
        continue
    fi

    if [ "$dry_run" = 1 ]; then
        echo "p4-task-stream-gc: DRY-RUN would purge ${stream} (client ${client})" >&2
        purged=$((purged + 1))
        continue
    fi

    echo "p4-task-stream-gc: purging ${stream} (client ${client})" >&2
    # Resolve client root before deleting the client (need it for on-disk cleanup)
    client_root=$("$p4" client -o "$client" 2>/dev/null | grep -E '^Root:' | head -1 | awk '{print $2}' || true)
    "$p4" client -d "$client" >&2 2>&1 || echo "p4-task-stream-gc: client delete returned non-zero (continuing)" >&2
    "$p4" stream -d "$stream" >&2 2>&1 || echo "p4-task-stream-gc: stream delete returned non-zero (continuing)" >&2

    # On-disk cleanup: only inside .claude/streams/<id>/ to avoid surprises
    if [ -n "$client_root" ] && [ -d "$client_root" ]; then
        case "$client_root" in
            *.claude/streams/*|*.claude\\streams\\*)
                echo "p4-task-stream-gc: removing on-disk ${client_root}" >&2
                rm -rf "$client_root"
                ;;
            *)
                echo "p4-task-stream-gc: client_root ${client_root} not under .claude/streams/; leaving on disk" >&2
                ;;
        esac
    fi

    purged=$((purged + 1))
done

# Subshell loses purged/kept counters; re-derive from one final stream list for reporting.
remaining=$("$p4" streams "${scan_pattern}" 2>/dev/null | wc -l || echo 0)
echo "p4-task-stream-gc: done. ${remaining} task stream(s) remain under ${scan_pattern}." >&2
