#!/usr/bin/env python3
"""
merge_watcher_registry — the per-user merge-watcher registry primitives shared by
`merge-watcher-cli.py` (the register/unregister/status CLI) and `merge-watcher.py`
(the polling daemon).

Extracted from merge-watcher-cli.py (core-scripts-python-10): the daemon used to
pull these helpers in via `importlib.util.spec_from_file_location(... "merge-watcher-cli.py")`
+ `exec_module`, a fragile dynamic-import dance forced only because the sibling
filename carries hyphens and can't be `import`ed by name. This module has an
import-legal (underscore) name, so both callers `import` it normally.

The registry itself is a per-user JSON file at
`%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (Windows) or
`$XDG_STATE_HOME/smatchet/merge-watch/active.json` (POSIX) — OUTSIDE any git
clone so a `git clean -fx` can't nuke watch state — guarded by a cross-platform
sentinel-file lock. See docs/plans/shipped/smatchet-merge-watcher.md.
"""

from __future__ import annotations

import contextlib
import json
import os
import pathlib
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
