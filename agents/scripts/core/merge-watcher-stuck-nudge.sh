#!/usr/bin/env bash
# merge-watcher-stuck-nudge.sh — SessionStart nudge for PRs the merge-watcher has
# escalated to STUCK_NEEDS_ATTENTION (see docs/plans/shipped/merge-watcher-stuck-
# escalation.md + merge-watcher.py maybe_escalate_stuck_pr). A wedged PR (merge
# conflict, behind base, CI-failing, or all-green-but-unresolved-threads) sits
# forever otherwise; this surfaces it so a human un-wedges it.
#
# Modes:
#   --list   (default) plain "stuck: PR #N — <reason> (<streak> cycles)" lines.
#   --nudge  SessionStart-formatted block (silent when nothing is stuck).
#
# Also detects daemon ABSENCE (merge-watcher-liveness-unmonitored, infra P2):
# the per-PR state files this script reads are written BY the daemon, so a
# daemon that never starts (or dies later) is silent by construction — with
# PRs registered, that silence means throughput stopped with no signal. When
# the registry is non-empty and the freshest daemon evidence (any per-PR
# last_poll_unix, or daemon.log mtime) is older than
# MERGE_WATCH_NUDGE_ABSENCE_GRACE_SECONDS (default 300 = 5 poll intervals) —
# or there is no evidence at all for a PR registered longer ago than that —
# a "daemon looks DEAD" nudge fires alongside any STUCK lines.
#
# Mirrors postmortem-owed.sh: deterministic read of the watcher state, no action.
# Advisory — never blocks. Exit 0 always (even without python; degrades silent).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

MODE="list"
case "${1:-}" in
    --nudge) MODE="nudge" ;;
    --list|"") MODE="list" ;;
    *) echo "usage: merge-watcher-stuck-nudge.sh [--list|--nudge]" >&2; exit 2 ;;
esac

# python is required to read the watcher's JSON state via the CLI module's own
# watcher_root()/state_dir()/read_registry() (single source of truth for the
# per-user state path). Absent → degrade to a quiet exit (advisory tool).
# Exec-validate each candidate rather than only resolving it: on Windows
# `python3` resolves to the Microsoft Store App Execution Alias stub, which is
# on PATH but exits non-zero on run — a resolve-only probe would pick it and the
# nudge would degrade to silence on a machine that has a working interpreter.
PYBIN=""
for _c in python python3 py; do
    if command -v "$_c" >/dev/null 2>&1 && "$_c" -c "" >/dev/null 2>&1; then
        PYBIN="$(command -v "$_c")"; break
    fi
done
[ -n "$PYBIN" ] || exit 0

"$PYBIN" - "$MODE" "$SCRIPT_DIR" <<'PY'
import importlib.util, json, os, sys, time

mode, sd = sys.argv[1], sys.argv[2]
# Import merge-watcher-cli.py for its watcher_root()/state_dir()/read_registry()
# (import-safe: guarded by `if __name__ == "__main__"`). Any failure → silent.
try:
    spec = importlib.util.spec_from_file_location(
        "mwcli", os.path.join(sd, "merge-watcher-cli.py")
    )
    cli = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(cli)
    state_dir = cli.state_dir()
    entries = cli.read_registry()
    root = cli.watcher_root()
except Exception:
    sys.exit(0)

# Daemon-absence grace: freshest daemon evidence older than this (or no
# evidence at all for an entry registered longer ago than this) = dead.
# Default 5 × the 60 s poll interval, so a slow poll never false-alarms.
try:
    grace = int(os.environ.get("MERGE_WATCH_NUDGE_ABSENCE_GRACE_SECONDS", "300"))
except ValueError:
    grace = 300
now = time.time()

stuck = []
evidence = []       # unix timestamps proving the daemon has run recently
missing_state = []  # PRs registered > grace ago with NO state file at all
log_file = root / "daemon.log"
try:
    if log_file.exists():
        evidence.append(log_file.stat().st_mtime)
except OSError:
    pass
for e in entries:
    try:
        pr = int(e.get("pr", -1))
    except (TypeError, ValueError):
        continue
    sf = state_dir / f"{pr}.json"
    last_state = ""
    if sf.exists():
        try:
            st = json.loads(sf.read_text(encoding="utf-8"))
            last_state = st.get("last_state", "")
            evidence.append(float(st.get("last_poll_unix", 0) or 0))
        except Exception:
            last_state = ""
    else:
        try:
            registered_at = float(e.get("registered_at", 0) or 0)
        except (TypeError, ValueError):
            registered_at = 0.0
        if (now - registered_at) > grace:
            missing_state.append(pr)
    # An ESCALATED wedge is the authoritative signal: the per-PR state file's
    # last_state == STUCK_NEEDS_ATTENTION (the registry stuck_streak alone can be
    # a sub-threshold streak that hasn't escalated yet — don't nudge on those).
    if last_state == "STUCK_NEEDS_ATTENTION":
        stuck.append((pr, e.get("stuck_reason", "?"), e.get("stuck_streak", "?")))

# Daemon-absence verdict — only meaningful when something IS registered
# ("Register with watcher" handed these PRs to a component that must exist).
# No evidence + only fresh registrations = daemon may not have polled yet →
# stay silent rather than false-alarm the install instant.
dead_reason = ""
if entries:
    fresh = max(evidence) if evidence else None
    if fresh is not None and (now - fresh) > grace:
        dead_reason = (
            f"latest daemon evidence (state files / daemon.log) is "
            f"{int(now - fresh)}s old (grace {grace}s)"
        )
    elif fresh is None and missing_state:
        prs = ", ".join(f"#{p}" for p in missing_state)
        dead_reason = (
            f"no daemon output at all — no per-PR state file and no daemon.log — "
            f"yet PR(s) {prs} registered more than {grace}s ago"
        )

if not stuck and not dead_reason:
    sys.exit(0)

if mode == "nudge":
    if stuck:
        print("## === merge-watcher: PR(s) STUCK_NEEDS_ATTENTION ===")
        print(
            "The merge-watcher escalated these wedged PR(s) -- they will NOT merge "
            "without a human action:"
        )
        for pr, reason, streak in stuck:
            print(f"  - PR #{pr}: {reason} (stuck {streak} cycles)")
        print(
            "Un-wedge each (rebase / fix CI / resolve threads) or "
            "`merge-watch unregister <pr>` to hand it back."
        )
    if dead_reason:
        print(f"## === merge-watcher: daemon looks DEAD ({len(entries)} PR(s) registered) ===")
        print(f"{dead_reason}.")
        print(
            "Registered PR(s) will sit green and unmerged until the daemon runs. "
            "On the Windows host re-run the installer (idempotent, now self-verifies):"
        )
        print(
            "  powershell -ExecutionPolicy Bypass -File "
            "agents/scripts/core/merge-watcher-install-autostart.ps1"
        )
        print("then check: Get-ScheduledTaskInfo SmatchetMergeWatcher; tail daemon.log.")
else:
    for pr, reason, streak in stuck:
        print(f"stuck: PR #{pr} — {reason} ({streak} cycles)")
    if dead_reason:
        print(f"watcher-dead: {len(entries)} PR(s) registered — {dead_reason}")
PY
