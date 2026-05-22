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

# Defensive: force stdout / stderr to UTF-8 so any future unicode in print()
# doesn't crash the daemon on Windows where the default codec is cp1252.
# Cycle 861 (2026-05-22) crashed because of `→` in a status line; the
# arrow has been ASCII-folded but other unicode might creep into BLOCKED
# status lines (CR review bodies, branch names with non-ASCII chars, etc.).
# Python 3.7+ supports reconfigure(); older versions silently no-op via try.
with contextlib.suppress(AttributeError, ValueError):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def _resolve_bin(name: str, *extra_candidates: str) -> str:
    """Locate a CLI on the daemon's env. Scheduled-task env on Windows
    usually doesn't include the GitHub CLI / WinGet-installed tools on
    PATH, so bare `subprocess.run(["jq", ...])` fails with FileNotFoundError.
    Probe standard install locations + PATH; cache once per daemon."""
    import shutil
    via_path = shutil.which(name) or shutil.which(name + ".exe")
    if via_path:
        return via_path
    for candidate in extra_candidates:
        if candidate and os.path.exists(candidate):
            return candidate
    return name  # last-resort — subprocess.run will FileNotFoundError loudly


GH_BIN = _resolve_bin(
    "gh",
    r"C:\Program Files\GitHub CLI\gh.exe",
    r"C:\Program Files (x86)\GitHub CLI\gh.exe",
    os.path.expandvars(r"%LOCALAPPDATA%\Programs\GitHub CLI\gh.exe"),
)
# `merge-gates.sh` shells out to `jq` from bash; winget puts jq under
# %LOCALAPPDATA%\Microsoft\WinGet\Links which is rarely on Scheduled-Task
# PATH. Resolve so we can prepend its dir + the GitHub CLI dir to the
# bash subprocess's PATH together.
JQ_BIN = _resolve_bin(
    "jq",
    os.path.expandvars(r"%LOCALAPPDATA%\Microsoft\WinGet\Links\jq.exe"),
    r"C:\msys64\ucrt64\bin\jq.exe",
    r"C:\Program Files\Git\usr\bin\jq.exe",
)
# On Windows + WSL, bare `bash` on PATH usually resolves to WSL's bash via
# %SystemRoot%\System32\bash.exe — which can't run `merge-gates.sh` (WSL
# has its own /bin/bash that may be misconfigured or missing). Force Git
# for Windows' bash.exe by full path. Cycle 862 (after the gh+jq fix)
# crashed with `execvpe(/bin/bash) failed: No such file or directory`
# because WSL was first on PATH.
BASH_BIN = _resolve_bin(
    "bash",
    r"C:\Program Files\Git\bin\bash.exe",
    r"C:\Program Files (x86)\Git\bin\bash.exe",
    r"C:\msys64\usr\bin\bash.exe",
)
# If shutil.which picked up WSL bash, override with the Git Bash path even
# though the resolution looked successful — WSL bash isn't usable here.
if BASH_BIN.lower().endswith(r"system32\bash.exe"):
    for candidate in (
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
        r"C:\msys64\usr\bin\bash.exe",
    ):
        if os.path.exists(candidate):
            BASH_BIN = candidate
            break

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
_orch_user_cache: str | None = None


