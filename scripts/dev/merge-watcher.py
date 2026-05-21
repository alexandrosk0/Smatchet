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
import contextlib
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
# Phase 2 — PASS-branch auto-merge + cascade detection
# ---------------------------------------------------------------------------
def cascade_locks_dir() -> pathlib.Path:
    return watcher_root() / "locks"


@contextlib.contextmanager
def cascade_lock(branch: str, timeout_seconds: float = 30.0):
    """Per-branch lock to serialize cascade-into-stacked-child.

    Two parent PRs merging near-simultaneously could both try to pull develop
    into the same child branch + push, racing each other. The lock guarantees
    one-at-a-time per branch. Same sentinel-file pattern as registry_lock,
    but a separate dir + per-branch filename.
    """
    cascade_locks_dir().mkdir(parents=True, exist_ok=True)
    # Sanitize branch name for filename (replace / with __).
    safe = branch.replace("/", "__").replace("\\", "__")
    lock = cascade_locks_dir() / f"cascade-{safe}.lock"
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
                    f"merge-watcher: cascade lock {lock} held > {timeout_seconds}s"
                )
            time.sleep(0.2)
    try:
        yield
    finally:
        try:
            os.unlink(str(lock))
        except FileNotFoundError:
            pass


def _gh_json(args: list[str], cwd: str | None = None, timeout: int = 30) -> dict | list:
    """Run a `gh` subcommand expecting JSON on stdout; return parsed."""
    result = subprocess.run(
        ["gh", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"gh {' '.join(args[:2])} exited {result.returncode}: {result.stderr.strip()[:200]}"
        )
    try:
        return json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"gh {' '.join(args[:2])} produced non-JSON: {exc}") from exc


def squash_merge_pr(owner: str, repo: str, pr: int) -> str:
    """Squash-merge a PR via REST. Returns the merge commit SHA.

    Per AGENTS.md § Merge gates: REST merge call is what GitHub enforces;
    no client-side branch-protection / conflict duplication.
    """
    merge = _gh_json(
        [
            "api",
            "-X",
            "PUT",
            f"repos/{owner}/{repo}/pulls/{pr}/merge",
            "-f",
            "merge_method=squash",
        ]
    )
    if not merge.get("merged"):
        raise RuntimeError(f"squash-merge of PR #{pr} returned merged=false: {merge}")
    return merge.get("sha", "")


def detect_merged_branch_name(owner: str, repo: str, pr: int) -> str | None:
    """After merge, fetch the branch the PR was merging FROM (its head ref)
    so we can search for stacked children that have base=<this-branch>.
    """
    try:
        pr_meta = _gh_json(["api", f"repos/{owner}/{repo}/pulls/{pr}"])
    except RuntimeError:
        return None
    head = pr_meta.get("head") or {}
    ref = head.get("ref")
    return ref if isinstance(ref, str) else None


def find_stacked_children(owner: str, repo: str, base_branch: str) -> list[dict]:
    """gh pr list --base <branch> --state open → list of {number, headRefName}."""
    try:
        prs = _gh_json(
            [
                "pr",
                "list",
                "--repo",
                f"{owner}/{repo}",
                "--base",
                base_branch,
                "--state",
                "open",
                "--json",
                "number,headRefName,title",
                "--limit",
                "50",
            ]
        )
    except RuntimeError:
        return []
    return prs if isinstance(prs, list) else []


def cascade_update_child(owner: str, repo: str, child_pr: int) -> tuple[bool, str]:
    """Trigger GitHub's PR update-branch — merges base (e.g. develop, freshly
    advanced by the parent merge) into the child PR's head. This is the
    canonical "pull develop into my PR" API path; no local checkout needed,
    no force-push (it creates a merge commit on the child's branch).

    Returns (ok, message). Skips force-push (banned on feat/* / chore/* / etc.
    per AGENTS.md § Project rules § Force-push carve-out — only claude/<id>
    qualifies).
    """
    result = subprocess.run(
        [
            "gh",
            "api",
            "-X",
            "PUT",
            f"repos/{owner}/{repo}/pulls/{child_pr}/update-branch",
        ],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30,
    )
    if result.returncode != 0:
        # Common case: 422 if branch is already up-to-date — treat as OK.
        if "already up to date" in result.stderr.lower() or "up-to-date" in result.stderr.lower():
            return True, "child already up-to-date"
        return False, f"update-branch failed (exit {result.returncode}): {result.stderr.strip()[:200]}"
    return True, "update-branch dispatched"


