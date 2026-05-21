#!/usr/bin/env python3
"""
merge-watcher — foreground daemon that polls every registered PR.

Phase 1 of `docs/design/smatchet-merge-watcher.md`. Reads the per-user
registry at `%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (managed
by `merge-watcher-cli.py`), runs `scripts/dev/merge-gates.sh` for each
entry per the configured interval, writes per-PR state to
`%LOCALAPPDATA%/Smatchet/merge-watch/state/<pr>.json`, emits structured
stdout per poll cycle.

Phase-1 scope: poll + observe only. No auto-merge (Phase 2), no triage
(Phase 3), no notify (Phase 4). The daemon parses `merge-gates.sh`
status output but does NOT act on PASS / FAIL — that's the orchestrator's
job until Phase 2 lands the auto-merge branch.

Usage:
  merge-watcher daemon                # foreground (default)
  merge-watcher daemon --background   # detached (Phase 1 stub; for now defers to user using `&`)

Env knobs:
  MERGE_WATCH_POLL_INTERVAL — seconds between per-PR polls (default 60).
  MERGE_GATES_*             — inherited by merge-gates.sh per its own contract.
  ORCH_USER                 — required by merge-gates.sh.
"""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import pathlib
import signal
import subprocess
import sys
import time
from typing import Any

# Import shared helpers from the CLI module (in the same directory).
_HERE = pathlib.Path(__file__).resolve().parent
_CLI_SPEC = importlib.util.spec_from_file_location("merge_watcher_cli", _HERE / "merge-watcher-cli.py")
_CLI = importlib.util.module_from_spec(_CLI_SPEC)
_CLI_SPEC.loader.exec_module(_CLI)  # type: ignore[union-attr]
watcher_root = _CLI.watcher_root
state_dir = _CLI.state_dir
read_registry = _CLI.read_registry


MERGE_GATES_SCRIPT = _HERE / "merge-gates.sh"


# ---------------------------------------------------------------------------
# PID file
# ---------------------------------------------------------------------------
def pid_file() -> pathlib.Path:
    return watcher_root() / "daemon.pid"


def write_pid_file() -> None:
    watcher_root().mkdir(parents=True, exist_ok=True)
    p = pid_file()
    p.write_text(
        json.dumps(
            {"pid": os.getpid(), "started_at_unix": int(time.time())},
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )


def clear_pid_file() -> None:
    p = pid_file()
    if p.exists():
        try:
            p.unlink()
        except OSError:
            pass


# ---------------------------------------------------------------------------
# Per-PR poll
# ---------------------------------------------------------------------------
def poll_one(entry: dict[str, Any]) -> dict[str, Any]:
    """Run merge-gates.sh for one registered PR, parse the last status line,
    return a state dict the daemon writes to state/<pr>.json.
    """
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    if not pathlib.Path(clone_path).exists():
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "CLONE_MISSING",
            "last_status_line": f"clone path {clone_path} does not exist",
        }
    # Resolve owner / repo via `gh repo view --json owner,name` in the clone.
    try:
        meta = subprocess.run(
            ["gh", "repo", "view", "--json", "owner,name"],
            cwd=clone_path,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "GH_UNAVAILABLE",
            "last_status_line": f"gh repo view failed: {exc}",
        }
    if meta.returncode != 0:
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "GH_REPO_ERR",
            "last_status_line": f"gh repo view exited {meta.returncode}: {meta.stderr.strip()[:200]}",
        }
    try:
        meta_json = json.loads(meta.stdout)
        owner = meta_json["owner"]["login"]
        repo = meta_json["name"]
    except (json.JSONDecodeError, KeyError) as exc:
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "GH_PARSE_ERR",
            "last_status_line": f"gh repo view JSON parse: {exc}",
        }
    # Invoke merge-gates.sh once (single poll iteration — MERGE_GATES_MAX_POLLS=1).
    env = os.environ.copy()
    env.setdefault("MERGE_GATES_MAX_POLLS", "1")
    env.setdefault("MERGE_GATES_POLL_INTERVAL", "0")
    try:
        gates = subprocess.run(
            ["bash", str(MERGE_GATES_SCRIPT), owner, repo, str(pr)],
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "GATES_SCRIPT_ERR",
            "last_status_line": f"merge-gates.sh failed: {exc}",
        }
    # Parse the last status line — merge-gates.sh emits one line per poll
    # (here, MAX=1 → exactly one). Phase 1 records the raw line + return code;
    # Phase 2 will branch on the parsed state.
    status_lines = [ln for ln in gates.stdout.splitlines() if ln.startswith("Poll ")]
    last_line = status_lines[-1] if status_lines else gates.stdout.strip().splitlines()[-1] if gates.stdout.strip() else ""
    state_label = {
        0: "GATES_PASSED",
        1: "BLOCKED",
        2: "TIMEOUT",
        3: "GH_API_DOWN",
        4: "PR_CLOSED_OR_MERGED",
        5: "PAGINATION_OVERFLOW",
    }.get(gates.returncode, f"EXIT_{gates.returncode}")
    return {
        "pr": pr,
        "clone_path": clone_path,
        "last_poll_unix": int(time.time()),
        "last_state": state_label,
        "last_status_line": last_line,
        "gates_return_code": gates.returncode,
    }


