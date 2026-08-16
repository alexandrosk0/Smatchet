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


# ---------------------------------------------------------------------------
# Merge authorization (registration != consent)
# ---------------------------------------------------------------------------
def entry_is_authorized(entry: dict[str, Any]) -> bool:
    """True when the USER explicitly authorized this entry for auto-merge.

    Registration and authorization are SEPARATE (2026-08-16 P1
    `watcher-autoregister-bypasses-merge-consent`): the `gh pr create`
    PostToolUse hook registers every agent-opened PR so it gets gate-polling +
    stuck-nudges, and that bookkeeping must NOT imply merge rights. Only an
    explicit user act — post-ship option 3 / in-session "merge when green",
    which run `merge-watch register --authorized` or `merge-watch authorize` —
    writes `authorized: true`.

    A MISSING key reads as unauthorized: entries registered before this key
    existed park at DRAFT_UNAUTHORIZED / "PASS but skipped: not authorized"
    until a human runs `merge-watch authorize <pr>`. That is the safe default —
    the failure mode is a PR that waits, not a PR that merges unasked.
    """
    return bool(entry.get("authorized", False))


def set_entry_authorized(pr: int, clone_path: str, authorized: bool) -> str:
    """Flip one entry's `authorized` flag under the registry lock.

    Returns `"changed"`, `"unchanged"` (already at that value) or `"missing"`
    (no entry for this `(pr, clone_path)` pair — the identity every registry
    verb keys on).
    """
    with registry_lock():
        entries = read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                if entry_is_authorized(e) == authorized:
                    return "unchanged"
                e["authorized"] = authorized
                write_registry(entries)
                return "changed"
    return "missing"


# ---------------------------------------------------------------------------
# Agent-event vocabulary (shared: daemon emits, CLI await filters)
# ---------------------------------------------------------------------------
#: Terminal/actionable states an agent wants to wake on. Superset of the
#: daemon's NOTIFY_STATES plus GATES_PASSED (merged) so an
#: `await --until=terminal` returns on a merge too. Lives HERE, not in
#: merge-watcher.py, because merge-watcher-cli.py's `await` filters MUST use
#: the same vocabulary the daemon emits: the CLI once carried a hand-copied
#: subset that silently dropped ACTIONS_UNAVAILABLE (exit 7) and
#: REQUIRED_MISSING_CANCELLED (exit 8) — `await` then waited forever on
#: exactly the states that most need an agent to act (CodeRabbit finding on
#: PR #2012). Same extraction rationale as the registry primitives above
#: (core-scripts-python-10).
AGENT_EVENT_STATES = {
    "GATES_PASSED",
    "TRIAGE_BUDGET_EXHAUSTED",
    "STUCK_NEEDS_ATTENTION",
    "CI_FAIL",
    "GH_API_DOWN",
    "PR_CLOSED_OR_MERGED",
    "PAGINATION_OVERFLOW",
    "TIMEOUT",
    "READY_FLIP_FAILED",
    "ACTIONS_UNAVAILABLE",
    "REQUIRED_MISSING_CANCELLED",
    # Draft PR on an unauthorized entry: the watcher refuses to flip it ready
    # (that write needs consent) and refuses to poll a draft PR's gates (C4).
    # Nothing clears it but a human, so `await` MUST return on it rather than
    # block forever — the same failure the exit-7/exit-8 additions above fixed.
    "DRAFT_UNAUTHORIZED",
}

#: The two end states: the PR needs no further agent action.
AGENT_EVENT_END_STATES = frozenset({"GATES_PASSED", "PR_CLOSED_OR_MERGED"})
