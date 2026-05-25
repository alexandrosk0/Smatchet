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
  MERGE_WATCH_POLL_INTERVAL         — seconds between per-PR polls (default 60).
  MERGE_GATES_*                     — inherited by merge-gates.sh per its own contract.
  ORCH_USER                         — required by merge-gates.sh.
  MERGE_WATCH_AUTO_ACT_ON_SANITIZER — opt-in (default false). When true, sanitizer
                                      CI failures trigger auto-act with debug-detective
                                      (skips coderabbit-triage).
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
import pathlib
import shutil
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
    # C4 prong 1 — flip draft → ready BEFORE the gates poll, not just before
    # the merge call. CodeRabbit's `auto_review.drafts: false` default means
    # PRs that stay draft skip CR review entirely; the merge-gates then
    # passes via the "NONE + StatusContext=SUCCESS" branch on CR's placeholder
    # status without ever seeing a real review. Documented as C4 in
    # `docs/evaluation/agentic-infrastructure-2026-05-23.md` and as P0 in
    # `docs/backlog/agent-self-improvement/process.md` (2026-05-21).
    #
    # Registering with the watcher = explicit authorization to flip-ready
    # (per `docs/agent-rules/merge-gates.md` § Auto-`gh pr ready` + merge).
    # The call is idempotent on already-non-draft PRs, so calling it every
    # poll has no semantic effect beyond one extra `gh` API hop per minute.
    #
    # Per CR feedback on PR #428: if the flip-ready step returns False, the PR
    # is still observably draft (or we couldn't confirm non-draft state). DO
    # NOT proceed with the gates poll — that would re-introduce the C4 bypass
    # this PR is fixing (gates pass via the placeholder StatusContext SUCCESS
    # branch on a draft PR). Return a transient state the daemon retries on
    # the next cycle; the underlying issue (auth failure, network blip, PR
    # genuinely refusing the flip) will surface on a subsequent attempt.
    if not ensure_pr_ready_for_review(owner, repo, pr):
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "READY_FLIP_FAILED",
            "last_status_line": (
                "ensure_pr_ready_for_review returned False — PR may still be draft; "
                "skipping gates poll this cycle to avoid C4 bypass path. "
                "Re-attempts on next poll. If persistent, surface to user via "
                "`gh pr view <N> --json isDraft` + manual `gh pr ready <N>`."
            ),
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
    a registered PR is to make sure it's non-draft. Without this, two failures:
    1. CodeRabbit's `.coderabbit.yaml` ships `auto_review.drafts: false`, so
       PRs that stay draft skip CR review entirely (C4 — see
       `docs/evaluation/agentic-infrastructure-2026-05-23.md` § C4).
    2. The eventual REST squash-merge returns HTTP 405 "Pull Request is still
       a draft" if the PR somehow remains draft at merge time.

    Per H2 (`scripts/dev/merge-gates.sh:gh_pr_ready_idempotent`): mirror the
    bash positive-check fallback so the Python version is equally robust to
    `gh` wording changes / locale variation. Returns True if the PR is
    observably non-draft after the call (whether via successful flip OR
    confirmed-already-non-draft).
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
    if result.returncode == 0:
        return True
    # Fast path — English phrase match (matches H2's bash fast path).
    err = (result.stderr or "") + (result.stdout or "")
    if "not in draft state" in err or "already marked ready" in err:
        return True
    # H2 positive-check fallback — probe observable state via `gh pr view`.
    # Robust against `gh` wording changes + locale variation + CLI drift.
    try:
        probe = subprocess.run(
            [GH_BIN, "pr", "view", str(pr), "--repo", f"{owner}/{repo}",
             "--json", "isDraft", "--jq", ".isDraft"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return False
    if probe.returncode == 0 and probe.stdout.strip() == "false":
        return True
    return False


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
    "READY_FLIP_FAILED",
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
    status_line = state.get("last_status_line", "(no status line)")
    pr = int(entry["pr"])
    # PR URL via gh from the clone path.
    pr_url = ""
    or_meta = _gh_owner_repo(entry["clone_path"])
    if or_meta:
        owner, repo = or_meta
        pr_url = f"https://github.com/{owner}/{repo}/pull/{pr}"
    # When the terminal state is a CR-finding block, append the inline-files
    # URL so the user lands on the diff view that shows CR's per-line markers
    # — saves a second click hunting for the comments from the overview tab.
    # Option C of the watcher-loop fix: faster, actionable notify.
    cr_finding = cur_state == "TRIAGE_BUDGET_EXHAUSTED" or _looks_like_cr_finding_block(status_line)
    if cr_finding and pr_url:
        message = f"{status_line[:160]} — review inline: {pr_url}/files"
    else:
        message = status_line[:200]
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
    MERGE_WATCH_TRIAGE_BUDGET (default 1), marks the state as
    TRIAGE_BUDGET_EXHAUSTED so Phase 4's notification surface picks it up.

    Default budget lowered from 3 → 1 (option C of the watcher-loop fix):
    triage retries don't fix code, they only re-classify. The loop's value
    is the user notification — get it on the next poll, not 3 polls later.

    Per-HEAD reset (P2 process self-improvement, 2026-05-23): a new push
    advances the PR's `headRefOid`, which means CR will re-review the new
    commit. The old triage_attempts counter is no longer informative for
    the new review, so reset to 0 when `triage_for_head_sha` differs from
    the current head_sha. This matches the auto-act path's `_atomic_reserve_auto_act`
    semantics (dedup per head_sha; new push = one fresh attempt available).
    Without the reset, the user hit TRIAGE_BUDGET_EXHAUSTED on push N and
    every subsequent push, because the counter persisted across HEAD moves.
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
    budget = int(os.environ.get("MERGE_WATCH_TRIAGE_BUDGET", "1"))
    # Fetch current head_sha to decide whether to reset the per-HEAD
    # counter. One extra `gh pr view` per BLOCKED-on-CR poll is cheap; the
    # auto-act path at handle_pass / maybe_auto_act already does the same.
    # Fail-open: if gh can't return a head_sha, fall back to the legacy
    # per-PR-lifetime counter rather than spuriously resetting.
    try:
        meta_json = _gh_json(
            ["pr", "view", str(pr), "--json", "headRefOid"],
            cwd=clone_path,
        )
        head_sha = meta_json.get("headRefOid", "")
    except RuntimeError:
        head_sha = ""
    prior_head_sha = entry.get("triage_for_head_sha", "")
    if head_sha and prior_head_sha and head_sha != prior_head_sha:
        # HEAD moved since last triage attempt → reset counter to 0.
        # This poll will count as attempt 1 after the bump below.
        attempts_before = 0
        extras["triage_reset_on_head_change"] = (
            f"{prior_head_sha[:8]} -> {head_sha[:8]}"
        )
    else:
        attempts_before = int(entry.get("triage_attempts", 0))
    attempts_after = attempts_before + 1
    # Bump the registry entry's triage_attempts (registry-locked). Persist
    # the head_sha so the next poll knows what HEAD this counter is for.
    _bump_triage_attempts(pr, clone_path, attempts_after, head_sha)
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


def _looks_like_sanitizer_failure(status_line: str) -> bool:
    """Detect whether the merge-gates Poll line indicates a sanitizer CI failure.

    Matches ASAN / UBSAN / TSAN failure signatures that appear in CI check
    names or status lines when the `sanitizer-asan-ubsan` or `sanitizer-tsan`
    jobs fail. The merge-gates poller surfaces the failing check name in the
    Poll line; we match on the job name pattern.
    """
    s = status_line.lower()
    return (
        "sanitizer" in s
        and ("fail" in s or "error" in s or "block" in s or "conclusion" in s)
    ) or (
        ("addresssanitizer" in s or "undefinedbehaviorsanitizer" in s or "threadsanitizer" in s)
    )


def _extract_failing_test_from_status(status_line: str) -> str:
    """Best-effort extraction of the failing test name from a sanitizer status line.

    The merge-gates poller emits check names like "Sanitizer (ASAN + UBSAN)" in
    its Poll output. If a specific test name appears (e.g. from ctest output
    forwarded into the status), extract it. Falls back to "(see CI log)" when
    the line doesn't contain a recognizable test name.
    """
    import re
    # Look for patterns like "test_name" or "TestSuite::TestCase" in the line.
    # CTest output often has "The following tests FAILED: N - test_name (...)".
    m = re.search(r'\d+\s*-\s*(\S+)', status_line)
    if m:
        return m.group(1)
    # Fallback: look for anything that looks like a test identifier.
    m = re.search(r'(?:test[_-]?\w+)', status_line, re.IGNORECASE)
    if m:
        return m.group(0)
    return "(see CI log)"


def _bump_triage_attempts(pr: int, clone_path: str, new_count: int, head_sha: str = "") -> None:
    """Update registry entry's triage_attempts (and triage_for_head_sha when given).

    `triage_for_head_sha` is the HEAD this counter is for. Per-HEAD reset
    in handle_blocked_cr_triage compares the current head_sha to this
    stored value to decide whether the counter is still meaningful.

    Backwards-compatible: callers passing only (pr, clone_path, new_count)
    leave triage_for_head_sha untouched (legacy per-PR-lifetime semantics).
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["triage_attempts"] = new_count
                if head_sha:
                    e["triage_for_head_sha"] = head_sha
                break
        _CLI.write_registry(entries)


def _bump_auto_act_state(pr: int, clone_path: str, head_sha: str, new_attempts: int) -> None:
    """Record an auto-act attempt against the (pr, clone_path) registry entry.

    `auto_act_attempts` is a per-PR-lifetime counter (NOT reset on head_sha
    change) so a runaway loop where Claude's fix produces fresh CR findings
    can't auto-retry forever. `auto_act_for_head_sha` deduplicates within a
    single head (one attempt per push).
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["auto_act_attempts"] = new_attempts
                e["auto_act_for_head_sha"] = head_sha
                e["auto_act_dispatched_at_unix"] = int(time.time())
                break
        _CLI.write_registry(entries)


def _atomic_reserve_auto_act(pr: int, clone_path: str, head_sha: str, budget: int):
    """Atomically check dedup + budget AND reserve a slot under registry_lock.

    Returns one of:
      ("ok", attempts_after)  — slot reserved; caller may spawn.
      ("dedup", None)         — already acted on this head_sha.
      ("budget", attempts)    — budget exhausted; returns prior attempts.

    Closing the race window the prior split-read-then-write left open:
    two daemons used to both observe attempts=N before either wrote
    attempts=N+1, dispatching N+1 spawns instead of one. The check +
    bump now happen inside a single `registry_lock()` critical section.
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) != pr or e.get("clone_path") != clone_path:
                continue
            if e.get("auto_act_for_head_sha") == head_sha:
                return ("dedup", None)
            attempts_before = int(e.get("auto_act_attempts", 0))
            attempts_after = attempts_before + 1
            if attempts_after > budget:
                return ("budget", attempts_before)
            e["auto_act_attempts"] = attempts_after
            e["auto_act_for_head_sha"] = head_sha
            e["auto_act_dispatched_at_unix"] = int(time.time())
            _CLI.write_registry(entries)
            return ("ok", attempts_after)
        # PR not in registry — shouldn't happen, but treat as dedup-block.
        return ("dedup", None)


# Single source of truth for the spawned Claude session's instructions.
# Deliberately spare — no project rules pasted in; the spawned session reads
# AGENTS.md + CLAUDE.md from the clone on its own. We only tell it WHAT to do
# (address PR #N's CodeRabbit findings) and HOW to get a checkout (gh pr
# checkout) so it doesn't waste tokens guessing the branch.
#
# C4 prong 3 (per docs/evaluation/agentic-infrastructure-2026-05-23.md): the
# spawned session is explicitly instructed to use the `coderabbit-triage`
# agent's classification framework (per `agents/coderabbit-triage.md`) — that
# agent specialises in CR-finding triage with hard invariant-rejection
# heuristics. The agent itself is `read-only:true` (classification only); the
# spawned session does the *application* step after the agent has classified.
# Result: a finding the triage agent rejects (e.g. suggesting C++17 features
# in a C++14-hard codebase) never gets applied, even if the spawned session
# would otherwise have been tempted to "just go with CR's suggestion."
AUTO_ACT_PROMPT = (
    "You are the auto-act helper spawned by smatchet-merge-watcher because "
    "PR #{pr} is BLOCKED on CodeRabbit findings.\n\n"
    "Address every actionable CodeRabbit comment on this PR. Steps:\n"
    "  1. `gh pr checkout {pr}` to switch this clone to the PR's branch.\n"
    "  2. **Invoke the `coderabbit-triage` agent first** (per "
    "`agents/coderabbit-triage.md`). Pass it PR #{pr}; it will fetch the "
    "inline CR comments via `gh api`, classify each by severity + target "
    "Smatchet subsystem, and reject suggestions that collide with the "
    "AGENTS.md invariants (C++14 hard, dual-target, UI-thread non-blocking, "
    "RAII, LOG_* logging, never-crash, etc.). The agent is read-only and "
    "emits a per-finding handoff packet — VALID findings (with target "
    "subsystem named) plus REJECT-INVARIANT / REJECT-AMBIGUOUS lists with "
    "rationale.\n"
    "  3. Apply the smallest possible fix per VALID finding. Use the "
    "subsystem agents the triage packet names (`tracker-backend`, "
    "`grid-engine`, `offline-sync`, `command-system`, `lua-binder`, "
    "`mcp-toolsmith`, etc.) for the actual edits when the finding is in their "
    "territory; otherwise edit directly. Skip every REJECT-INVARIANT finding "
    "outright; surface the rationale in the commit body.\n"
    "  4. Build / test only when the diff actually warrants it (docs-only "
    "diffs skip both per the pure-docs slice rule).\n"
    "  5. `git commit` with a message of the form "
    "`fix(merge-watcher auto-act): address CR findings on PR #{pr}` (body "
    "lists the triage classification: N VALID applied, M REJECT-INVARIANT "
    "with one-line reason each) and `git push`.\n\n"
    "Auto-act head_sha at dispatch was {head_sha}. If the PR's head has moved "
    "since (user pushed manually), STOP — say so and exit without committing.\n"
    "Auto-act is gated to {budget} attempts per PR lifetime; this is attempt "
    "{attempt}/{budget}."
)


AUTO_ACT_SANITIZER_PROMPT = (
    "You are the auto-act helper spawned by smatchet-merge-watcher because "
    "PR #{pr} has a FAILING sanitizer CI job.\n\n"
    "A sanitizer failure was detected. Invoke `debug-detective` directly to "
    "diagnose and fix the issue. Steps:\n"
    "  1. `gh pr checkout {pr}` to switch this clone to the PR's branch.\n"
    "  2. **Invoke `debug-detective`** (per `agents/debug-detective.md`) with:\n"
    "     - Failing test: {failing_test}\n"
    "     - CI job log URL: https://github.com/{owner}/{repo}/actions/runs/"
    "{{run_id}} (check the sanitizer job output)\n"
    "     - The sanitizer stderr constitutes the reproducer per slice 10's "
    "reproducer-first contract.\n"
    "  3. Do NOT invoke `coderabbit-triage` — this is a sanitizer failure, "
    "not a CR-finding block.\n"
    "  4. Apply the smallest fix that resolves the sanitizer error.\n"
    "  5. Build with `cmake --build --preset ninja-debug-msys2-asan` and run "
    "`ctest --output-on-failure` in the build dir to confirm the fix.\n"
    "  6. `git commit` with a message of the form "
    "`fix(sanitizer): resolve ASAN/UBSAN finding on PR #{pr}` and `git push`.\n\n"
    "Auto-act head_sha at dispatch was {head_sha}. If the PR's head has moved "
    "since (user pushed manually), STOP — say so and exit without committing.\n"
    "Auto-act is gated to {budget} attempts per PR lifetime; this is attempt "
    "{attempt}/{budget}."
)


def maybe_auto_act(state: dict[str, Any], entry: dict[str, Any]) -> dict[str, Any]:
    """Option-A loop closer — spawn `claude -p ...` in the background to
    address CR findings OR sanitizer failures when the watcher detects them.

    CR-finding path: Opt-in via `MERGE_WATCH_AUTO_ACT=true`.
    Sanitizer path:  Opt-in via `MERGE_WATCH_AUTO_ACT_ON_SANITIZER=true`
                     (separate knob — sanitizer auto-act is independent).

    Off by default — auto-spawning a Claude session against a checked-in
    clone has real cost (token spend) + runaway-loop risk. Safeguards:

      - Single attempt per (PR, head_sha) pair. Dedup key persisted on the
        registry entry's `auto_act_for_head_sha` field. A push to the PR
        (which advances head_sha) unlocks one more attempt — bounded by:
      - Per-PR-lifetime budget. `MERGE_WATCH_AUTO_ACT_BUDGET` (default 2).
        Once attempts >= budget the auto-act stops firing on this PR even
        on new pushes; the user `merge-watch unregister`s to take back.
      - Refuses if `claude` is not on PATH (no silent no-op).
      - Refuses if the clone has uncommitted tracked-modified files (a
        concurrent agent / user may be editing).
      - Detached subprocess so the daemon's poll loop is NOT blocked by
        Claude's session runtime; output captured to a per-(PR,sha) log
        file under the state dir for after-the-fact inspection.

    When the trigger is a sanitizer failure (not CR-finding), the spawned
    session invokes `debug-detective` directly — skips `coderabbit-triage`.

    Returns extras dict to merge into the state.
    """
    cur_state = state.get("last_state", "")
    status_line = state.get("last_status_line", "")
    # Determine which trigger path applies.
    is_cr_trigger = (
        cur_state == "TRIAGE_BUDGET_EXHAUSTED"
        or _looks_like_cr_finding_block(status_line)
    )
    is_sanitizer_trigger = _looks_like_sanitizer_failure(status_line)
    # Gate on the appropriate env knob.
    auto_act_enabled = os.environ.get(
        "MERGE_WATCH_AUTO_ACT", ""
    ).strip().lower() in {"true", "1", "yes"}
    sanitizer_auto_act_enabled = os.environ.get(
        "MERGE_WATCH_AUTO_ACT_ON_SANITIZER", ""
    ).strip().lower() in {"true", "1", "yes"}
    if is_sanitizer_trigger and sanitizer_auto_act_enabled:
        trigger_kind = "sanitizer"
    elif is_cr_trigger and auto_act_enabled:
        trigger_kind = "cr"
    else:
        return {}
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    or_meta = _gh_owner_repo(clone_path)
    if not or_meta:
        return {"auto_act_action": "skipped: gh repo view failed"}
    owner, repo = or_meta
    # Fetch head_sha — we don't trust last_status_line for this since the
    # gates poller redacts it. One extra gh call per auto-act candidate is
    # cheap; gating happens upstream so we only land here on real CR blocks.
    try:
        meta_json = _gh_json(
            ["pr", "view", str(pr), "--json", "headRefOid"],
            cwd=clone_path,
        )
        head_sha = meta_json.get("headRefOid", "")
    except RuntimeError as exc:
        return {"auto_act_action": f"skipped: gh pr view failed: {exc}"}
    if not head_sha:
        return {"auto_act_action": "skipped: headRefOid empty"}
    # Per-PR-lifetime budget — fail-open on malformed env values so a typo
    # in MERGE_WATCH_AUTO_ACT_BUDGET doesn't kill the daemon loop.
    budget_raw = os.environ.get("MERGE_WATCH_AUTO_ACT_BUDGET", "2")
    try:
        budget = int(budget_raw)
    except ValueError:
        budget = 2
    # claude binary on PATH?
    claude_bin = shutil.which("claude")
    if not claude_bin:
        return {"auto_act_action": "skipped: claude binary not on PATH"}
    # Refuse if the clone has uncommitted work — concurrent edits would race.
    try:
        st = subprocess.run(
            ["git", "-C", clone_path, "status", "--porcelain"],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=10,
        )
        if st.returncode == 0 and st.stdout.strip():
            return {
                "auto_act_action": (
                    "skipped: clone has uncommitted changes "
                    f"({len(st.stdout.splitlines())} file(s))"
                ),
            }
    except (FileNotFoundError, subprocess.TimeoutExpired) as exc:
        return {"auto_act_action": f"skipped: git status check failed: {exc}"}
    # Atomically check dedup + budget AND reserve a slot under a single
    # `registry_lock()` critical section. Closes a race where two daemons
    # could both observe attempts=N before either wrote attempts=N+1.
    reserved, payload = _atomic_reserve_auto_act(pr, clone_path, head_sha, budget)
    if reserved == "dedup":
        return {"auto_act_action": "suppressed (already acted on this head_sha)"}
    if reserved == "budget":
        return {
            "auto_act_action": f"BUDGET_EXHAUSTED ({payload}/{budget})",
        }
    attempts_after = payload  # int — the reserved slot index
    # Spawn detached.
    log_path = state_dir() / f"{pr}-auto-act-{head_sha[:8]}.log"
    # Select the appropriate prompt template based on trigger kind.
    if trigger_kind == "sanitizer":
        # Extract a rough failing-test name from the status line. The
        # merge-gates poller includes the check name; best-effort parse.
        failing_test = _extract_failing_test_from_status(status_line)
        prompt = AUTO_ACT_SANITIZER_PROMPT.format(
            pr=pr, owner=owner, repo=repo,
            head_sha=head_sha[:12], budget=budget, attempt=attempts_after,
            failing_test=failing_test,
        )
    else:
        prompt = AUTO_ACT_PROMPT.format(
            pr=pr, owner=owner, repo=repo,
            head_sha=head_sha[:12], budget=budget, attempt=attempts_after,
        )
    try:
        # Context-manage the parent's log handle. The child inherits its
        # own copy via the OS dup-on-fork/spawn; the parent's close-after-
        # Popen does not affect the child's stream. Prevents fd leaks
        # across many auto-act cycles in long-running daemons.
        with open(log_path, "w", encoding="utf-8") as logf:
            popen_kwargs: dict[str, Any] = {
                "stdout": logf, "stderr": subprocess.STDOUT,
                "cwd": clone_path,
            }
            if sys.platform == "win32":
                popen_kwargs["creationflags"] = (
                    subprocess.CREATE_NEW_PROCESS_GROUP  # type: ignore[attr-defined]
                    | 0x00000008  # DETACHED_PROCESS — daemon outlives this poll cycle.
                )
            else:
                popen_kwargs["start_new_session"] = True
            proc = subprocess.Popen(
                [claude_bin, "-p", prompt],
                **popen_kwargs,
            )
        return {
            "auto_act_action": (
                f"dispatched (background pid={proc.pid}, "
                f"attempt {attempts_after}/{budget})"
            ),
            "auto_act_for_head_sha": head_sha,
            "auto_act_attempts": attempts_after,
            "auto_act_dispatched_at_unix": int(time.time()),
            "auto_act_log": str(log_path),
        }
    except (OSError, ValueError) as exc:
        # Slot was already reserved — spawn failure consumes one budget point.
        # Documented behavior: failed auto-acts count against the per-PR budget
        # so a wedge'd claude binary can't burn unbounded attempts.
        return {"auto_act_action": f"spawn failed: {exc}"}


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
                    # Option A (opt-in) — spawn `claude -p` in the background
                    # to address CR findings. Gated by MERGE_WATCH_AUTO_ACT.
                    auto_act_extras = maybe_auto_act(state, entry)
                    if auto_act_extras:
                        state.update(auto_act_extras)
                        if "auto_act_action" in auto_act_extras:
                            print(
                                f"    auto-act: {auto_act_extras.get('auto_act_action', '?')}"
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
