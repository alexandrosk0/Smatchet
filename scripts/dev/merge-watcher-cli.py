#!/usr/bin/env python3
"""
merge-watcher-cli — register / unregister / status / list subcommands.

Phase 1 of `docs/design/archive/smatchet-merge-watcher.md`. Per-user registry at
`%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (cross-clone). The
companion daemon (`merge-watcher.py`) polls every registered PR via
`scripts/dev/merge-gates.sh`.

Phase-1 scope: registry CRUD only. No auto-merge (Phase 2), no triage
(Phase 3), no notify (Phase 4). The daemon prints per-PR state to stdout;
state transitions are observed by reading `state/<pr>.json` files.

Owner transfer: `register` prints "watcher now owns this PR; use
`unregister` to take back control" per the locked design decision. The
orchestrator is expected to check the registry before any merge-gates
poll and skip if the PR is registered.

Usage:
  merge-watch register <pr>        # add PR to registry (clone_path = cwd repo root)
  merge-watch unregister <pr>      # remove PR
  merge-watch status [<pr>]        # show one PR's state or all
  merge-watch list                 # JSON dump of full registry
"""

from __future__ import annotations

import argparse
import contextlib
import json
import os
import pathlib
import subprocess
import sys
import time
from typing import Any


# ---------------------------------------------------------------------------
# Per-user registry path resolution
# ---------------------------------------------------------------------------
def watcher_root() -> pathlib.Path:
    """`%LOCALAPPDATA%/Smatchet/merge-watch/` on Windows; XDG-style fallback otherwise.

    Per the 2026-05-21 plan-doc locked decision: per-user registry watches
    PRs across all Smatchet clones. The path is OUTSIDE any specific git
    clone so a `git clean -fx` doesn't nuke watch state.
    """
    if os.name == "nt":
        base = os.environ.get("LOCALAPPDATA")
        if not base:
            raise RuntimeError("LOCALAPPDATA not set; cannot resolve watcher root on Windows")
        return pathlib.Path(base) / "Smatchet" / "merge-watch"
    # POSIX fallback — XDG_STATE_HOME with $HOME/.local/state default.
    xdg = os.environ.get("XDG_STATE_HOME") or str(pathlib.Path.home() / ".local" / "state")
    return pathlib.Path(xdg) / "smatchet" / "merge-watch"


def registry_path() -> pathlib.Path:
    return watcher_root() / "active.json"


def state_dir() -> pathlib.Path:
    return watcher_root() / "state"


def lockfile_path() -> pathlib.Path:
    return watcher_root() / "active.json.lockfile"


# ---------------------------------------------------------------------------
# File-locked registry read / write
# ---------------------------------------------------------------------------
@contextlib.contextmanager
def registry_lock(timeout_seconds: float = 10.0):
    """Cross-platform file lock around the registry.

    Uses a sentinel file with exclusive `O_CREAT | O_EXCL`. Polls every
    50ms up to `timeout_seconds` before raising `TimeoutError`. The
    cleanup `os.unlink` runs even on exception via try/finally.

    Lock contention is expected to be rare: foreground-default daemon
    means multi-daemon misconfigurations are visible; the CLI commands
    are short-lived (sub-second) and only one runs at a time per user
    invocation.
    """
    watcher_root().mkdir(parents=True, exist_ok=True)
    lock = lockfile_path()
    deadline = time.monotonic() + timeout_seconds
    while True:
        try:
            fd = os.open(str(lock), os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, f"{os.getpid()}\n".encode())
            os.close(fd)
            break
        except FileExistsError:
            if time.monotonic() > deadline:
                raise TimeoutError(
                    f"merge-watcher: registry lock at {lock} held > {timeout_seconds}s"
                )
            time.sleep(0.05)
    try:
        yield
    finally:
        try:
            os.unlink(str(lock))
        except FileNotFoundError:
            pass


def read_registry() -> list[dict[str, Any]]:
    p = registry_path()
    if not p.exists():
        return []
    try:
        data = json.loads(p.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"merge-watcher: registry at {p} is malformed JSON: {exc}") from exc
    if not isinstance(data, list):
        raise RuntimeError(f"merge-watcher: registry at {p} is not a JSON list")
    return data