def write_state(state: dict[str, Any]) -> None:
    state_dir().mkdir(parents=True, exist_ok=True)
    p = state_dir() / f"{int(state['pr'])}.json"
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(state, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(p)


# ---------------------------------------------------------------------------
# Daemon loop
# ---------------------------------------------------------------------------
class StopSignal(Exception):
    pass


def install_signal_handlers() -> None:
    def _raise(_signum, _frame):
        raise StopSignal()

    signal.signal(signal.SIGINT, _raise)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, _raise)


def daemon_loop(poll_interval: int) -> int:
    print(
        f"merge-watcher: daemon started (PID {os.getpid()}, poll_interval={poll_interval}s).\n"
        f"  Registry: {watcher_root()}\n"
        f"  Press Ctrl-C to stop."
    )
    write_pid_file()
    cycle = 0
    try:
        while True:
            cycle += 1
            entries = read_registry()
            if not entries:
                print(f"[cycle {cycle}] registry empty; sleeping {poll_interval}s")
            else:
                print(f"[cycle {cycle}] polling {len(entries)} registered PR(s)")
                for entry in entries:
                    state = poll_one(entry)
                    write_state(state)
                    print(
                        f"  PR#{state['pr']:<6} state={state['last_state']:<24} "
                        f"poll_line={state.get('last_status_line', '')[:120]}"
                    )
            time.sleep(poll_interval)
    except StopSignal:
        print("\nmerge-watcher: stop signal received; exiting cleanly.")
        return 0
    finally:
        clear_pid_file()


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="merge-watcher",
        description=(
            "Foreground daemon for smatchet-merge-watcher Phase 1. "
            "Polls every registered PR via scripts/dev/merge-gates.sh + writes per-PR state."
        ),
    )
    sub = p.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("daemon", help="run the poll loop (foreground default)")
    d.add_argument(
        "--background",
        action="store_true",
        help="(Phase 1 stub) detach to background — currently a no-op + warning; use shell `&` for now",
    )
    d.add_argument(
        "--poll-interval",
        type=int,
        default=int(os.environ.get("MERGE_WATCH_POLL_INTERVAL", "60")),
        help="seconds between poll cycles (default $MERGE_WATCH_POLL_INTERVAL or 60)",
    )
    d.set_defaults(func=lambda args: daemon_loop(args.poll_interval))
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if getattr(args, "background", False):
        print(
            "merge-watcher: --background is a Phase-1 stub (no-op). "
            "Use shell `&` to detach for now; Phase 2 lands proper detach.",
            file=sys.stderr,
        )
    install_signal_handlers()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