def maybe_remove_from_registry(pr: int, clone_path: str) -> None:
    """After a successful merge, drop the PR from active.json so the daemon
    stops polling it. Uses the same file-lock as the CLI.
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        before = len(entries)
        entries = [
            e
            for e in entries
            if not (int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path)
        ]
        if len(entries) != before:
            _CLI.write_registry(entries)


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


def _gh_owner_repo(clone_path: str) -> tuple[str, str] | None:
    """Resolve owner + repo from a clone path via `gh repo view --json`."""
    try:
        meta = _gh_json(["repo", "view", "--json", "owner,name"], cwd=clone_path)
        return meta["owner"]["login"], meta["name"]
    except (RuntimeError, KeyError):
        return None


def handle_pass(entry: dict[str, Any]) -> dict[str, Any]:
    """PASS-branch handler — squash-merge + cascade to stacked children.

    Returns an additional state dict with `merge_action_*` fields appended.
    Removes the PR from the registry on successful merge.
    """
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    extras: dict[str, Any] = {}
    or_meta = _gh_owner_repo(clone_path)
    if not or_meta:
        extras["merge_action"] = "skipped: gh repo view failed"
        return extras
    owner, repo = or_meta
    # 1. Detect the head branch (for cascade after merge).
    head_branch = detect_merged_branch_name(owner, repo, pr)
    # 2. Squash-merge.
    try:
        merge_sha = squash_merge_pr(owner, repo, pr)
    except RuntimeError as exc:
        extras["merge_action"] = f"merge_failed: {exc}"
        return extras
    extras["merge_action"] = "merged"
    extras["merge_sha"] = merge_sha
    extras["merged_branch"] = head_branch
    # 3. Drop from registry.
    maybe_remove_from_registry(pr, clone_path)
    # 4. Cascade — find stacked children + trigger update-branch on each.
    children_results = []
    if head_branch:
        children = find_stacked_children(owner, repo, head_branch)
        for child in children:
            child_pr = int(child["number"])
            child_head = child.get("headRefName", "?")
            try:
                with cascade_lock(child_head):
                    ok, msg = cascade_update_child(owner, repo, child_pr)
            except TimeoutError as exc:
                ok, msg = False, str(exc)
            children_results.append(
                {"pr": child_pr, "head": child_head, "ok": ok, "msg": msg}
            )
    extras["cascade_children"] = children_results
    return extras


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
                    # Phase 2 — PASS-branch auto-merge + cascade.
                    if state.get("last_state") == "GATES_PASSED":
                        extras = handle_pass(entry)
                        state.update(extras)
                        if extras.get("merge_action") == "merged":
                            print(
                                f"  PR#{state['pr']:<6} MERGED sha={extras.get('merge_sha','?')[:10]} "
                                f"cascade_children={len(extras.get('cascade_children', []))}"
                            )
                            for child in extras.get("cascade_children", []):
                                tag = "OK" if child["ok"] else "ERR"
                                print(f"    cascade #{child['pr']:<5} {tag}: {child['msg'][:80]}")
                        else:
                            print(
                                f"  PR#{state['pr']:<6} PASS but {extras.get('merge_action', '?')}"
                            )
                    else:
                        print(
                            f"  PR#{state['pr']:<6} state={state['last_state']:<24} "
                            f"poll_line={state.get('last_status_line', '')[:120]}"
                        )
                    write_state(state)
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