def write_registry(entries: list[dict[str, Any]]) -> None:
    p = registry_path()
    p.parent.mkdir(parents=True, exist_ok=True)
    # Atomic-replace via tempfile rename so a partial write doesn't corrupt the registry.
    tmp = p.with_suffix(p.suffix + ".tmp")
    tmp.write_text(json.dumps(entries, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    tmp.replace(p)


# ---------------------------------------------------------------------------
# Clone-path resolution (gh / git surfaces)
# ---------------------------------------------------------------------------
def resolve_clone_path(cwd: str | pathlib.Path | None = None) -> str:
    """Find the git repo root for `cwd` (default: `os.getcwd()`).

    Raises if not inside a git repo.
    """
    cwd_str = str(cwd or os.getcwd())
    result = subprocess.run(
        ["git", "-C", cwd_str, "rev-parse", "--show-toplevel"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"merge-watcher: cwd '{cwd_str}' is not inside a git repository "
            f"(`git rev-parse --show-toplevel` exited {result.returncode}): "
            f"{result.stderr.strip()}"
        )
    return result.stdout.strip()


# ---------------------------------------------------------------------------
# Subcommands
# ---------------------------------------------------------------------------
def cmd_register(args: argparse.Namespace) -> int:
    pr = int(args.pr)
    clone_path = resolve_clone_path()
    now = int(time.time())
    with registry_lock():
        entries = read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                print(
                    f"merge-watch: PR #{pr} already registered for clone {clone_path} "
                    f"(registered_at={e.get('registered_at', '?')})",
                    file=sys.stderr,
                )
                return 1
        entries.append(
            {
                "pr": pr,
                "clone_path": clone_path,
                "registered_at": now,
                "triage_attempts": 0,
            }
        )
        write_registry(entries)
    print(
        f"merge-watch: registered PR #{pr} for clone {clone_path}.\n"
        f"  Watcher now owns this PR; use `merge-watch unregister {pr}` to take back control.\n"
        f"  The orchestrator must check this registry before any merge-gates poll + skip if "
        f"the PR is registered."
    )
    return 0


def cmd_unregister(args: argparse.Namespace) -> int:
    pr = int(args.pr)
    clone_path = resolve_clone_path()
    with registry_lock():
        entries = read_registry()
        before = len(entries)
        entries = [
            e
            for e in entries
            if not (int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path)
        ]
        removed = before - len(entries)
        if removed == 0:
            print(
                f"merge-watch: PR #{pr} not registered for clone {clone_path}; nothing to do.",
                file=sys.stderr,
            )
            return 1
        write_registry(entries)
        # Also drop the per-PR state file so a re-register starts clean.
        state = state_dir() / f"{pr}.json"
        if state.exists():
            state.unlink()
    print(f"merge-watch: unregistered PR #{pr}. Ownership returned to orchestrator.")
    return 0


def cmd_status(args: argparse.Namespace) -> int:
    pr_filter = int(args.pr) if args.pr is not None else None
    entries = read_registry()
    if pr_filter is not None:
        entries = [e for e in entries if int(e.get("pr", -1)) == pr_filter]
    if not entries:
        if pr_filter is not None:
            print(f"merge-watch: PR #{pr_filter} not registered.")
        else:
            print("merge-watch: registry empty.")
        return 0
    # Pretty table.
    rows = []
    for e in entries:
        pr = int(e.get("pr", -1))
        state_file = state_dir() / f"{pr}.json"
        last_state = "(no poll yet)"
        last_poll = "-"
        if state_file.exists():
            try:
                s = json.loads(state_file.read_text(encoding="utf-8"))
                last_state = s.get("last_state", "?")
                last_poll = time.strftime("%H:%M:%S", time.localtime(s.get("last_poll_unix", 0)))
            except json.JSONDecodeError:
                last_state = "(state-file corrupt)"
        rows.append(
            (
                f"#{pr}",
                pathlib.Path(e.get("clone_path", "?")).name or "?",
                last_state,
                last_poll,
                str(e.get("triage_attempts", 0)),
            )
        )
    widths = [max(len(r[i]) for r in [("PR", "CLONE", "LAST_STATE", "LAST_POLL", "TRIAGE"), *rows]) for i in range(5)]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    print(fmt.format("PR", "CLONE", "LAST_STATE", "LAST_POLL", "TRIAGE"))
    print("-" * (sum(widths) + 8))
    for r in rows:
        print(fmt.format(*r))
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    print(json.dumps(read_registry(), indent=2, sort_keys=True))
    return 0


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="merge-watch",
        description=(
            "Per-user CLI for smatchet-merge-watcher Phase 1 (registry CRUD). "
            "See docs/design/archive/smatchet-merge-watcher.md for the full design."
        ),
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("register", help="add a PR to the watcher registry")
    r.add_argument("pr", help="PR number (e.g. 361)")
    r.set_defaults(func=cmd_register)

    u = sub.add_parser("unregister", help="remove a PR from the watcher registry")
    u.add_argument("pr", help="PR number")
    u.set_defaults(func=cmd_unregister)

    s = sub.add_parser("status", help="show registry state (one PR or all)")
    s.add_argument("pr", nargs="?", help="optional PR number filter")
    s.set_defaults(func=cmd_status)

    l = sub.add_parser("list", help="dump full registry as JSON")
    l.set_defaults(func=cmd_list)

    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        return args.func(args)
    except (RuntimeError, TimeoutError) as exc:
        print(f"merge-watch: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