def _resolve_orch_user(clone_path: str) -> str:
    """Resolve ORCH_USER via `gh api user --jq .login`. Cached for the daemon
    lifetime — `gh` auth identity doesn't change mid-session, and we don't
    want one extra `gh api` call per PR per poll cycle.

    Returns empty string on failure (merge-gates.sh will then fail loudly
    with "ORCH_USER not set" rather than silently mis-attributing comments).
    """
    global _orch_user_cache
    if _orch_user_cache is not None:
        return _orch_user_cache
    try:
        r = subprocess.run(
            [GH_BIN, "api", "user", "--jq", ".login"],
            cwd=clone_path,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
        if r.returncode == 0 and r.stdout.strip():
            _orch_user_cache = r.stdout.strip()
            return _orch_user_cache
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass
    _orch_user_cache = ""
    return ""


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
            [GH_BIN, "repo", "view", "--json", "owner,name"],
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
    # ORCH_USER is required by merge-gates.sh but the Scheduled Task spawns
    # python with an inherited env that often lacks it (user-scope env vars
    # only flow through interactive shells). Resolve once and cache.
    env.setdefault("ORCH_USER", _resolve_orch_user(clone_path))
    # merge-gates.sh shells out to `gh` AND `jq` from bash; the daemon's
    # PATH may not include either tool's install dir (Scheduled-Task env
    # is minimal). Prepend both resolved binaries' dirs so the bash
    # subprocess finds them via bare-name invocation.
    extra_path_parts = []
    for bin_path in (GH_BIN, JQ_BIN):
        d = os.path.dirname(bin_path) if bin_path else ""
        if d and d not in extra_path_parts:
            extra_path_parts.append(d)
    if extra_path_parts:
        env["PATH"] = os.pathsep.join(extra_path_parts) + os.pathsep + env.get("PATH", "")
    try:
        gates = subprocess.run(
            [BASH_BIN, str(MERGE_GATES_SCRIPT), owner, repo, str(pr)],
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
    if status_lines:
        last_line = status_lines[-1]
    elif gates.stdout.strip():
        last_line = gates.stdout.strip().splitlines()[-1]
    elif gates.stderr.strip():
        # Surface stderr when stdout is empty — otherwise empty status_line
        # is a debugging black hole. Trim aggressively so the JSON state file
        # stays readable.
        err = " | ".join(ln.strip() for ln in gates.stderr.splitlines() if ln.strip())
        last_line = f"STDERR: {err[:300]}"
    else:
        last_line = ""
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
    # Windows occasionally races on the temp-file write — concurrent CLI reads
    # (`merge-watcher-cli.py status`) lock the inode, anti-virus scanners
    # hold the just-created .tmp for an instant, etc. A transient EACCES /
    # EBUSY would crash the entire daemon (no per-cycle exception handler in
    # the poll loop). Retry-with-backoff makes the write resilient to the
    # ~100 ms window where another reader is holding the file open.
    state_dir().mkdir(parents=True, exist_ok=True)
    p = state_dir() / f"{int(state['pr'])}.json"
    tmp = p.with_suffix(p.suffix + ".tmp")
    payload = json.dumps(state, indent=2, sort_keys=True) + "\n"
    last_exc: Exception | None = None
    for delay in (0.0, 0.05, 0.15, 0.5, 1.0):
        if delay:
            time.sleep(delay)
        try:
            tmp.write_text(payload, encoding="utf-8")
            tmp.replace(p)
            return
        except (PermissionError, OSError) as exc:
            last_exc = exc
            continue
    # All retries exhausted — surface to the poll loop so the daemon can log
    # WARN + skip this cycle's state write instead of crashing the process.
    assert last_exc is not None
    raise last_exc


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
        [GH_BIN, *args],
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


def ensure_pr_ready_for_review(owner: str, repo: str, pr: int) -> bool:
    """Idempotent flip-draft-to-ready via `gh pr ready <pr>`.

    Per AGENTS.md § Post-ship turn-end protocol: the watcher's first step on
    a `GATES_PASSED` PR is to make sure it's non-draft. Without this, the
    subsequent REST squash-merge call returns HTTP 405 "Pull Request is
    still a draft" and the PR stays stuck forever in `GATES_PASSED but
    merge_failed` state.

    `gh pr ready` is idempotent — it's a no-op on a PR that's already
    non-draft, so we don't need to gate on a prior `isDraft` check.

    Returns True on success (PR is now non-draft), False on failure.
    Failure here does NOT abort the merge attempt — the caller's
    `squash_merge_pr` will surface the underlying problem if the PR
    really is still a draft, and the registry entry stays for the next
    poll cycle.
    """
    args = [GH_BIN, "pr", "ready", str(pr), "--repo", f"{owner}/{repo}"]
    try:
        result = subprocess.run(
            args,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    # gh exits 0 on success; also exits 0 with "already marked as ready" stderr
    # if the PR was already non-draft (verified manually 2026-05-22).
    return result.returncode == 0


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
            GH_BIN,
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


TRIAGE_SCRIPT = _HERE / "coderabbit-triage.py"
NOTIFY_SCRIPT = _HERE / "smatchet-notify.sh"

# Terminal states that fire Phase 4a notification (smatchet-notify.sh →
# Smatchet in-app toast attempt + Windows native BurntToast fallback).
# Other states (GATES_PASSED handled by handle_pass; BLOCKED-on-CR handled
# by handle_blocked_cr_triage) don't notify directly — they're either acted
# on in-loop or wait for the next cycle.
NOTIFY_STATES = {
    "CI_FAIL",
    "GH_API_DOWN",
    "PR_CLOSED_OR_MERGED",
    "PAGINATION_OVERFLOW",
    "TIMEOUT",
    "TRIAGE_BUDGET_EXHAUSTED",
}


def maybe_notify(state: dict[str, Any], entry: dict[str, Any]) -> dict[str, Any]:
    """Phase 4a — fire smatchet-notify.sh for terminal states; suppress
    re-notify within the same state by comparing against the previous
    state's `notify_dispatched_for_state` field.

    Returns extras dict to merge into the state.
    """
    cur_state = state.get("last_state", "")
    if cur_state not in NOTIFY_STATES:
        return {}
    # Suppression — read prior state file to check last notified state.
    prior_state_file = state_dir() / f"{int(entry['pr'])}.json"
    prior_notified_for = None
    if prior_state_file.exists():
        try:
            prior = json.loads(prior_state_file.read_text(encoding="utf-8"))
            prior_notified_for = prior.get("notify_dispatched_for_state")
        except json.JSONDecodeError:
            pass
    if prior_notified_for == cur_state:
        return {"notify_action": "suppressed (same state as last notify)"}
    # Compose message + invoke notify script.
    message = state.get("last_status_line", "(no status line)")[:200]
    pr = int(entry["pr"])
    # PR URL via gh from the clone path.
    pr_url = ""
    or_meta = _gh_owner_repo(entry["clone_path"])
    if or_meta:
        owner, repo = or_meta
        pr_url = f"https://github.com/{owner}/{repo}/pull/{pr}"
    args = [
        BASH_BIN,
        str(NOTIFY_SCRIPT),
        "--pr",
        str(pr),
        "--state",
        cur_state,
        "--message",
        message,
    ]
    if pr_url:
        args += ["--pr-url", pr_url]
    # Same env discipline as the merge-gates.sh subprocess (see poll_one):
    # ensure gh/jq dirs are on PATH in case smatchet-notify.sh shells out to
    # either; force utf-8 + errors=replace on stdout decode so the daemon's
    # state-file write doesn't crash on a `→` in CR review text we surface
    # as the message. Pre-#393, this path ran with WSL bash (System32) which
    # printed `<3>WSL (12 - Relay) ERROR: execvpe(/bin/bash) failed` per poll
    # cycle and broke every Phase 4a notification.
    notify_env = os.environ.copy()
    extra_path_parts = []
    for bin_path in (GH_BIN, JQ_BIN):
        d = os.path.dirname(bin_path) if bin_path else ""
        if d and d not in extra_path_parts:
            extra_path_parts.append(d)
    if extra_path_parts:
        notify_env["PATH"] = os.pathsep.join(extra_path_parts) + os.pathsep + notify_env.get("PATH", "")
    try:
        result = subprocess.run(
            args, env=notify_env, capture_output=True, text=True,
            encoding="utf-8", errors="replace", timeout=30,
        )
        if result.returncode == 0:
            return {
                "notify_action": "dispatched",
                "notify_dispatched_for_state": cur_state,
                "notify_dispatched_at_unix": int(time.time()),
            }
        return {
            "notify_action": f"failed (exit {result.returncode}): {result.stderr.strip()[:120]}",
            "notify_dispatched_for_state": cur_state,
        }
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"notify_action": f"exec failed: {exc}"}


def handle_blocked_cr_triage(entry: dict[str, Any], status_line: str) -> dict[str, Any]:
    """Phase 3 — when merge-gates BLOCKED on CR findings, invoke the triage
    classifier + post a structured comment on the PR.

    Triggers only when the status line indicates a CR-finding block (matches
    'actionable' or 'COMMENTED' with > 0 findings). Other BLOCKED reasons
    (CI fail, user comment, missing review) don't fire triage.

    Increments `triage_attempts` on the registry entry. When attempts >=
    MERGE_WATCH_TRIAGE_BUDGET (default 3), marks the state as
    TRIAGE_BUDGET_EXHAUSTED so Phase 4's notification surface picks it up.
    """
    extras: dict[str, Any] = {}
    if not _looks_like_cr_finding_block(status_line):
        extras["triage_action"] = "skipped: BLOCKED but not CR-finding"
        return extras
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    or_meta = _gh_owner_repo(clone_path)
    if not or_meta:
        extras["triage_action"] = "skipped: gh repo view failed"
        return extras
    owner, repo = or_meta
    budget = int(os.environ.get("MERGE_WATCH_TRIAGE_BUDGET", "3"))
    attempts_before = int(entry.get("triage_attempts", 0))
    attempts_after = attempts_before + 1
    # Bump the registry entry's triage_attempts (registry-locked).
    _bump_triage_attempts(pr, clone_path, attempts_after)
    extras["triage_attempts"] = attempts_after
    extras["triage_budget"] = budget
    if attempts_after > budget:
        extras["triage_action"] = "BUDGET_EXHAUSTED"
        extras["last_state"] = "TRIAGE_BUDGET_EXHAUSTED"  # overrides BLOCKED for notify
        return extras
    # Invoke the classifier in post-comment mode.
    try:
        result = subprocess.run(
            [
                sys.executable,
                str(TRIAGE_SCRIPT),
                "post-comment",
                owner,
                repo,
                str(pr),
                "--attempt",
                str(attempts_after),
                "--budget",
                str(budget),
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
        if result.returncode == 0:
            extras["triage_action"] = f"posted (attempt {attempts_after}/{budget})"
        else:
            extras["triage_action"] = (
                f"classifier failed (exit {result.returncode}): "
                f"{result.stderr.strip()[:160]}"
            )
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        extras["triage_action"] = f"classifier exec failed: {exc}"
    return extras


def _looks_like_cr_finding_block(status_line: str) -> bool:
    """Detect whether the merge-gates Poll line indicates a CR-finding block.

    Matches the new STALE_WITH_FINDINGS / COMMENTED (N actionable — block)
    shapes from the post-#360 poller. Excludes pure CI-fail / user-comment /
    NONE-grace-expired blocks — those go straight to Phase 4 notification
    without triage.
    """
    s = status_line.lower()
    return (
        "actionable" in s
        and ("block" in s or "stale_with_findings" in s)
    ) or "changes_requested" in s


def _bump_triage_attempts(pr: int, clone_path: str, new_count: int) -> None:
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["triage_attempts"] = new_count
                break
        _CLI.write_registry(entries)


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
    # 1. Flip draft → ready-for-review (idempotent). Required per AGENTS.md
    #    § Post-ship turn-end protocol; without this, draft PRs that pass
    #    gates fail the squash-merge with HTTP 405 and stay stuck.
    ensure_pr_ready_for_review(owner, repo, pr)
    # 2. Detect the head branch (for cascade after merge).
    head_branch = detect_merged_branch_name(owner, repo, pr)
    # 3. Squash-merge.
    try:
        merge_sha = squash_merge_pr(owner, repo, pr)
    except RuntimeError as exc:
        extras["merge_action"] = f"merge_failed: {exc}"
        return extras
    extras["merge_action"] = "merged"
    extras["merge_sha"] = merge_sha
    extras["merged_branch"] = head_branch
    # 4. Drop from registry.
    maybe_remove_from_registry(pr, clone_path)
    # 5. Cascade — find stacked children + trigger update-branch on each.
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
                    # Phase 3 — BLOCKED on CR findings → triage classifier + comment.
                    elif state.get("last_state") == "BLOCKED":
                        extras = handle_blocked_cr_triage(entry, state.get("last_status_line", ""))
                        state.update(extras)
                        if extras.get("triage_action"):
                            print(
                                f"  PR#{state['pr']:<6} BLOCKED -> triage: {extras.get('triage_action')}"
                            )
                        else:
                            print(
                                f"  PR#{state['pr']:<6} BLOCKED (no triage) "
                                f"poll_line={state.get('last_status_line', '')[:100]}"
                            )
                    else:
                        print(
                            f"  PR#{state['pr']:<6} state={state['last_state']:<24} "
                            f"poll_line={state.get('last_status_line', '')[:120]}"
                        )
                    # Phase 4a — fire smatchet-notify on terminal states.
                    notify_extras = maybe_notify(state, entry)
                    if notify_extras:
                        state.update(notify_extras)
                        if "notify_action" in notify_extras and notify_extras["notify_action"] != "suppressed (same state as last notify)":
                            print(
                                f"    notify: {notify_extras.get('notify_action', '?')}"
                            )
                    try:
                        write_state(state)
                    except OSError as _write_err:
                        print(
                            f"  WARN: write_state PR#{state['pr']} failed after retries: {_write_err}; "
                            "skipping this cycle's state flush — daemon continues"
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
    # Force UTF-8 on stdout / stderr so any future non-ASCII glyph in a print
    # call doesn't crash the daemon under Windows' default cp1252 codepage.
    # `reconfigure` lands in 3.7+. Fail open — the daemon must keep running
    # even if reconfigure raises on an exotic stdout wrapper.
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(encoding="utf-8", errors="replace")  # type: ignore[attr-defined]
        except Exception:
            pass
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
