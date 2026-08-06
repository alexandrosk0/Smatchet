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
import importlib.util, json, os, sys

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
except Exception:
    sys.exit(0)

stuck = []
for e in entries:
    try:
        pr = int(e.get("pr", -1))
    except (TypeError, ValueError):
        continue
    sf = state_dir / f"{pr}.json"
    last_state = ""
    if sf.exists():
        try:
            last_state = json.loads(sf.read_text(encoding="utf-8")).get("last_state", "")
        except Exception:
            last_state = ""
    # An ESCALATED wedge is the authoritative signal: the per-PR state file's
    # last_state == STUCK_NEEDS_ATTENTION (the registry stuck_streak alone can be
    # a sub-threshold streak that hasn't escalated yet — don't nudge on those).
    if last_state == "STUCK_NEEDS_ATTENTION":
        stuck.append((pr, e.get("stuck_reason", "?"), e.get("stuck_streak", "?")))

if not stuck:
    sys.exit(0)

if mode == "nudge":
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
else:
    for pr, reason, streak in stuck:
        print(f"stuck: PR #{pr} — {reason} ({streak} cycles)")
PY
