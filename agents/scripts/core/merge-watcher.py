#!/usr/bin/env python3
"""
merge-watcher — foreground daemon that polls every registered PR.

Phase 1 of `docs/plans/shipped/smatchet-merge-watcher.md`. Reads the per-user
registry at `%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (managed
by `merge-watcher-cli.py`), runs `agents/scripts/core/merge-gates.sh` for each
entry per the configured interval, writes per-PR state to
`%LOCALAPPDATA%/Smatchet/merge-watch/state/<pr>.json`, emits structured
stdout per poll cycle.

Scope has grown past the original poll-only Phase 1: the daemon now acts
on the parsed `merge-gates.sh` state. A GATES_PASSED PR drives squash
auto-merge + cascade to stacked children (Phase 2 — `handle_pass` →
`squash_merge_pr`), BLOCKED on CodeRabbit findings drives the triage
classifier (Phase 3 — `invoke_triage`), and terminal states fire
human-facing notification (Phase 4a — `smatchet-notify.sh`). The
auto-merge path is live, not observe-only: once gates pass the daemon
squash-merges without further prompting. Env knobs below tune the
surrounding behaviour (sanitizer auto-act, CR grace cycles), not whether
a clean PASS merges.

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
  MERGE_WATCH_CR_NONE_GRACE_CYCLES  — poll cycles to wait before treating a NONE +
                                      status-SUCCESS / NONE + pending CodeRabbit state
                                      (skipped or absent review) as pass (default 10;
                                      floored at 1). Mirrors merge-gates.sh CR_GRACE_POLLS
                                      but measured across real cycles, since the daemon
                                      drives merge-gates with MAX_POLLS=1 (its in-process
                                      grace window can never elapse). See
                                      `maybe_pass_cr_none_grace`.
  MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS
                                    — same window, but used when the PR's diff is
                                      pure-docs (per is-pure-docs-diff.sh's allow-list:
                                      docs/, backlog/, agents/scripts/, any *.md).
                                      Default 1 (floored at 1). On a pure-docs diff
                                      CodeRabbit's "Review skipped" is its TERMINAL
                                      verdict — no inline review will ever land — so
                                      the full code-diff grace (10 cycles ≈ 10 min) is
                                      pure dead wait. Shrinking it to ~1 cycle lets a
                                      backlog/docs PR merge in ~1 poll once its required
                                      checks (incl. the `CR finding gate` status check,
                                      which still gates the squash) are green. Does NOT
                                      bypass CR or branch protection — only collapses a
                                      wait that exists for code diffs. See
                                      `maybe_pass_cr_none_grace`.
  MERGE_WATCH_AUTO_RESYNC           — gate-logic self-resync (default "on"; #1428
                                      residual). At startup + every
                                      MERGE_WATCH_RESYNC_EVERY_CYCLES cycles the daemon
                                      checks whether its long-lived host checkout has
                                      drifted behind origin/develop on any gate-logic file
                                      (merge-gates.sh / its .graphql / merge-watcher*.py);
                                      on a SAFE fast-forward (clean tree, on develop, no
                                      divergence) it pulls develop so the on-disk gate
                                      scripts go fresh again — the throughput-safe
                                      complement to merge-gates.sh's FAIL-CLOSED
                                      MERGE_GATES_FRESHNESS guard (which preserves
                                      correctness but wedges a stale daemon at zero merge
                                      throughput). When the daemon's OWN code drifted it
                                      re-execs (POSIX) to load it. "off" disables the
                                      resync, leaving only the fail-closed guard. The
                                      unsafe-tree case (the #1428 feature-branch park) is
                                      never auto-mutated — it warns + waits for a human.
                                      See `maybe_self_resync`.
  MERGE_WATCH_RESYNC_EVERY_CYCLES   — poll cycles between periodic self-resync checks
                                      (default 30; floored at 1). A startup check always
                                      runs regardless of this cadence.
"""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
import pathlib
import re
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
# If shutil.which picked up the WSL bash launcher, override with the Git Bash
# path even though the resolution looked successful — WSL bash isn't usable here
# (it mangles Windows C:\ paths into C:DevSmatchet... and has its own /bin/bash).
# The launcher ships from two locations: %SystemRoot%\System32\bash.exe and the
# newer WindowsApps store shim (...\Microsoft\WindowsApps\bash.exe) — catch both,
# else a Scheduled-Task launch (minimal PATH, no Git\bin prepend) resolves the
# store shim and every poll fails EXIT_127.
_bash_lower = BASH_BIN.lower()
if _bash_lower.endswith(r"system32\bash.exe") or "\\windowsapps\\" in _bash_lower:
    for candidate in (
        r"C:\Program Files\Git\bin\bash.exe",
        r"C:\Program Files (x86)\Git\bin\bash.exe",
        r"C:\msys64\usr\bin\bash.exe",
    ):
        if os.path.exists(candidate):
            BASH_BIN = candidate
            break

# merge-gates.sh's freshness guard shells out to `git` from bash, and the daemon's
# own gate-logic self-resync (maybe_self_resync) drives `git` directly from Python.
# The Scheduled-Task PATH is minimal, so resolve git the same way as gh/jq/bash.
GIT_BIN = _resolve_bin(
    "git",
    r"C:\Program Files\Git\cmd\git.exe",
    r"C:\Program Files\Git\bin\git.exe",
    r"C:\Program Files (x86)\Git\cmd\git.exe",
)

# Import shared helpers from the CLI module (in the same directory).
_HERE = pathlib.Path(__file__).resolve().parent
_CLI_SPEC = importlib.util.spec_from_file_location("merge_watcher_cli", _HERE / "merge-watcher-cli.py")
_CLI = importlib.util.module_from_spec(_CLI_SPEC)
_CLI_SPEC.loader.exec_module(_CLI)  # type: ignore[union-attr]
watcher_root = _CLI.watcher_root
state_dir = _CLI.state_dir
read_registry = _CLI.read_registry


MERGE_GATES_SCRIPT = _HERE / "merge-gates.sh"
MERGE_SNAPSHOT_SCRIPT = _HERE / "merge-snapshot-append.sh"


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


def _pr_lifecycle_state(pr: int, clone_path: str) -> str:
    """Return the PR's lifecycle state via `gh pr view --json state` — one of
    'MERGED' / 'CLOSED' / 'OPEN', or '' when gh is unavailable or errors.

    Isolated from poll_one so the reconcile-on-poll short-circuit is unit
    testable by monkeypatching this helper — native-Windows-Python
    `shutil.which` skips extensionless PATH stubs (it requires a PATHEXT
    extension), so the usual bats `PATH=stub gh` trick can't override the
    absolute GH_BIN the daemon resolves at import. Returning '' on any
    failure makes poll_one fall through to the normal poll path rather than
    stranding the PR on a transient gh hiccup.
    """
    try:
        st = subprocess.run(
            [GH_BIN, "pr", "view", str(pr), "--json", "state", "-q", ".state"],
            cwd=clone_path,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return ""
    if st.returncode == 0:
        return st.stdout.strip().upper()
    return ""


def _poll_owner_repo(pr: int, clone_path: str):
    """Resolve (owner, repo) for a poll via `gh repo view`.

    Returns a 2-tuple `(owner, repo)` on success, or a poll state-dict
    (last_state GH_UNAVAILABLE / GH_REPO_ERR / GH_PARSE_ERR) on failure.
    Extracted from poll_one verbatim as a monkeypatchable seam: the
    integration bats can't reach real gh on Windows (the absolute GH_BIN
    ignores extensionless PATH stubs), so it patches this to return a fixed
    (owner, repo). Production behavior is identical to the previous inline block.
    """
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
        return (meta_json["owner"]["login"], meta_json["name"])
    except (json.JSONDecodeError, KeyError) as exc:
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "GH_PARSE_ERR",
            "last_status_line": f"gh repo view JSON parse: {exc}",
        }


def _poll_run_gates(owner: str, repo: str, pr: int, env: dict):
    """Run merge-gates.sh once and return the CompletedProcess. Seam for test
    monkeypatching — the integration bats supply a fake result with the desired
    returncode/stdout instead of executing the real gh-backed gates script."""
    return subprocess.run(
        [BASH_BIN, str(MERGE_GATES_SCRIPT), owner, repo, str(pr)],
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120,
    )


def _parse_gate_carry(stdout: str) -> dict[str, Any] | None:
    """Parse the `GATE_CARRY nudge_head=… stale_head=… stale_streak=…` line the
    poller emits before its blocked `return 1`. Returns
    {nudged_head, stale_head, stale_streak}, or None when absent (pass /
    early-return — the caller carries the prior registry values forward)."""
    for ln in stdout.splitlines():
        if ln.startswith("GATE_CARRY "):
            fields: dict[str, str] = {}
            for tok in ln[len("GATE_CARRY "):].split():
                k, _, v = tok.partition("=")
                fields[k] = v
            try:
                streak = int(fields.get("stale_streak", "0") or "0")
            except ValueError:
                streak = 0
            return {
                "nudged_head": fields.get("nudge_head", ""),
                "stale_head": fields.get("stale_head", ""),
                "stale_streak": streak,
            }
    return None


def _parse_gate_snapshot(stdout: str) -> dict[str, Any] | None:
    """Parse the `GATE_SNAPSHOT cr_override=<0|1> downgraded=<csv>` line the poller
    emits ONLY on the PASS path (mandatory-merge-snapshot-on-override-merge; ADR-0017).

    Returns {downgraded: [check names], cr_override: bool} naming exactly what an
    override label bypassed at the decision instant, so handle_pass() can record
    those checks in the merge-snapshot ledger's redChecks (not the hardcoded []).
    `downgraded=` is the LAST field and spans the rest of the line (the names are
    jq `join(", ")` output — they contain spaces+commas — so everything after
    "downgraded=" is the verbatim value, NOT whitespace-tokenised).

    Returns None when no GATE_SNAPSHOT line is present (older merge-gates.sh, or a
    non-PASS poll) so the caller falls back to the empty-redChecks path."""
    for ln in stdout.splitlines():
        if not ln.startswith("GATE_SNAPSHOT "):
            continue
        rest = ln[len("GATE_SNAPSHOT "):]
        cr_override = False
        downgraded_csv = ""
        # cr_override is a single fixed-position token; downgraded= spans the
        # remainder (may be empty, may contain spaces/commas). Split on the
        # FIRST "downgraded=" so the value stays intact.
        dg_idx = rest.find("downgraded=")
        if dg_idx >= 0:
            downgraded_csv = rest[dg_idx + len("downgraded="):].strip()
            head = rest[:dg_idx]
        else:
            head = rest
        for tok in head.split():
            k, _, v = tok.partition("=")
            if k == "cr_override":
                cr_override = v.strip() == "1"
        downgraded = [
            seg.strip() for seg in downgraded_csv.split(",") if seg.strip()
        ]
        return {"downgraded": downgraded, "cr_override": cr_override}
    return None


def poll_one(
    entry: dict[str, Any], extra_gates_env: dict[str, str] | None = None
) -> dict[str, Any]:
    """Run merge-gates.sh for one registered PR, parse the last status line,
    return a state dict the daemon writes to state/<pr>.json.

    `extra_gates_env` overrides specific merge-gates env knobs for this single
    invocation (used by `maybe_pass_cr_none_grace` to collapse the CR grace
    window via MERGE_GATES_CR_GRACE_POLLS=0 once the watcher has waited the
    window out across real poll cycles). Overrides win over both the daemon
    defaults and the inherited env (applied via dict.update, not setdefault).
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
    # Reconcile-on-poll: drop PRs that have already merged or closed (by the
    # watcher on a prior cycle, by the orchestrator, or by a human) BEFORE the
    # expensive gates poll. The daemon's only auto-unregister path keys off
    # merge-gates exit 4 (PR_CLOSED_OR_MERGED), but a merged PR never reaches
    # it: ensure_pr_ready_for_review() below cannot `gh pr ready` a merged PR,
    # so it returns READY_FLIP_FAILED every cycle and the entry polls forever.
    # Result before this check: the registry accreted stale MERGED/CLOSED
    # entries (observed 2026-05: 19 zombies, triage_attempts up to 211,
    # starving live PRs' poll slots). A cheap `gh pr view --json state`
    # short-circuits straight to the existing PR_CLOSED_OR_MERGED branch
    # (auto-unregister + one-shot notify), independent of merge-gates' own
    # closed-detection. A transient gh failure returns '' from the helper and
    # falls through to the normal poll path rather than stranding the PR.
    pr_lifecycle = _pr_lifecycle_state(pr, clone_path)
    if pr_lifecycle in ("MERGED", "CLOSED"):
        return {
            "pr": pr,
            "clone_path": clone_path,
            "last_poll_unix": int(time.time()),
            "last_state": "PR_CLOSED_OR_MERGED",
            "last_status_line": (
                f"reconcile precheck: gh reports PR #{pr} {pr_lifecycle}; "
                "short-circuiting to auto-unregister"
            ),
            "gates_return_code": 4,
        }
    # Resolve owner / repo via `gh repo view` — extracted into the
    # _poll_owner_repo seam so the integration bats can monkeypatch the gh hit.
    owner_repo = _poll_owner_repo(pr, clone_path)
    if isinstance(owner_repo, dict):
        return owner_repo  # GH_UNAVAILABLE / GH_REPO_ERR / GH_PARSE_ERR
    owner, repo = owner_repo
    # C4 prong 1 — flip draft → ready BEFORE the gates poll, not just before
    # the merge call. CodeRabbit's `auto_review.drafts: false` default means
    # PRs that stay draft skip CR review entirely; the merge-gates then
    # passes via the "NONE + StatusContext=SUCCESS" branch on CR's placeholder
    # status without ever seeing a real review. Documented as C4 in
    # `docs/reference/agentic-infrastructure-2026-05-23.md` and as P0 in
    # `docs/self-improvement/categories/process.md` (2026-05-21).
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
    # Gate-logic self-freshness guard (#1428): refuse GATES_PASSED when this host
    # checkout's merge-gates.sh is behind origin/develop, so an unattended daemon
    # parked on a stale tree can't merge on out-of-date gate logic. setdefault so an
    # operator can override (warn/off) via the inherited env if a refresh is pending.
    env.setdefault("MERGE_GATES_FRESHNESS", "block")
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
    # Seed the cross-poll nudge guard + STALE streak from the registry entry so
    # the once-per-HEAD @coderabbitai-review nudge + the STALE re-review survive
    # the watcher's MERGE_GATES_MAX_POLLS=1 model (merge-gates.sh's in-process
    # locals reset every invocation otherwise). Mirrors the cr_none_grace counter.
    env["MERGE_GATES_PRIOR_NUDGE_HEAD"] = str(entry.get("nudged_head", "") or "")
    env["MERGE_GATES_PRIOR_STALE_HEAD"] = str(entry.get("stale_head", "") or "")
    env["MERGE_GATES_PRIOR_STALE_STREAK"] = str(int(entry.get("stale_streak", 0) or 0))
    # Per-invocation overrides (e.g. MERGE_GATES_CR_GRACE_POLLS=0 from the
    # CR-NONE grace-elapsed re-poll). update(), not setdefault(), so the
    # override wins over the daemon defaults above and the inherited env.
    if extra_gates_env:
        env.update(extra_gates_env)
    try:
        gates = _poll_run_gates(owner, repo, pr, env)
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
    result = {
        "pr": pr,
        "clone_path": clone_path,
        "last_poll_unix": int(time.time()),
        "last_state": state_label,
        "last_status_line": last_line,
        "gates_return_code": gates.returncode,
    }
    # GATE_CARRY — the poll emits "GATE_CARRY nudge_head=… stale_head=… stale_streak=…"
    # before its blocked return so the watcher can persist the once-per-HEAD nudge
    # guard + STALE streak across cycles (the in-process locals reset under
    # MERGE_GATES_MAX_POLLS=1). Absent on pass / early-return → the daemon carries
    # the prior registry values forward unchanged.
    nudge_carry = _parse_gate_carry(gates.stdout)
    if nudge_carry is not None:
        result["nudge_carry"] = nudge_carry
    # GATE_SNAPSHOT — present only on the PASS path; names the checks an override
    # label bypassed so handle_pass() records them in the merge-snapshot ledger
    # (mandatory-merge-snapshot-on-override-merge). Absent → handle_pass falls
    # back to redChecks=[] (the prior clean-merge behaviour).
    gate_snapshot = _parse_gate_snapshot(gates.stdout)
    if gate_snapshot is not None:
        result["gate_snapshot"] = gate_snapshot
    return result


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
    """Run a `gh` subcommand expecting JSON on stdout; return parsed.

    Raises RuntimeError on ANY failure — non-zero exit, non-JSON stdout, or a
    launch/timeout error (gh not on PATH, or an invalid `cwd`). Normalizing
    launch failures to RuntimeError keeps the single-exception contract every
    caller already relies on (`except RuntimeError`). Previously a raw OSError
    from a bad cwd escaped uncaught and crashed the daemon poll — e.g. a
    stale/moved registered clone_path, or (in tests) a driveless POSIX path on
    Windows, which CreateProcess rejects with NotADirectoryError [WinError 267].
    """
    try:
        result = subprocess.run(
            [GH_BIN, *args],
            cwd=cwd,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        raise RuntimeError(
            f"gh {' '.join(args[:2])} failed to launch (cwd={cwd!r}): {exc}"
        ) from exc
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
       `docs/reference/agentic-infrastructure-2026-05-23.md` § C4).
    2. The eventual REST squash-merge returns HTTP 405 "Pull Request is still
       a draft" if the PR somehow remains draft at merge time.

    Per H2 (`agents/scripts/core/merge-gates.sh:gh_pr_ready_idempotent`): mirror the
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


#: Sentinel return from squash_merge_pr when the PR was placed on a merge queue
#: (auto-merge enabled, GitHub will merge it once the queue's merge_group checks
#: pass) rather than merged immediately. handle_pass treats this distinctly from
#: an actual merge SHA: it does NOT cascade / snapshot / drop-from-registry, so a
#: later poll cycle observes the real merge (merge-gates exit 4) and unregisters.
ENQUEUED_SENTINEL = "enqueued"


def squash_merge_pr(owner: str, repo: str, pr: int) -> str:
    """Enable squash auto-merge on a PR. Returns the merge commit SHA when the
    PR merges immediately (no merge queue configured), or ENQUEUED_SENTINEL when
    it was placed on a GitHub merge queue.

    Queue-safe (docs/plans/active/build-quality-velocity-hardening.md finding #14
    path B): a repo behind a GitHub merge queue FORBIDS a direct REST merge
    (`PUT pulls/{pr}/merge` → 405 "merge queue is enabled"). `gh pr merge --auto`
    is the one call that works both ways — it merges immediately if no queue is
    set (preserving the prior behaviour) and enqueues if a queue exists. The
    pre-enqueue gate-poll (merge-gates.sh) still runs in poll_one before we reach
    here, so this only fires on a PR that already passed the custom poller.

    Per AGENTS.md § Merge gates: GitHub enforces branch protection / conflicts /
    required checks on the merge (or on the queue's merge_group run); we do not
    duplicate that client-side.
    """
    try:
        result = subprocess.run(
            [
                GH_BIN,
                "pr",
                "merge",
                str(pr),
                "--repo",
                f"{owner}/{repo}",
                "--squash",
                "--auto",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        # Normalize launch/timeout failures (subprocess.TimeoutExpired,
        # FileNotFoundError, ...) into the single RuntimeError contract every
        # caller relies on (handle_pass: `except RuntimeError`). Mirrors _gh_json.
        # A bare TimeoutExpired here would escape handle_pass, unwind the daemon's
        # per-PR loop past `except StopSignal`, and crash the WHOLE daemon —
        # stranding every registered PR (infra-outage class; see the daemon_loop
        # per-PR backstop + postmortems.md).
        raise RuntimeError(
            f"gh pr merge --auto of PR #{pr} failed to launch or timed out: {exc}"
        ) from exc
    if result.returncode != 0:
        raise RuntimeError(
            f"gh pr merge --auto of PR #{pr} exited {result.returncode}: "
            f"{(result.stderr or result.stdout).strip()[:200]}"
        )
    # Determine the outcome: immediate merge (no queue) yields state=MERGED +
    # a mergeCommit oid; enqueue (queue configured) leaves state=OPEN.
    try:
        meta = _gh_json(
            [
                "pr",
                "view",
                str(pr),
                "--repo",
                f"{owner}/{repo}",
                "--json",
                "state,mergeCommit",
            ]
        )
    except RuntimeError:
        # The merge/enqueue call succeeded; only the follow-up read failed. Treat
        # as enqueued (conservative — avoids a false cascade/unregister on a PR
        # that may not actually be merged yet).
        return ENQUEUED_SENTINEL
    if isinstance(meta, dict) and meta.get("state") == "MERGED":
        merge_commit = meta.get("mergeCommit") or {}
        return merge_commit.get("oid", "") if isinstance(merge_commit, dict) else ""
    return ENQUEUED_SENTINEL


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
# Gate-logic self-resync (#1428 residual — throughput-safe complement of the
# merge-gates.sh fail-closed freshness guard)
# ---------------------------------------------------------------------------
# The watcher runs as a LONG-LIVED daemon from its own host checkout. merge-gates.sh
# (the actual merge-eligibility logic) is re-read from disk as a fresh subprocess on
# every poll, so a checkout parked behind origin/develop silently enforces STALE gate
# rules — the #1428 escape (an old block-allow-list merged a PR past a RED "Intent
# section"). The shipped fix is a FAIL-CLOSED freshness guard inside merge-gates.sh
# (MERGE_GATES_FRESHNESS=block): it refuses GATES_PASSED when its on-disk blob differs
# from origin/develop's. That preserves correctness but WEDGES throughput — a stale
# daemon merges nothing until a human refreshes the checkout. This self-resync is the
# throughput-safe complement: detect the drift, fast-forward-pull develop so the on-disk
# gate scripts go fresh again (which un-wedges the freshness guard), and — only when the
# daemon's OWN code drifted — re-exec to load it. SAFE-ONLY: it never pulls over a dirty
# tree or switches a branch (the banned shared-tree rug-pull, AGENTS.md § Concurrent
# sessions); the #1428 "parked on a feature branch" tree is left to a human with a loud
# warning, the fail-closed guard still blocking any stale merge in the meantime.

#: Gate-logic files whose staleness changes a MERGE decision — the drift TRIGGER set.
_GATE_LOGIC_RELPATHS = (
    "agents/scripts/core/merge-gates.sh",
    "agents/scripts/core/merge-gates.graphql",
    "agents/scripts/core/merge-watcher.py",
    "agents/scripts/core/merge-watcher-cli.py",
)

#: The subset whose change only takes effect on a PROCESS RESTART (it's THIS running
#: daemon's own code / its imported CLI module, loaded once at start). Drift confined to
#: the other gate-logic files (merge-gates.sh, its query) is fixed by the ff-pull alone —
#: the next poll re-reads the fresh merge-gates.sh from disk, no re-exec needed.
_DAEMON_CODE_RELPATHS = (
    "agents/scripts/core/merge-watcher.py",
    "agents/scripts/core/merge-watcher-cli.py",
)


def _git(args: list[str], cwd: str, timeout: int = 30) -> "subprocess.CompletedProcess | None":
    """Run `git -C <cwd> <args>`; return the CompletedProcess, or None on a launch/
    timeout failure (git missing, bad cwd). Never raises — the self-resync path must
    degrade to 'skip + rely on the fail-closed guard', never crash the daemon (mirrors
    _gh_json's normalize-launch-failure contract)."""
    try:
        return subprocess.run(
            [GIT_BIN, "-C", cwd, *args],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
        )
    except (OSError, subprocess.SubprocessError):
        return None


def _git_ok(args: list[str], cwd: str, timeout: int = 30) -> "str | None":
    """_git wrapper returning stripped stdout on exit 0, else None."""
    r = _git(args, cwd, timeout=timeout)
    if r is not None and r.returncode == 0:
        return r.stdout.strip()
    return None


def _repo_root() -> "str | None":
    """Resolve this checkout's repo root from the daemon's own location. None when the
    daemon isn't running inside a git checkout (nothing to resync)."""
    return _git_ok(["rev-parse", "--show-toplevel"], cwd=str(_HERE))


def _git_fetch_develop(root: str) -> bool:
    """Bounded, refs-only `git fetch origin develop` (never touches the worktree).
    Returns True on success. A failed fetch means origin/develop can't be trusted, so
    the caller must NOT resync against a possibly-stale local ref (it skips)."""
    r = _git(["fetch", "--no-tags", "-q", "origin", "develop"], cwd=root, timeout=60)
    return r is not None and r.returncode == 0


def detect_gate_logic_drift(root: str) -> list[str]:
    """Return the gate-logic relpaths whose ON-DISK blob differs from origin/develop's —
    the same comparison the merge-gates.sh freshness guard makes (git hash-object vs
    origin/develop:<path>), extended across the whole gate-logic surface. Only POSITIVE,
    verifiable drift counts: a path whose local OR develop blob can't be resolved is
    skipped (the fail-closed freshness guard covers the unverifiable case for
    correctness; self-resync acts only on a definite signal). Assumes a prior fetch."""
    drifted: list[str] = []
    for rel in _GATE_LOGIC_RELPATHS:
        run_blob = _git_ok(["hash-object", os.path.join(root, rel)], cwd=root)
        dev_blob = _git_ok(["rev-parse", "-q", "--verify", f"origin/develop:{rel}"], cwd=root)
        if run_blob and dev_blob and run_blob != dev_blob:
            drifted.append(rel)
    return drifted


def _resync_safety(root: str) -> "tuple[bool, str]":
    """Decide whether a fast-forward resync to origin/develop is SAFE on this tree.
    Safe iff (1) clean worktree — never pull over uncommitted work; (2) on `develop` —
    switching a shared tree's branch is the banned rug-pull; (3) local HEAD is an
    ANCESTOR of origin/develop — a true fast-forward with no divergent local commits to
    merge/rebase. Returns (ok, reason). The #1428 'parked on a feature branch' tree
    fails (2) → left untouched for a human, the freshness guard still blocking."""
    porcelain = _git(["status", "--porcelain"], cwd=root)
    if porcelain is None or porcelain.returncode != 0:
        return False, "git status failed"
    if porcelain.stdout.strip():
        return False, "working tree dirty"
    branch = _git_ok(["rev-parse", "--abbrev-ref", "HEAD"], cwd=root)
    if branch is None:
        return False, "git rev-parse HEAD failed"
    if branch != "develop":
        return False, f"not on develop (on '{branch}')"
    anc = _git(["merge-base", "--is-ancestor", "HEAD", "origin/develop"], cwd=root)
    if anc is None:
        return False, "git merge-base failed"
    if anc.returncode != 0:
        return False, "local HEAD diverged from origin/develop (not a fast-forward)"
    return True, "clean develop checkout, fast-forwardable"


def _ff_pull_develop(root: str) -> "tuple[bool, bool]":
    """Fast-forward-only merge of origin/develop (== `git pull --ff-only`). Returns
    (ok, head_moved). head_moved guards a re-exec loop: a no-op 'Already up to date' ff
    leaves HEAD put, so the caller must NOT re-exec on it (a residual blob-vs-HEAD
    divergence the clean-check missed — e.g. an eol filter — would otherwise loop)."""
    before = _git_ok(["rev-parse", "HEAD"], cwd=root)
    merged = _git(["merge", "--ff-only", "origin/develop"], cwd=root, timeout=60)
    if merged is None or merged.returncode != 0:
        return False, False
    after = _git_ok(["rev-parse", "HEAD"], cwd=root)
    return True, bool(before and after and before != after)


def _reexec_daemon(drifted: list[str]) -> None:
    """Replace this process image with a fresh interpreter on the just-pulled code,
    preserving argv. POSIX os.execv is a true same-PID replace (transparent to any
    supervisor + the existing pid file). NOT used on Windows — there os.execv is
    emulated as spawn-new-PID + terminate-self, which would DETACH the daemon from its
    Scheduled Task (orphaning it + risking a second instance at next login); the caller
    takes the Windows branch instead, relying on the already-fresh on-disk merge-gates.sh
    for correctness until the task restarts. Flushes stdio so the log captures the banner
    before the exec; clears the pid file (the re-exec'd process writes a fresh one)."""
    print(
        "merge-watcher: gate-logic drift resynced from origin/develop "
        f"({', '.join(drifted)}); re-execing to load fresh daemon code.",
        flush=True,
    )
    sys.stdout.flush()
    sys.stderr.flush()
    clear_pid_file()
    os.execv(sys.executable, [sys.executable, os.path.abspath(__file__), *sys.argv[1:]])


def _resync_every_cycles() -> int:
    """Periodic self-resync cadence in poll cycles (default 30 ≈ 30 min at the 60 s
    default interval). Floored at 1; an invalid value falls back to the default."""
    try:
        n = int(os.environ.get("MERGE_WATCH_RESYNC_EVERY_CYCLES", "30"))
    except (TypeError, ValueError):
        n = 30
    return max(1, n)


def maybe_self_resync(cycle: int) -> dict[str, Any]:
    """Gate-logic self-resync — the #1428 residual. Detect whether this daemon's
    checkout has drifted behind origin/develop on any gate-logic file; if so AND a
    fast-forward is SAFE, pull develop (re-freshing the on-disk gate scripts the
    freshness guard checks) and — when the daemon's OWN code changed — re-exec (POSIX)
    to load it. Returns a dict describing the outcome for logging/tests; on a successful
    POSIX re-exec it does NOT return (the process is replaced).

    Default ON (the watcher self-heals); MERGE_WATCH_AUTO_RESYNC=off disables it, leaving
    only the fail-closed freshness guard. Every failure mode degrades to a described
    no-op + that guard — this path must never crash the daemon. `cycle` is informational
    (0 = startup check)."""
    mode = os.environ.get("MERGE_WATCH_AUTO_RESYNC", "on").strip().lower()
    if mode == "off":
        return {"resync_action": "disabled (MERGE_WATCH_AUTO_RESYNC=off)"}
    root = _repo_root()
    if not root:
        return {"resync_action": "skipped: daemon not in a git checkout"}
    if not _git_fetch_develop(root):
        return {"resync_action": "skipped: git fetch origin develop failed"}
    drifted = detect_gate_logic_drift(root)
    if not drifted:
        return {"resync_action": "fresh"}
    ok, reason = _resync_safety(root)
    if not ok:
        # #1428 shape (feature-branch / dirty / diverged tree): do NOT mutate a shared
        # tree. Warn loudly; the fail-closed freshness guard still blocks any stale
        # merge, so this is throughput-stopped-but-SAFE until a human refreshes.
        return {
            "resync_action": f"DRIFT but unsafe to auto-resync ({reason}) — refresh the checkout manually",
            "resync_drifted": drifted,
            "resync_needs_human": True,
        }
    pulled, moved = _ff_pull_develop(root)
    if not pulled:
        return {
            "resync_action": "DRIFT; ff-pull failed — staying on fail-closed guard",
            "resync_drifted": drifted,
            "resync_needs_human": True,
        }
    if not moved:
        # ff was a no-op yet a blob still diverged (an eol/filter edge the clean-check
        # missed): re-execing here would loop, so stop and flag a human instead.
        return {
            "resync_action": "DRIFT but ff-pull moved no commit — not re-execing (avoids loop)",
            "resync_drifted": drifted,
            "resync_needs_human": True,
        }
    needs_restart = any(p in _DAEMON_CODE_RELPATHS for p in drifted)
    if not needs_restart:
        # Only merge-gates.sh / its query drifted — the pull refreshed them on disk and
        # the next poll re-reads them as a fresh subprocess. No process restart needed.
        return {
            "resync_action": f"resynced on-disk gate scripts, no restart needed ({', '.join(drifted)})",
            "resync_drifted": drifted,
        }
    if os.name == "posix":
        _reexec_daemon(drifted)  # does not return on success
        return {"resync_action": "re-exec failed (os.execv returned)", "resync_drifted": drifted}
    # Windows: a re-exec would detach the daemon from its Scheduled Task (see
    # _reexec_daemon). The on-disk gate scripts are already fresh (correctness held by
    # them + the freshness guard); the daemon's own code loads on the next task restart.
    return {
        "resync_action": "resynced; daemon code changed — restart SmatchetMergeWatcher to load it (no auto-reexec on Windows)",
        "resync_drifted": drifted,
        "resync_needs_restart": True,
    }


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
    # Wedge escalation — set by maybe_escalate_stuck_pr after a PR sits in a
    # non-progressing blocked state for >= MERGE_WATCH_STUCK_CYCLES cycles.
    "STUCK_NEEDS_ATTENTION",
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
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    if not _looks_like_cr_finding_block(status_line):
        # Sub-bug (a) reset (P1, 2026-05-28 — closes 2026-05-22 P1 entry in
        # docs/self-improvement/categories/tooling.md line 31).
        # When the gates poll shows CR is no longer in a block shape (status
        # line lacks "actionable…block" / "stale_with_findings" / "changes
        # requested"), the per-PR-lifetime triage_attempts counter is stale
        # — it was incremented during a prior CR-finding round that has now
        # cleared. Reset to 0 so a later CR-finding round starts fresh and
        # the registry never appears latched at TRIAGE_BUDGET_EXHAUSTED.
        # Preserve triage_for_head_sha so the per-HEAD diagnostic stays
        # accurate; only the count zeroes out.
        if int(entry.get("triage_attempts", 0)) > 0:
            _bump_triage_attempts(
                pr, clone_path, 0,
                entry.get("triage_for_head_sha", ""),
            )
            extras["triage_reset_on_cr_clear"] = (
                f"prior_attempts={entry.get('triage_attempts')} -> 0"
            )
        extras["triage_action"] = "skipped: BLOCKED but not CR-finding"
        return extras
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
    # Clamp the bump (merge-watcher-triage-attempts-unbounded, PR-3): once the
    # counter is already past budget on the SAME head_sha, every further same-head
    # re-poll used to keep incrementing triage_attempts unbounded (the counter
    # only resets on a HEAD move / CR-clear), so a wedged PR's registry entry grew
    # without bound (observed triage_attempts up to 211 on stale zombies). The
    # counter conveys exactly one fact past budget — "exhausted" — so it carries no
    # information beyond budget+1. When the PR is already exhausted on this head,
    # early-return the EXHAUSTED state WITHOUT persisting a further increment,
    # leaving the registry clamped at budget+1. (A HEAD move above already reset
    # attempts_before to 0, so a fresh push still gets its full budget.)
    if attempts_before > budget:
        extras["triage_attempts"] = attempts_before
        extras["triage_budget"] = budget
        extras["triage_action"] = "BUDGET_EXHAUSTED"
        extras["last_state"] = "TRIAGE_BUDGET_EXHAUSTED"  # overrides BLOCKED for notify
        return extras
    attempts_after = attempts_before + 1
    # Bump the registry entry's triage_attempts (registry-locked). Persist
    # the head_sha so the next poll knows what HEAD this counter is for. The
    # clamp above guarantees attempts_after never exceeds budget+1.
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


def _looks_like_cr_none_grace_wait(status_line: str) -> bool:
    """Detect the CR-NONE grace-wait shapes the merge-gates poller emits when
    CodeRabbit returned review state NONE and the gate is waiting out its grace
    window before assuming a status-only / skipped review.

    Two shapes (see merge-gates.sh NONE branch): `NONE+status-SUCCESS-waiting-
    for-inline (poll N/M)` — CR fired its SUCCESS StatusContext placeholder but
    posted no inline review (the common "Review skipped" case for trivial
    diffs) — and `NONE+pending (poll N/M)` — CR hasn't started. Both pass in
    merge-gates ONLY once its in-process poll index reaches CR_GRACE_POLLS,
    which is unreachable under the watcher's MERGE_GATES_MAX_POLLS=1 driving;
    the watcher counts the window across real cycles instead (see
    `maybe_pass_cr_none_grace`). Excludes finding-shaped blocks (those route to
    triage) — a NONE state never carries actionable findings, but guard anyway.
    """
    s = status_line.lower()
    if "actionable" in s or "changes_requested" in s:
        return False
    return "none+status-success-waiting-for-inline" in s or "none+pending" in s


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
        and ("fail" in s or "failed" in s or "error" in s or "blocked" in s)
    ) or (
        ("addresssanitizer" in s or "undefinedbehaviorsanitizer" in s or "threadsanitizer" in s)
        and ("fail" in s or "failed" in s or "error" in s or "blocked" in s)
    )


def _select_sanitizer_preset(status_line: str) -> str:
    """Map a sanitizer CI failure status line to the CMake preset that reproduces it.

    - TSAN / ThreadSanitizer  → `ninja-tsan-linux` (the only TSAN preset).
    - UBSAN / UndefinedBehaviorSanitizer → `ninja-clang-asan` — per
      `.github/workflows/sanitizer-nightly.yml`, the *only* preset delivering
      ASan AND UBSan (clang-cl); the MSVC preset lacks UBSan.
    - ASAN / everything else → `ninja-msvc-asan` (the "Sanitizer (ASAN via MSVC)"
      PR lane).

    Order matters: check TSAN then UBSAN then fall through to ASAN, because a
    combined "ASan+UBSan" line contains both tokens and must map to the clang
    preset. Before 2026-07 both branches returned an ASAN preset, so a TSAN
    (data-race) failure was handed an ASAN build that can never reproduce it
    (`core-scripts-python-02`); the auto-act repro then always failed to
    reproduce and the debug loop stalled.
    """
    s = status_line.lower()
    if "tsan" in s or "threadsanitizer" in s:
        return "ninja-tsan-linux"
    if "ubsan" in s or "undefinedbehaviorsanitizer" in s:
        return "ninja-clang-asan"
    return "ninja-msvc-asan"


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


def _atomic_reserve_auto_update(pr: int, clone_path: str, head_sha: str, budget: int):
    """Atomically check dedup + budget AND reserve an auto-update-branch slot.

    Sibling of `_atomic_reserve_auto_act` for the BEHIND auto-advance path
    (tooling.md 2026-06-18 `merge-watcher-no-update-branch-for-standalone-behind-
    pr`). Dedup is keyed on `auto_update_for_head_sha` so a single update-branch
    is dispatched per head; a successful update advances the head (restarting the
    key) and a hot develop can't trigger whack-a-mole update-branch churn beyond
    the per-PR-lifetime `budget`.

    Returns ("ok", attempts_after) / ("dedup", None) / ("budget", attempts).
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) != pr or e.get("clone_path") != clone_path:
                continue
            if e.get("auto_update_for_head_sha") == head_sha:
                return ("dedup", None)
            attempts_before = int(e.get("auto_update_attempts", 0))
            attempts_after = attempts_before + 1
            if attempts_after > budget:
                return ("budget", attempts_before)
            e["auto_update_attempts"] = attempts_after
            e["auto_update_for_head_sha"] = head_sha
            e["auto_update_dispatched_at_unix"] = int(time.time())
            _CLI.write_registry(entries)
            return ("ok", attempts_after)
        return ("dedup", None)


def _cr_none_grace_cycles() -> int:
    """Watcher-side CR-NONE grace window, measured in poll CYCLES.

    Default 10 mirrors merge-gates.sh's CR_GRACE_POLLS default so the
    wall-clock wait (cycles x MERGE_WATCH_POLL_INTERVAL) matches the original
    in-process grace intent. Override via MERGE_WATCH_CR_NONE_GRACE_CYCLES.
    Floored at 1 so a misconfigured 0 can't force an instant unreviewed pass.
    """
    try:
        return max(1, int(os.environ.get("MERGE_WATCH_CR_NONE_GRACE_CYCLES", "10")))
    except ValueError:
        return 10


def _cr_none_grace_cycles_pure_docs() -> int:
    """CR-NONE grace window for a PURE-DOCS diff, measured in poll CYCLES.

    Default 1 (vs 10 for a code diff) because CodeRabbit's review-skip on a
    docs-only diff is TERMINAL — no inline review will ever land — so the
    code-diff grace window is pure dead wait (the dominant latency for backlog
    PRs: measured ~6 min on PR #976, all of it this grace). Override via
    MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS. Floored at 1 so a misconfigured
    0 still settles one cycle (confirms the NONE-grace shape persists / gives a
    truly-`pending` CR one cycle to start) before forcing the pass.

    Safety: forcing the CR-review grace pass does NOT skip the squash's required
    status checks — the `CR finding gate` context is in branch_protection and
    merge-gates still refuses the merge until it (and every other required
    context) reports green. This knob only collapses the watcher-side WAIT for
    an inline review that, on a pure-docs diff, is never coming.
    """
    try:
        return max(
            1, int(os.environ.get("MERGE_WATCH_CR_NONE_GRACE_CYCLES_PURE_DOCS", "1"))
        )
    except ValueError:
        return 1


# Mirror of agents/scripts/core/is-pure-docs-diff.sh's allow-list (kept in sync
# by tests/bats/merge_watcher.bats § pure-docs-allowlist-parity). A path is
# docs-class if it is under docs/, backlog/, agents/scripts/, OR ends in .md
# anywhere (markdown is never compiled → never needs a build/review).
_PURE_DOCS_ALLOW = re.compile(r"^(docs/|backlog/|agents/scripts/|.*\.md$)")


def _pr_diff_is_pure_docs(pr: int, clone_path: str) -> bool:
    """True iff EVERY file in PR `pr`'s diff is docs-class (see `_PURE_DOCS_ALLOW`).

    Queries GitHub for the changed-file list (`gh pr view --json files`) rather
    than the local clone's working tree — the daemon's clone is not guaranteed
    to be checked out to the PR head, so a local `git diff` would be wrong.

    Fail-safe: ANY error (gh down, empty/unknown file list) returns False, which
    routes the caller to the conservative full-length code-diff grace window.
    Never raises. Monkeypatched in unit tests via `mw._pr_diff_is_pure_docs`.
    """
    try:
        meta = _gh_json(
            ["pr", "view", str(pr), "--json", "files"], cwd=clone_path
        )
    except (RuntimeError, subprocess.TimeoutExpired, OSError):
        return False
    if not isinstance(meta, dict):
        return False
    files = meta.get("files")
    if not isinstance(files, list) or not files:
        return False  # empty / malformed → fail-safe to the code-diff path
    paths = [f.get("path", "") for f in files if isinstance(f, dict)]
    if not paths or any(not p for p in paths):
        return False
    return all(_PURE_DOCS_ALLOW.match(p) for p in paths)


def _bump_cr_none_grace(
    pr: int, clone_path: str, new_count: int, head_sha: str
) -> None:
    """Persist the per-(PR, head) CR-NONE grace cycle counter in the registry.

    Mirrors `_bump_triage_attempts`. `cr_none_grace_head` pins the counter to a
    HEAD so a fresh push (CR may review the new commit) restarts the window.
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["cr_none_grace_polls"] = new_count
                e["cr_none_grace_head"] = head_sha
                break
        _CLI.write_registry(entries)


def _bump_nudge_state(
    pr: int, clone_path: str, nudged_head: str, stale_head: str, stale_streak: int
) -> None:
    """Persist the cross-cycle CR-nudge guard + STALE streak in the registry.

    Mirrors `_bump_cr_none_grace`. `nudged_head` pins the once-per-HEAD
    @coderabbitai-review guard; `stale_head` / `stale_streak` carry the STALE
    re-review counter. Both survive the MERGE_GATES_MAX_POLLS=1 single-poll model
    that resets merge-gates.sh's in-process locals every cycle.
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["nudged_head"] = nudged_head
                e["stale_head"] = stale_head
                e["stale_streak"] = stale_streak
                break
        _CLI.write_registry(entries)


def maybe_pass_cr_none_grace(
    entry: dict[str, Any], state: dict[str, Any]
) -> dict[str, Any]:
    """Cross-cycle CR-NONE grace driver — closes the wedge where a PR whose only
    blocker is a skipped/absent CodeRabbit review (state NONE + SUCCESS status,
    no inline comments) never merges.

    Root cause: merge-gates.sh passes NONE+SUCCESS only after its in-process
    poll index `p >= CR_GRACE_POLLS`, but `poll_one` runs merge-gates with
    MERGE_GATES_MAX_POLLS=1, so `p` is always 0 — the grace window can never
    elapse within a single invocation and resets every cycle.

    Fix: count consecutive grace-wait cycles per HEAD in the registry; once the
    watcher has waited MERGE_WATCH_CR_NONE_GRACE_CYCLES real cycles, re-poll
    ONCE with MERGE_GATES_CR_GRACE_POLLS=0 so the single poll's grace
    comparison passes and the gate returns GATES_PASSED (which the daemon
    routes into handle_pass). Preserves the grace INTENT (wall-clock wait
    before assuming a status-only / skipped review) while making it reachable.

    Returns a state-delta dict (merged into `state` by the daemon). When the
    forced re-poll passes it carries `last_state == GATES_PASSED`, so the
    daemon's existing PASS branch merges. Fail-closed: if the current HEAD
    can't be resolved, it does NOT force a pass. Only call when
    `state["last_state"] == "BLOCKED"`.
    """
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    status_line = state.get("last_status_line", "")
    if not _looks_like_cr_none_grace_wait(status_line):
        # CR left the grace-wait shape (real review posted, findings, CI block,
        # etc.) — clear any stale counter so a later NONE-wait starts fresh.
        if int(entry.get("cr_none_grace_polls", 0)) != 0:
            _bump_cr_none_grace(pr, clone_path, 0, "")
            return {
                "cr_none_grace_polls": 0,
                "cr_none_grace_action": "reset (CR left NONE-grace-wait state)",
            }
        return {}
    # Resolve current HEAD — fail-closed: without it we can't distinguish a
    # stale counter (old commit) from a live one, so we must not force a pass.
    try:
        meta = _gh_json(
            ["pr", "view", str(pr), "--json", "headRefOid"], cwd=clone_path
        )
        head_sha = meta.get("headRefOid", "") if isinstance(meta, dict) else ""
    except (RuntimeError, subprocess.TimeoutExpired, OSError):
        head_sha = ""
    if not head_sha:
        return {
            "cr_none_grace_action": "HEAD fetch failed; not forcing CR grace this cycle"
        }
    # Pure-docs PRs collapse the grace window: CR's "Review skipped" verdict is
    # TERMINAL on a docs-only diff (no inline review will ever land), so waiting
    # the full code-PR window just burns wall-clock. The required `CR finding
    # gate` status check still gates the squash — this only shrinks the wait,
    # it does NOT bypass CodeRabbit or branch protection.
    if _pr_diff_is_pure_docs(pr, clone_path):
        threshold = _cr_none_grace_cycles_pure_docs()
        grace_kind = "pure-docs"
    else:
        threshold = _cr_none_grace_cycles()
        grace_kind = "code"
    prior = int(entry.get("cr_none_grace_polls", 0))
    if entry.get("cr_none_grace_head", "") != head_sha:
        prior = 0  # new commit — CR may yet review it; restart the window
    new_count = prior + 1
    _bump_cr_none_grace(pr, clone_path, new_count, head_sha)
    if new_count < threshold:
        return {
            "cr_none_grace_polls": new_count,
            "cr_none_grace_head": head_sha,
            "cr_none_grace_action": (
                f"waiting out CR-NONE grace [{grace_kind}] "
                f"({new_count}/{threshold} cycles)"
            ),
        }
    # Grace elapsed across real cycles. Re-poll once with the in-process grace
    # window collapsed so merge-gates' own NONE pass path fires this poll.
    forced = poll_one(entry, extra_gates_env={"MERGE_GATES_CR_GRACE_POLLS": "0"})
    forced["cr_none_grace_polls"] = new_count
    forced["cr_none_grace_head"] = head_sha
    if forced.get("last_state") == "GATES_PASSED":
        forced["cr_none_grace_action"] = (
            f"CR-NONE grace elapsed [{grace_kind}] ({new_count} cycles on "
            f"{head_sha[:8]}); forced MERGE_GATES_CR_GRACE_POLLS=0 -> GATES_PASSED"
        )
    else:
        forced["cr_none_grace_action"] = (
            f"CR-NONE grace elapsed [{grace_kind}] ({new_count} cycles); forced "
            f"grace=0 but gates still {forced.get('last_state')}: "
            f"{forced.get('last_status_line', '')[:80]}"
        )
    return forced


# ---------------------------------------------------------------------------
# Sub-bug (b) — resolveReviewThread after auto-act push
# ---------------------------------------------------------------------------
# After the auto-act spawn pushes a fix commit, CR's per-line review threads
# can stay `isResolved:false` on the prior head even when CR's StatusContext
# on the new head flips to SUCCESS. The merge gate's `cr_open > 0` check then
# keeps the PR BLOCKED indefinitely. These helpers enumerate the stuck CR
# threads and resolve them via the GraphQL `resolveReviewThread` mutation so
# the next poll lands at GATES_PASSED.
#
# Closes the 2026-05-22 P1 entry in docs/self-improvement/categories/
# tooling.md (line 31), sub-bug (b). See docs/plans/active/merge-watcher-triage-
# recovery.md.

_CR_THREADS_QUERY = (
    "query($owner:String!,$repo:String!,$pr:Int!){"
    "repository(owner:$owner,name:$repo){"
    "pullRequest(number:$pr){"
    "headRefOid "
    "reviewThreads(first:100){pageInfo{hasNextPage} nodes{"
    "id isResolved isOutdated "
    "comments(first:1){nodes{author{login __typename}}}"
    "}}"
    "}}}"
)


def _fetch_unresolved_cr_threads(
    owner: str, repo: str, pr: int, clone_path: str
) -> tuple[str, list[str]]:
    """Return (current_head_sha, [thread_id, ...]) for CR-authored,
    non-outdated, unresolved review threads on the PR.

    Raises RuntimeError on gh / GraphQL failure. Callers fail-open: if we
    can't enumerate threads, skip the resolve step and let the next poll
    retry — never wedge the daemon loop on a transient gh hiccup.
    """
    data = _gh_json(
        [
            "api", "graphql",
            "-f", f"query={_CR_THREADS_QUERY}",
            "-F", f"owner={owner}",
            "-F", f"repo={repo}",
            "-F", f"pr={pr}",
        ],
        cwd=clone_path,
        timeout=30,
    )
    if not isinstance(data, dict):
        raise RuntimeError("_fetch_unresolved_cr_threads: top-level not dict")
    try:
        pr_node = data["data"]["repository"]["pullRequest"]
    except (KeyError, TypeError) as exc:
        raise RuntimeError(f"_fetch_unresolved_cr_threads: missing PR node: {exc}")
    head_sha = pr_node.get("headRefOid", "") or ""
    thread_ids: list[str] = []
    for node in pr_node.get("reviewThreads", {}).get("nodes", []) or []:
        if node.get("isResolved") or node.get("isOutdated"):
            continue
        comments = (node.get("comments") or {}).get("nodes") or []
        # First-comment author drives bot-attribution. CR posts the thread-
        # opening inline comment as `coderabbitai` / `coderabbitai[bot]`;
        # human replies on the same thread don't shift authorship of the
        # leading comment, so the filter is robust.
        if not comments:
            continue
        author = (comments[0].get("author") or {}).get("login") or ""
        if author.lower() not in {"coderabbitai", "coderabbitai[bot]"}:
            continue
        tid = node.get("id")
        if tid:
            thread_ids.append(tid)
    return head_sha, thread_ids


_RESOLVE_MUTATION = (
    "mutation($threadId:ID!){"
    "resolveReviewThread(input:{threadId:$threadId}){thread{id isResolved}}"
    "}"
)


def _resolve_review_threads(thread_ids: list[str], clone_path: str) -> tuple[int, int]:
    """Call resolveReviewThread for each id. Return (resolved_count, failed_count).

    Failures are logged to stderr but do NOT raise — one stuck thread should
    not abort the batch (the next poll will retry whichever remain open).
    """
    resolved = 0
    failed = 0
    for tid in thread_ids:
        try:
            _gh_json(
                [
                    "api", "graphql",
                    "-f", f"query={_RESOLVE_MUTATION}",
                    "-f", f"threadId={tid}",
                ],
                cwd=clone_path,
                timeout=20,
            )
            resolved += 1
        except RuntimeError as exc:
            failed += 1
            print(
                f"WARN: resolveReviewThread failed for {tid}: {exc}",
                file=sys.stderr,
            )
    return resolved, failed


def _bump_resolved_threads(
    pr: int, clone_path: str, head_sha: str, resolved_count: int
) -> None:
    """Persist the resolve-action outcome on the registry entry. Tracks the
    head_sha at the time of resolution so subsequent polls can dedup (don't
    re-resolve already-resolved threads on the same head).
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["last_resolved_threads_count"] = resolved_count
                e["last_resolved_at_unix"] = int(time.time())
                e["last_resolved_for_head_sha"] = head_sha
                break
        _CLI.write_registry(entries)


def maybe_resolve_stuck_cr_threads(
    state: dict[str, Any], entry: dict[str, Any]
) -> dict[str, Any]:
    """Sub-bug (b) — resolve CR review threads stuck `isResolved:false` after
    an auto-act push.

    Gate conditions (all must hold):

      1. `MERGE_WATCH_RESOLVE_CR_THREADS` not opted out (default `true` as of
         2026-05-28; set `false` / `0` / `no` to disable).
      2. Registry entry has `auto_act_for_head_sha` recorded (we previously
         dispatched a fix-spawn against some prior head).
      3. Current `headRefOid` differs from `auto_act_for_head_sha` (the push
         landed; we're no longer on the head the spawn was triggered against).
      4. We have NOT already resolved threads on this current head (the
         `last_resolved_for_head_sha` dedup mirrors the `auto_act_for_head_sha`
         pattern — one resolve pass per head).
      5. Daemon's last poll returned BLOCKED with a status_line shape that
         suggests cr_open is the blocker (the early-exit "skipped: BLOCKED
         but not CR-finding" branch — sub-bug (a) reset just fired — implies
         CR review state is clean but threads still open).

    Action: GraphQL fetch of all CR-authored, non-outdated, unresolved review
    threads on the PR; one `resolveReviewThread` mutation per thread.

    Returns extras dict with `resolve_action` describing the outcome.
    """
    # Default-on as of 2026-05-28 — opt-in shipped via PR #487 worked
    # cleanly across 3 production cycles this session (manually run on PR
    # #487 itself, #488, #496 / #497 to unblock stuck STALE_WITH_FINDINGS
    # threads). Plan-doc § Out of scope had flagged "flip default after one
    # production cycle"; the cycle's over. Set the env to "false" (or "0"
    # / "no") to opt back out if the resolution behaviour ever causes a
    # surprise — but the gate conditions (auto_act_for_head_sha recorded,
    # head advanced, status_line not CR-block-shaped) are conservative
    # enough that the false-positive blast radius is bounded.
    if os.environ.get("MERGE_WATCH_RESOLVE_CR_THREADS", "true").strip().lower() in {
        "false", "0", "no",
    }:
        return {}
    prior_auto_act_head = entry.get("auto_act_for_head_sha", "")
    if not prior_auto_act_head:
        return {}
    last_state = state.get("last_state", "")
    if last_state != "BLOCKED":
        return {}
    status_line = state.get("last_status_line", "")
    # We only fire when CR's review state is NOT block-shaped (cr_open is
    # the blocker, not a fresh CR finding). The sub-bug (a) early-exit
    # branch surfaces `triage_action == "skipped: BLOCKED but not CR-finding"`
    # in the same poll's state; use that as the signal so we don't act on
    # genuine CR-finding blocks.
    if _looks_like_cr_finding_block(status_line):
        return {}
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    or_meta = _gh_owner_repo(clone_path)
    if not or_meta:
        return {"resolve_action": "skipped: gh repo view failed"}
    owner, repo = or_meta
    try:
        current_head, thread_ids = _fetch_unresolved_cr_threads(
            owner, repo, pr, clone_path
        )
    except RuntimeError as exc:
        return {"resolve_action": f"skipped: fetch failed: {exc}"}
    if not current_head:
        return {"resolve_action": "skipped: headRefOid empty"}
    if current_head == prior_auto_act_head:
        return {"resolve_action": "skipped: head unchanged since auto-act"}
    if entry.get("last_resolved_for_head_sha") == current_head:
        return {"resolve_action": "suppressed (already resolved on this head)"}
    if not thread_ids:
        # Record the no-op so subsequent polls dedup against this head.
        _bump_resolved_threads(pr, clone_path, current_head, 0)
        return {"resolve_action": "noop: zero unresolved CR threads"}
    resolved, failed = _resolve_review_threads(thread_ids, clone_path)
    # CR feedback PR #487 — only persist the same-head dedup marker when the
    # batch fully succeeded. Otherwise the next poll must retry the still-
    # unresolved threads on this same head; suppressing via
    # `last_resolved_for_head_sha` would leave them stuck until the next push.
    if failed == 0:
        _bump_resolved_threads(pr, clone_path, current_head, resolved)
    return {
        "resolve_action": (
            f"resolved {resolved}/{len(thread_ids)} CR threads "
            f"(failed={failed}) on head {current_head[:8]}"
        ),
        "resolved_thread_ids": thread_ids,
    }


# ---------------------------------------------------------------------------
# Wedge escalation — STUCK_NEEDS_ATTENTION
# (docs/plans/shipped/merge-watcher-stuck-escalation.md)
# ---------------------------------------------------------------------------
# A PR can sit in a non-progressing blocked state forever while the watcher just
# re-logs BLOCKED every cycle. This driver classifies the wedge (conflict /
# behind / CI-failing / blocked-all-green-but-unresolved-threads), counts a
# head-pinned streak across real cycles, and on threshold flips the PR's state
# to STUCK_NEEDS_ATTENTION so the existing maybe_notify toast + the CLI status
# highlight + the SessionStart nudge raise a visible, one-shot signal.
#
# Strictly FAIL-CLOSED: any gh / parse failure, or CI still pending / a required
# check not yet reported, yields reason None (skip — reset the streak), NEVER a
# spurious escalation. A missed escalation self-heals next cycle; a false one
# cries wolf and erodes trust in the nudge.

# merge-gates.sh:929 Poll grammar:
#   Poll N/M — CI: P/T pass (F fail, G pending, W warn-downgraded, R req-missing)
#   | CodeRabbit: S (O open) | User: U | reviewDecision: D
_POLL_CI_RE = re.compile(
    r"CI:\s*\d+/\d+\s*pass\s*\((\d+)\s*fail,\s*(\d+)\s*pending,"
    r"\s*\d+\s*warn-downgraded,\s*(\d+)\s*req-missing\)"
)
_POLL_CR_OPEN_RE = re.compile(r"CodeRabbit:[^|]*?\((\d+)\s*open\)")
_POLL_USER_RE = re.compile(r"User:\s*(\d+)")

_STUCK_REASON_TEXT = {
    "CONFLICT": "merge conflict with base (develop advanced under the PR — needs a rebase)",
    "BEHIND": (
        "branch is behind base and cannot fast-forward — run `gh pr update-branch` "
        "(or `safe-admin-merge.sh <pr>` for a BEHIND-all-green wedge), or set "
        "MERGE_WATCH_AUTO_UPDATE_BEHIND=true so the watcher auto-advances it"
    ),
    "CI_FAILING": "a required CI check is failing (needs a fix push)",
    "UNRESOLVED_THREADS": "all CI green but unresolved CodeRabbit review threads block the merge",
    "REVIEW_REQUIRED": "all CI green but the PR is BLOCKED (missing required review or unresolved user comment)",
}


def _stuck_cycles() -> int:
    """Consecutive non-progressing cycles before a wedge escalates.

    Default 3 (cycles x MERGE_WATCH_POLL_INTERVAL ~= minutes). Floored at 1 so a
    misconfigured 0 can't escalate on the very first cycle (a one-cycle
    transient must never trip it). Override via MERGE_WATCH_STUCK_CYCLES.
    """
    try:
        return max(1, int(os.environ.get("MERGE_WATCH_STUCK_CYCLES", "3")))
    except ValueError:
        return 3


def _parse_poll_ci_counts(status_line: str) -> dict[str, int] | None:
    """Parse the merge-gates Poll line's CI counts.

    Returns {fail, pending, req_missing, cr_open, user} or None when the line is
    not a parseable Poll line (transient gh-failed line / stderr surface / empty
    / forced-state string). None is the fail-closed signal: a caller that can't
    read the counts must not treat the PR as a CI wedge.
    """
    m = _POLL_CI_RE.search(status_line)
    if not m:
        return None
    cr = _POLL_CR_OPEN_RE.search(status_line)
    usr = _POLL_USER_RE.search(status_line)
    return {
        "fail": int(m.group(1)),
        "pending": int(m.group(2)),
        "req_missing": int(m.group(3)),
        "cr_open": int(cr.group(1)) if cr else 0,
        "user": int(usr.group(1)) if usr else 0,
    }


def _classify_pr_wedge(
    entry: dict[str, Any], state: dict[str, Any]
) -> tuple[str | None, str]:
    """Classify whether a PR is genuinely WEDGED vs transiently in-flight.

    Returns (reason, head_sha). `reason` is CONFLICT / BEHIND / CI_FAILING /
    UNRESOLVED_THREADS / REVIEW_REQUIRED when wedged, else None (transient — the
    caller resets the streak, does NOT escalate). `head_sha` is "" when it can't
    be resolved (caller fail-closes).

    The crux is the transient-vs-wedge split: a required check still pending or
    not-yet-reported is NORMAL in-flight progress, never a wedge.
    """
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    status_line = state.get("last_status_line", "")
    counts = _parse_poll_ci_counts(status_line)
    # Authoritative GitHub merge state — one gh view, fail-closed to (None, "").
    try:
        meta = _gh_json(
            ["pr", "view", str(pr), "--json",
             "mergeStateStatus,mergeable,headRefOid"],
            cwd=clone_path,
        )
    except (RuntimeError, subprocess.TimeoutExpired, OSError):
        return None, ""
    if not isinstance(meta, dict):
        return None, ""
    head_sha = meta.get("headRefOid", "") or ""
    merge_state = (meta.get("mergeStateStatus") or "").upper()
    mergeable = (meta.get("mergeable") or "").upper()
    # Conflict wedge (Case 1) — develop advanced under the PR; GitHub refuses
    # the squash. mergeable=CONFLICTING is authoritative; DIRTY corroborates.
    if mergeable == "CONFLICTING" or merge_state == "DIRTY":
        return "CONFLICT", head_sha
    # CI discrimination when the Poll line parsed. A failing required check is a
    # wedge regardless of other still-pending checks (it won't self-heal without
    # a push), so test fail FIRST; only then is a still-pending / not-yet-
    # reported required check TRANSIENT (normal in-flight progress, not a wedge).
    if counts is not None:
        if counts["fail"] > 0:
            return "CI_FAILING", head_sha
        if counts["pending"] > 0 or counts["req_missing"] > 0:
            return None, head_sha
    # Behind base (no conflict). Only after CI is confirmed not-pending, else a
    # normally-building PR reads BEHIND.
    if merge_state == "BEHIND":
        return "BEHIND", head_sha
    # All CI green but still BLOCKED — the green-but-blocked wedge (Case 2):
    # unresolved CR threads under require_conversation_resolution, or a missing
    # required review / unresolved user comment. Distinguish for an actionable
    # nudge.
    if (
        state.get("last_state") == "BLOCKED"
        and counts is not None
        and counts["fail"] == 0
        and counts["pending"] == 0
        and counts["req_missing"] == 0
    ):
        or_meta = _gh_owner_repo(clone_path)
        if not or_meta:
            return None, head_sha
        owner, repo = or_meta
        try:
            _h, thread_ids = _fetch_unresolved_cr_threads(owner, repo, pr, clone_path)
        except RuntimeError:
            return None, head_sha
        if thread_ids:
            return "UNRESOLVED_THREADS", head_sha
        return "REVIEW_REQUIRED", head_sha
    return None, head_sha


def _bump_stuck_streak(
    pr: int, clone_path: str, new_count: int, head_sha: str, reason: str
) -> None:
    """Persist the per-(PR, head) wedge streak + reason in the registry.

    Mirrors `_bump_cr_none_grace`. `stuck_head` pins the streak to a HEAD so a
    fresh push (progress) restarts it; `stuck_reason` feeds the CLI status
    highlight + the SessionStart nudge.
    """
    with _CLI.registry_lock():
        entries = _CLI.read_registry()
        for e in entries:
            if int(e.get("pr", -1)) == pr and e.get("clone_path") == clone_path:
                e["stuck_streak"] = new_count
                e["stuck_head"] = head_sha
                e["stuck_reason"] = reason
                break
        _CLI.write_registry(entries)


def maybe_auto_update_behind(
    entry: dict[str, Any], status_line: str, head_sha: str
) -> dict[str, Any]:
    """Auto-advance a registered PR whose ONLY blocker is BEHIND base.

    Closes the gap where a green + CR-cleared REGISTERED PR that drifts BEHIND a
    busy develop was never advanced — the daemon's sole `update-branch` path was
    the post-merge cascade for stacked CHILDREN, so a STANDALONE behind PR
    starved until a manual `gh pr update-branch` (tooling.md / infra.md
    2026-06-18 `merge-watcher-no-update-branch-for-standalone-behind-pr`; lived on
    PR #1358).

    Opt-in via `MERGE_WATCH_AUTO_UPDATE_BEHIND=true` (off by default — dispatching
    update-branch has a real cost and a hot develop risks churn). Safeguards
    mirror `maybe_auto_act`:
      - ONLY when the rollup is green + CR-cleared (the Poll line parsed AND
        fail/pending/req-missing/cr-open all 0) — never on an unparseable line.
      - Single dispatch per (PR, head_sha); a successful update advances the head
        and restarts the key. Per-PR-lifetime budget `MERGE_WATCH_AUTO_UPDATE_
        BUDGET` (default 2) caps total dispatches so develop churn can't whack-a-
        mole. Both reserved atomically under the registry lock.
    Returns a state-delta dict ({} when it does not / cannot act, so the caller
    falls through to the normal STUCK escalation — e.g. budget exhausted still
    raises the human notify).
    """
    if os.environ.get("MERGE_WATCH_AUTO_UPDATE_BEHIND", "").strip().lower() not in {
        "true", "1", "yes"
    }:
        return {}
    # Green + CR-cleared gate — only advance a PR whose sole blocker is BEHIND.
    counts = _parse_poll_ci_counts(status_line)
    if counts is None:
        return {}
    if not (
        counts["fail"] == 0
        and counts["pending"] == 0
        and counts["req_missing"] == 0
        and counts["cr_open"] == 0
        and counts["user"] == 0   # unresolved user comments also block — don't churn (Cursor #1393)
    ):
        return {}
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    # Strict contract: a NON-EMPTY return means "handled — caller short-circuits
    # the STUCK escalation". So EVERY non-acting path (cannot resolve repo, dedup,
    # over-budget, dispatch failure) MUST return {} so the human escalation still
    # fires; only a SUCCESSFUL dispatch returns the non-empty delta (CodeRabbit
    # #1393 — a non-empty failure return silently suppressed escalation).
    or_meta = _gh_owner_repo(clone_path)
    if not or_meta:
        return {}
    owner, repo = or_meta
    budget_raw = os.environ.get("MERGE_WATCH_AUTO_UPDATE_BUDGET", "2")
    try:
        budget = int(budget_raw)
    except ValueError:
        budget = 2
    reserved, payload = _atomic_reserve_auto_update(pr, clone_path, head_sha, budget)
    if reserved in ("dedup", "budget"):
        # Already advanced this head, or out of budget — let the streak/escalate
        # path raise the human signal instead.
        return {}
    attempts_after = payload  # int — reserved slot index
    ok, msg = cascade_update_child(owner, repo, pr)
    if not ok:
        # Dispatch failed — do NOT reset the streak or short-circuit; fall through
        # so the wedge still accrues toward the human STUCK escalation. (The slot
        # is consumed + head dedup'd, so we won't whack-a-mole this same head.)
        return {}
    # Clear the wedge streak in the REGISTRY (not just the returned state delta —
    # maybe_escalate_stuck_pr reads the streak off the registry ENTRY next cycle;
    # a delta-only reset would not actually clear it — Cursor #1393). The dispatch
    # advances the head, but reset explicitly so a no-op "already up-to-date"
    # update can't leave a stale streak latched.
    _bump_stuck_streak(pr, clone_path, 0, "", "")
    return {
        "stuck_streak": 0,
        "stuck_head": "",
        "stuck_reason": "",
        "auto_update_action": (
            f"update-branch dispatched for BEHIND #{pr} ({msg}) "
            f"[{attempts_after}/{budget}]"
        ),
        "stuck_action": (
            f"auto-update-branch dispatched on BEHIND #{pr} head {head_sha[:8]}: {msg}"
        ),
    }


def maybe_escalate_stuck_pr(
    state: dict[str, Any], entry: dict[str, Any]
) -> dict[str, Any]:
    """Escalate a PR wedged in a non-progressing state for >= _stuck_cycles()
    consecutive cycles by flipping its state to STUCK_NEEDS_ATTENTION — which the
    following maybe_notify toasts once, the CLI status highlights, and the
    SessionStart nudge surfaces.

    Gated to states sibling drivers don't already own: runs only when the PR is
    BLOCKED, or GATES_PASSED-but-merge_failed (the Case-1 DIRTY path where
    handle_pass couldn't squash). Skips CR-finding blocks (triage owns them),
    CR-NONE grace-waits (the grace driver is progressing), and any cycle where
    the resolve-threads step just made progress.

    Fail-closed throughout: a None classification or any gh error resets/holds
    the streak, never escalates. Returns a state-delta dict.
    """
    last_state = state.get("last_state", "")
    merge_action = str(state.get("merge_action", ""))
    is_dirty_pass = last_state == "GATES_PASSED" and merge_action.startswith("merge_failed")
    if last_state != "BLOCKED" and not is_dirty_pass:
        return {}
    pr = int(entry["pr"])
    clone_path = entry["clone_path"]
    status_line = state.get("last_status_line", "")
    # Gate out states owned by sibling drivers — they ARE making progress.
    if _looks_like_cr_finding_block(status_line) or _looks_like_cr_none_grace_wait(status_line):
        return {}
    resolve_action = str(state.get("resolve_action", ""))
    if resolve_action.startswith("resolved ") and not resolve_action.startswith("resolved 0/"):
        return {}
    reason, head_sha = _classify_pr_wedge(entry, state)
    if reason is None:
        # Transient / unclassifiable — clear any stale streak so a real wedge
        # later starts fresh.
        if int(entry.get("stuck_streak", 0)) != 0:
            _bump_stuck_streak(pr, clone_path, 0, "", "")
            return {"stuck_streak": 0, "stuck_action": "reset (PR progressing / transient)"}
        return {}
    if not head_sha:
        # Can't pin the streak to a head — hold without escalating (fail-closed).
        return {"stuck_action": "HEAD unresolved; not escalating this cycle"}
    # BEHIND-all-green auto-advance (opt-in): dispatch update-branch instead of
    # only escalating, so a standalone registered PR behind a busy develop is not
    # starved. Falls through to the streak/escalate path when disabled, not
    # green, dedup'd, or out of budget.
    if reason == "BEHIND":
        au = maybe_auto_update_behind(entry, status_line, head_sha)
        if au:
            return au
    prior = int(entry.get("stuck_streak", 0))
    if entry.get("stuck_head", "") != head_sha:
        prior = 0  # fresh push — progress; restart the streak
    new_count = prior + 1
    threshold = _stuck_cycles()
    _bump_stuck_streak(pr, clone_path, new_count, head_sha, reason)
    if new_count < threshold:
        return {
            "stuck_streak": new_count,
            "stuck_reason": reason,
            "stuck_action": f"wedge [{reason}] {new_count}/{threshold} cycles on {head_sha[:8]}",
        }
    # Threshold reached — escalate. Flip last_state so the following maybe_notify
    # fires once (suppressed thereafter by notify_dispatched_for_state). Preserve
    # a prefix of the original Poll line for forensics.
    return {
        "last_state": "STUCK_NEEDS_ATTENTION",
        "stuck_streak": new_count,
        "stuck_reason": reason,
        "stuck_head": head_sha,
        "stuck_action": (
            f"ESCALATED after {new_count} cycles: {_STUCK_REASON_TEXT.get(reason, reason)}"
        ),
        "last_status_line": (
            f"STUCK_NEEDS_ATTENTION ({reason}) — {_STUCK_REASON_TEXT.get(reason, reason)}; "
            f"original: {status_line[:120]}"
        ),
    }


# Single source of truth for the spawned Claude session's instructions.
# Deliberately spare — no project rules pasted in; the spawned session reads
# AGENTS.md + CLAUDE.md from the clone on its own. We only tell it WHAT to do
# (address PR #N's CodeRabbit findings) and HOW to get a checkout (gh pr
# checkout) so it doesn't waste tokens guessing the branch.
#
# C4 prong 3 (per docs/reference/agentic-infrastructure-2026-05-23.md): the
# spawned session is explicitly instructed to use the `coderabbit-triage`
# agent's classification framework (per `agents/core/coderabbit-triage.md`) — that
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
    "`agents/core/coderabbit-triage.md`). Pass it PR #{pr}; it will fetch the "
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
    "  2. **Invoke `debug-detective`** (per `agents/core/debug-detective.md`) with:\n"
    "     - Failing test: {failing_test}\n"
    "     - CI job log: run `gh run list --branch $(gh pr view {pr} --json "
    "headRefName -q .headRefName) --status failure --limit 5` to find the "
    "sanitizer run URL.\n"
    "     - The sanitizer stderr constitutes the reproducer per slice 10's "
    "reproducer-first contract.\n"
    "  3. Do NOT invoke `coderabbit-triage` — this is a sanitizer failure, "
    "not a CR-finding block.\n"
    "  4. Apply the smallest fix that resolves the sanitizer error.\n"
    "  5. Build with `cmake --build --preset {sanitizer_preset}` and run "
    "`ctest --output-on-failure` in the build dir to confirm the fix.\n"
    "  6. `git commit` with a message of the form "
    "`fix(sanitizer): resolve ASAN/UBSAN finding on PR #{pr}` and `git push`.\n\n"
    "Auto-act head_sha at dispatch was {head_sha}. If the PR's head has moved "
    "since (user pushed manually), STOP — say so and exit without committing.\n"
    "Auto-act is gated to {budget} attempts per PR lifetime; this is attempt "
    "{attempt}/{budget}."
)


def _session_kv_int(text: str, key: str) -> int | None:
    """First `key=<int>` value from a session-registry file's key=value lines,
    or None when absent / non-numeric. Mirrors the bash `sed -n 's/^key=//p' |
    head -n1` + numeric `case` guard in git-janitor.sh / session-tree-banner.sh."""
    prefix = key + "="
    for line in text.splitlines():
        if line.startswith(prefix):
            val = line[len(prefix):].strip()
            return int(val) if val.isdigit() else None
    return None


def _pid_alive(pid: int) -> bool:
    """Best-effort liveness probe for a registered session's ppid.

    POSIX: signal 0 checks existence without delivering anything. Windows:
    os.kill(pid, 0) maps to TerminateProcess and would *kill* the process, so it
    is NEVER used there — probe read-only via OpenProcess + GetExitCodeProcess
    through ctypes instead. Any probe failure returns False (the caller then
    falls back to the ts-freshness signal), matching git-janitor.sh's
    `kill -0 … || alive=0` bash behaviour.
    """
    if pid <= 0:
        return False
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes

            process_query_limited_information = 0x1000
            still_active = 259
            kernel32 = ctypes.windll.kernel32  # type: ignore[attr-defined]
            handle = kernel32.OpenProcess(process_query_limited_information, False, pid)
            if not handle:
                return False
            try:
                code = wintypes.DWORD()
                ok = kernel32.GetExitCodeProcess(handle, ctypes.byref(code))
                return bool(ok) and code.value == still_active
            finally:
                kernel32.CloseHandle(handle)
        except (OSError, AttributeError):
            return False
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # exists, just not signalable by us
    except OSError:
        return False
    return True


def _count_live_sessions(tree_path: str) -> int:
    """Count live interactive Claude Code sessions registered in a tree.

    Mirrors the bash confinement guard git-janitor.sh got in PR #913 (its Step
    3.5): read `<tree>/.claude/.active-sessions/*` — each file is key=value lines
    (branch=/sha=/ppid=/ts=) named by session id (session-tree-banner.sh) — and
    count an entry live when its `ts` is fresh (< 1800s old) OR its `ppid` is
    still alive. Used by maybe_auto_act to confine the HEAD-mutating
    `gh pr checkout` its spawn performs. No self-exclusion: the watcher daemon
    is not itself a registered interactive session.
    """
    reg_dir = pathlib.Path(tree_path) / ".claude" / ".active-sessions"
    if not reg_dir.is_dir():
        return 0
    now = int(time.time())
    live = 0
    try:
        entries = list(reg_dir.iterdir())
    except OSError:
        return 0
    for f in entries:
        if not f.is_file():
            continue
        try:
            text = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        ts = _session_kv_int(text, "ts")
        ppid = _session_kv_int(text, "ppid")
        fresh = ts is not None and (now - ts) < 1800
        alive = ppid is not None and _pid_alive(ppid)
        if fresh or alive:
            live += 1
    return live


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
    # Concurrent-session confinement — mirror the guard git-janitor.sh got in
    # PR #913 (its Step 3.5). The spawn below runs `claude -p` with
    # cwd=clone_path, whose prompt does `gh pr checkout <pr>` — that moves the
    # clone's HEAD. If live interactive Claude Code sessions share this tree,
    # that rug-pulls them (the exact multi-session collision #913 fixes). Defer
    # — without consuming a budget slot (the reserve below is what increments
    # auto_act_attempts) — when any session is live; the next push / poll
    # re-attempts once the tree is idle. See docs/agent-rules/process-rules.md
    # § Concurrent interactive sessions. The server-side squash-merge path
    # (handle_pass) needs no such guard — it never touches a local HEAD.
    live_sessions = _count_live_sessions(clone_path)
    if live_sessions > 0:
        return {
            "auto_act_action": (
                f"deferred: {live_sessions} live session(s) in clone tree "
                "(HEAD-mutating gh pr checkout would rug-pull them)"
            ),
        }
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
        # Select the preset that actually reproduces the failing sanitizer
        # (TSAN→tsan preset, UBSAN→clang ASan+UBSan, ASAN→msvc); see helper.
        sanitizer_preset = _select_sanitizer_preset(status_line)
        prompt = AUTO_ACT_SANITIZER_PROMPT.format(
            pr=pr, owner=owner, repo=repo,
            head_sha=head_sha[:12], budget=budget, attempt=attempts_after,
            failing_test=failing_test, sanitizer_preset=sanitizer_preset,
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


# ---------------------------------------------------------------------------
# Agent-reachable notify sink (merge-watcher-agent-notify, PR-3)
# ---------------------------------------------------------------------------
# maybe_notify() drives smatchet-notify.sh — an in-app toast + a Windows native
# BurntToast fallback aimed at a HUMAN. An autonomous orchestrator that just
# opened a PR has no way to consume that: it would have to poll `gh pr checks`.
# This sink writes a machine-readable, append-only NDJSON event line per terminal
# state transition that a SessionStart hook / the `merge-watcher-cli.py await`
# subcommand can tail. It is purely additive: the human toast still fires.

#: Terminal/actionable states an agent wants to wake on. Superset of NOTIFY_STATES
#: plus GATES_PASSED (merged) so an `await --until=terminal` returns on a merge too.
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
}


def agent_events_path() -> pathlib.Path:
    """Per-user agent-event NDJSON sink. Lives under the watcher root (outside any
    git clone, so a `git clean -fx` can't nuke it) next to the registry + state."""
    return watcher_root() / "agent-events.jsonl"


def append_agent_event(event: dict[str, Any]) -> None:
    """Append one compact JSON event line to the agent-events sink. Best-effort:
    NEVER raises (an event-sink write must not crash the daemon poll loop) and
    never blocks a merge — a failed append degrades to a silent no-op."""
    try:
        watcher_root().mkdir(parents=True, exist_ok=True)
        line = json.dumps(event, sort_keys=True, ensure_ascii=False)
        with open(agent_events_path(), "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except (OSError, ValueError, TypeError):
        pass


def maybe_emit_agent_event(
    state: dict[str, Any], entry: dict[str, Any]
) -> dict[str, Any]:
    """Emit an agent-reachable event line on a terminal/actionable state.

    Mirrors maybe_notify's suppression (one event per distinct state per PR) by
    comparing against the prior state file's `agent_event_emitted_for_state`.
    Returns extras to merge into the state. Runs alongside maybe_notify (the
    human toast); this is the agent-facing complement (merge-watcher-agent-notify).
    """
    cur_state = state.get("last_state", "")
    if cur_state not in AGENT_EVENT_STATES:
        return {}
    pr = int(entry["pr"])
    prior_state_file = state_dir() / f"{pr}.json"
    prior_emitted_for = None
    if prior_state_file.exists():
        try:
            prior = json.loads(prior_state_file.read_text(encoding="utf-8"))
            prior_emitted_for = prior.get("agent_event_emitted_for_state")
        except (json.JSONDecodeError, OSError):
            pass
    if prior_emitted_for == cur_state:
        return {"agent_event_action": "suppressed (same state as last event)"}
    event = {
        "ts_unix": int(time.time()),
        "pr": pr,
        "clone_path": entry.get("clone_path", ""),
        "state": cur_state,
        "status_line": state.get("last_status_line", "")[:300],
        "source": "merge-watcher",
    }
    # Carry the most actionable fields when present so a consumer doesn't re-query.
    for k in ("merge_sha", "stuck_reason", "triage_attempts", "triage_budget"):
        if k in state:
            event[k] = state[k]
    append_agent_event(event)
    return {
        "agent_event_action": f"emitted ({cur_state})",
        "agent_event_emitted_for_state": cur_state,
        "agent_event_emitted_at_unix": int(time.time()),
    }


# ---------------------------------------------------------------------------
# Durable-ledger pre-reset guard (merge-snapshot-ledger-uncommitted-loss-risk, PR-3)
# ---------------------------------------------------------------------------
# _append_merge_snapshot writes each merged-PR audit row to the WORKING COPY of
# docs/self-improvement/merge-snapshots.jsonl on whatever branch the shared tree
# has checked out, leaving the rows UNCOMMITTED — they only reach develop via an
# irregular manual `chore(ledger)` harvest. So a `git reset`/`checkout`/`clean` of
# that tree (which the concurrent-session rules actively encourage for a stale
# shared tree) silently DESTROYS every un-harvested row.
#
# For an autonomous daemon the LOWER-RISK option (vs the daemon committing/pushing
# to a ledger branch — which needs auth, branch management + conflict handling on a
# live unattended process) is this purely-DEFENSIVE pre-reset guard: a function +
# CLI subcommand (`merge-watcher-cli.py ledger-guard`) that a human / git-janitor
# runs (or wires as a pre-reset hook) BEFORE any destructive op on the shared tree.
# It refuses (exit non-zero) when the ledger has uncommitted rows, naming the
# harvest command, so the rows are never silently lost. It mutates nothing.


def _ledger_relpath() -> str:
    return "docs/self-improvement/merge-snapshots.jsonl"


def ledger_has_uncommitted_rows(root: str) -> "tuple[bool, str]":
    """True iff the merge-snapshots.jsonl ledger in checkout `root` has uncommitted
    changes (staged or unstaged) vs HEAD. Returns (dirty, detail). Fail-CLOSED: if
    git can't be queried (missing git / not a checkout), report dirty=True with the
    reason so a guard caller refuses rather than greenlighting a possibly-lossy
    reset on an unverifiable tree. `git status --porcelain <path>` prints nothing
    when the file is clean / untracked-but-unchanged-from-HEAD."""
    rel = _ledger_relpath()
    r = _git(["status", "--porcelain", "--", rel], cwd=root)
    if r is None or r.returncode != 0:
        return True, "git status failed (fail-closed: cannot verify ledger state)"
    porcelain = r.stdout.strip()
    if not porcelain:
        return False, "ledger clean (no uncommitted rows)"
    return True, porcelain


def _configured_override_labels() -> list[str]:
    """Read project.config.json merge_gates.override_labels (config-sourced, not
    hardcoded — same set merge-gates.sh / postmortem-owed.sh use). Returns [] on
    any read/parse failure (the snapshot still writes, just with no labels)."""
    try:
        cfg_path = _HERE / ".." / ".." / ".." / "project.config.json"
        with open(cfg_path, encoding="utf-8") as fh:
            cfg = json.load(fh)
        labels = cfg.get("merge_gates", {}).get("override_labels", [])
        return [str(x) for x in labels] if isinstance(labels, list) else []
    except (OSError, ValueError):
        return []


def _append_merge_snapshot(
    owner: str,
    repo: str,
    pr: int,
    merge_sha: str,
    gate_snapshot: dict[str, Any] | None = None,
) -> str:
    """Write a lossless merge-time gate-verdict snapshot to the committed JSONL
    ledger via merge-snapshot-append.sh (mergeActor='merge-watcher').

    Closes the scope gap: handle_pass() has neither the head SHA nor the
    override-labels at the write-site, so fetch them here (`gh pr view --json
    labels,headRefOid`), filter labels to the configured override set, and shell
    out to the shared idempotent helper.

    redChecks records what an override label actually BYPASSED at the decision
    instant (mandatory-merge-snapshot-on-override-merge; ADR-0017): the CI checks
    tests-/perf-out-of-band downgraded FAIL→WARN (from the PASS-path GATE_SNAPSHOT
    line, via `gate_snapshot`), plus the literal "CodeRabbit" when cr-out-of-band
    waived a real CR block. On a clean merge (no GATE_SNAPSHOT / no load-bearing
    override) redChecks stays [] so postmortem-owed never double-flags a moot label.

    NEVER raises: a ledger-write failure must not abort the merge path. Returns a
    short status string for the extras dict (caller logs it; merge already done).
    """
    try:
        meta = _gh_json(
            [
                "pr",
                "view",
                str(pr),
                "--repo",
                f"{owner}/{repo}",
                "--json",
                "labels,headRefOid",
            ]
        )
        head_sha = meta.get("headRefOid", "") if isinstance(meta, dict) else ""
        pr_labels = [
            lbl.get("name", "")
            for lbl in (meta.get("labels", []) if isinstance(meta, dict) else [])
            if isinstance(lbl, dict)
        ]
        override_set = _configured_override_labels()
        override_present = [lbl for lbl in pr_labels if lbl in override_set]
        override_csv = ",".join(override_present)
        # redChecks — the checks an override actually bypassed at the merge instant.
        # The downgraded CI names come from the PASS-path GATE_SNAPSHOT line (the
        # SAME set merge-gates.sh's $downgraded computed — no forked logic); a
        # load-bearing cr-out-of-band adds the literal "CodeRabbit". Order-stable,
        # de-duped. Empty unless an override was load-bearing → redChecks=[].
        red_checks: list[str] = []
        if gate_snapshot:
            for name in gate_snapshot.get("downgraded", []) or []:
                if name and name not in red_checks:
                    red_checks.append(name)
            if gate_snapshot.get("cr_override") and "CodeRabbit" not in red_checks:
                red_checks.append("CodeRabbit")
        red_csv = ",".join(red_checks)
        result = subprocess.run(
            [
                BASH_BIN,
                str(MERGE_SNAPSHOT_SCRIPT),
                str(pr),
                merge_sha,
                head_sha,
                "GATES_PASSED",
                red_csv,  # redChecks — checks an override bypassed (GATE_SNAPSHOT)
                override_csv,
                "merge-watcher",
            ],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if result.returncode != 0:
            return f"snapshot_append_failed (exit {result.returncode}): {result.stderr.strip()[:200]}"
        return "snapshot_appended"
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        # Lossless ledger is best-effort — the live statusCheckRollup fallback in
        # postmortem-owed.sh covers a missed write; never abort the merge path.
        return f"snapshot_append_skipped: {exc}"


def handle_pass(
    entry: dict[str, Any], gate_snapshot: dict[str, Any] | None = None
) -> dict[str, Any]:
    """PASS-branch handler — squash-merge + cascade to stacked children.

    Returns an additional state dict with `merge_action_*` fields appended.
    Removes the PR from the registry on successful merge.

    `gate_snapshot` (from the PASS-path GATE_SNAPSHOT line) names the checks an
    override label bypassed; threaded into the merge-snapshot ledger so an
    override merge records what it bypassed in redChecks
    (mandatory-merge-snapshot-on-override-merge). None → redChecks=[] (clean merge).
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
    # 3. Squash auto-merge (queue-safe: merges now if no queue, else enqueues).
    try:
        merge_sha = squash_merge_pr(owner, repo, pr)
    except RuntimeError as exc:
        extras["merge_action"] = f"merge_failed: {exc}"
        return extras
    # 3a. Enqueued onto a merge queue (queue configured) — NOT yet merged. Leave
    #     the registry entry in place so a later cycle catches the real merge
    #     (merge-gates exit 4 → PR_CLOSED_OR_MERGED → auto-unregister) and skip
    #     the cascade/snapshot, which require an actual merge commit on develop.
    if merge_sha == ENQUEUED_SENTINEL:
        extras["merge_action"] = "enqueued"
        return extras
    extras["merge_action"] = "merged"
    extras["merge_sha"] = merge_sha
    extras["merged_branch"] = head_branch
    # 3b. Lossless gate-verdict snapshot to the committed ledger (best-effort;
    #     never aborts the merge path). See docs/adr/0017-merge-time-snapshot-ledger.md.
    extras["merge_snapshot"] = _append_merge_snapshot(
        owner, repo, pr, merge_sha, gate_snapshot=gate_snapshot
    )
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
            except (TimeoutError, OSError, subprocess.SubprocessError) as exc:
                # cascade_lock raises TimeoutError on a lock-acquire timeout;
                # cascade_update_child's `gh api PUT update-branch` runs
                # subprocess.run(timeout=30) un-normalized, so a hung update-branch
                # raises subprocess.TimeoutExpired (a subprocess.SubprocessError, NOT
                # the builtins.TimeoutError this clause used to catch) or OSError on a
                # launch failure. Widen to all three so ONE hung/failed child degrades
                # to a per-child ERR row and the cascade still dispatches to its
                # siblings — same exception-contract normalization as squash_merge_pr /
                # _gh_json, applied at the post-merge cascade (which the L2 per-PR
                # backstop would otherwise only coarsely contain, silently dropping the
                # rest of the cascade).
                ok, msg = False, f"{type(exc).__name__}: {exc}"
            children_results.append(
                {"pr": child_pr, "head": child_head, "ok": ok, "msg": msg}
            )
    extras["cascade_children"] = children_results
    return extras


def process_registered_pr(entry: dict[str, Any]) -> dict[str, Any]:
    """Poll + act on ONE registered PR for a single cycle, then flush its state.

    Extracted from daemon_loop's per-PR body so each iteration can be wrapped in
    a backstop (see daemon_loop): one PR's transient failure — a gh launch/timeout
    (now normalized to RuntimeError in squash_merge_pr) or any unexpected raise
    from a sub-handler — degrades to a retry next cycle instead of unwinding the
    loop and crashing the WHOLE daemon, which would strand EVERY registered PR
    (infra-outage class; see postmortems.md). References only `entry` + module
    globals (no cycle/entries/poll_interval state), so the move is behaviour-
    preserving. Returns the final flushed state dict.
    """
    state = poll_one(entry)
    # Persist the cross-cycle nudge guard + STALE streak the poll
    # emitted (GATE_CARRY) so the once-per-HEAD @coderabbitai-review
    # nudge doesn't re-fire next cycle and the STALE streak
    # accumulates (merge-gates.sh's in-process locals reset under
    # MERGE_GATES_MAX_POLLS=1). pop() so it never lands in state/<pr>.json.
    nudge_carry = state.pop("nudge_carry", None)
    if nudge_carry is not None:
        _bump_nudge_state(
            int(entry["pr"]),
            entry["clone_path"],
            nudge_carry["nudged_head"],
            nudge_carry["stale_head"],
            nudge_carry["stale_streak"],
        )
    # CR-NONE grace driver — flip BLOCKED -> GATES_PASSED once a
    # skipped/absent-review grace window has elapsed across real
    # cycles (closes the MAX_POLLS=1 grace wedge). Runs before
    # the dispatch so a forced pass routes into handle_pass.
    if state.get("last_state") == "BLOCKED":
        grace_extras = maybe_pass_cr_none_grace(entry, state)
        if grace_extras:
            state.update(grace_extras)
            if grace_extras.get("cr_none_grace_action"):
                print(
                    f"  PR#{state['pr']:<6} cr-none-grace: "
                    f"{grace_extras['cr_none_grace_action']}"
                )
    # GATE_SNAPSHOT — the override-bypass record the PASS-path poll
    # emitted (which checks tests-/perf-out-of-band downgraded +
    # whether cr-out-of-band waived a CR block). pop() so it never
    # lands in state/<pr>.json; hand to handle_pass so the merge
    # snapshot records what the override bypassed (redChecks),
    # not the hardcoded [] (mandatory-merge-snapshot-on-override-merge).
    gate_snapshot = state.pop("gate_snapshot", None)
    # Phase 2 — PASS-branch auto-merge + cascade.
    if state.get("last_state") == "GATES_PASSED":
        extras = handle_pass(entry, gate_snapshot=gate_snapshot)
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
    elif state.get("last_state") == "PR_CLOSED_OR_MERGED":
        # PR closed or merged externally (merge-gates exit 4):
        # drop it from the registry so the daemon stops polling a
        # dead PR. Without this, closed PRs accrete as stale
        # registry entries (observed: 17 pre-fix EXIT-state PRs).
        # maybe_notify below still fires once so the user sees the
        # terminal state before the entry is removed.
        maybe_remove_from_registry(int(entry["pr"]), entry["clone_path"])
        state["registry_action"] = "auto-unregistered (closed/merged)"
        print(f"  PR#{state['pr']:<6} CLOSED/MERGED -> auto-unregistered")
    else:
        print(
            f"  PR#{state['pr']:<6} state={state['last_state']:<24} "
            f"poll_line={state.get('last_status_line', '')[:120]}"
        )
    # Sub-bug (b) — resolve stuck CR review threads after
    # an auto-act push lands (opt-in via
    # MERGE_WATCH_RESOLVE_CR_THREADS). Runs before notify so
    # that if resolution succeeds, the next poll's gates can
    # pass cleanly without surfacing a spurious notification.
    resolve_extras = maybe_resolve_stuck_cr_threads(state, entry)
    if resolve_extras:
        state.update(resolve_extras)
        if "resolve_action" in resolve_extras:
            print(
                f"    resolve-threads: {resolve_extras.get('resolve_action', '?')}"
            )
    # Wedge escalation — flip a non-progressing BLOCKED / DIRTY-pass PR to
    # STUCK_NEEDS_ATTENTION after _stuck_cycles() consecutive cycles. Runs AFTER
    # resolve-threads (which may have just un-wedged the PR) and BEFORE notify
    # (which consumes the flipped state to fire the one-shot toast).
    stuck_extras = maybe_escalate_stuck_pr(state, entry)
    if stuck_extras:
        state.update(stuck_extras)
        if stuck_extras.get("stuck_action"):
            print(f"    stuck-watch: {stuck_extras.get('stuck_action')}")
    # Phase 4a — fire smatchet-notify on terminal states (human-facing toast).
    notify_extras = maybe_notify(state, entry)
    if notify_extras:
        state.update(notify_extras)
        if "notify_action" in notify_extras and notify_extras["notify_action"] != "suppressed (same state as last notify)":
            print(
                f"    notify: {notify_extras.get('notify_action', '?')}"
            )
    # Agent-reachable event sink — the machine-readable complement to the human
    # toast (merge-watcher-agent-notify). Runs AFTER maybe_notify so the flipped
    # terminal state (incl. STUCK_NEEDS_ATTENTION) is captured; an orchestrator's
    # `merge-watcher-cli.py await` tails this NDJSON. Must run before write_state
    # so agent_event_emitted_for_state lands in state/<pr>.json (the suppression key).
    agent_event_extras = maybe_emit_agent_event(state, entry)
    if agent_event_extras:
        state.update(agent_event_extras)
        if agent_event_extras.get("agent_event_action", "").startswith("emitted"):
            print(
                f"    agent-event: {agent_event_extras.get('agent_event_action', '?')}"
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
    return state


def daemon_loop(poll_interval: int) -> int:
    print(
        f"merge-watcher: daemon started (PID {os.getpid()}, poll_interval={poll_interval}s).\n"
        f"  Registry: {watcher_root()}\n"
        f"  Press Ctrl-C to stop."
    )
    write_pid_file()
    # Startup gate-logic self-freshness check (#1428 residual): before the first poll,
    # make sure this checkout isn't about to enforce STALE gate logic. On a safe drift
    # this pulls develop (and may re-exec on POSIX, replacing this process).
    _startup_resync = maybe_self_resync(0)
    print(f"  self-resync (startup): {_startup_resync.get('resync_action', '?')}")
    if _startup_resync.get("resync_needs_human") or _startup_resync.get("resync_needs_restart"):
        print(
            f"  WARN: stale gate logic — {_startup_resync.get('resync_action')} "
            f"(drifted: {', '.join(_startup_resync.get('resync_drifted', []))})"
        )
    cycle = 0
    try:
        while True:
            cycle += 1
            # Periodic self-resync — re-check drift every N cycles so a days-old daemon
            # picks up gate-logic changes (or flags an unsafe stale tree) without a human
            # restart. Cheap: one bounded fetch + a few hash-objects per N cycles. May
            # re-exec on POSIX when the daemon's own code drifted.
            if cycle % _resync_every_cycles() == 0:
                _resync = maybe_self_resync(cycle)
                print(f"[cycle {cycle}] self-resync: {_resync.get('resync_action', '?')}")
                if _resync.get("resync_needs_human") or _resync.get("resync_needs_restart"):
                    print(
                        f"[cycle {cycle}] WARN: stale gate logic — {_resync.get('resync_action')} "
                        f"(drifted: {', '.join(_resync.get('resync_drifted', []))})"
                    )
            try:
                entries = read_registry()
                if not entries:
                    print(f"[cycle {cycle}] registry empty; sleeping {poll_interval}s")
                else:
                    print(f"[cycle {cycle}] polling {len(entries)} registered PR(s)")
                    for entry in entries:
                        try:
                            process_registered_pr(entry)
                        except StopSignal:
                            # Clean shutdown — the signal handler raises StopSignal
                            # (an Exception subclass) to unwind to the outer handler
                            # below. It MUST re-raise here, BEFORE the broad backstop,
                            # or Ctrl-C/SIGTERM during a PR's processing gets swallowed
                            # and the daemon never stops.
                            raise
                        except Exception as _poll_err:
                            # Per-PR backstop: one PR's transient failure (a gh
                            # launch/timeout, or any unexpected raise in a sub-handler)
                            # degrades to a retry next cycle instead of unwinding the
                            # loop and crashing the WHOLE daemon — which would strand
                            # EVERY registered PR (the infra-outage this fixes).
                            print(
                                f"  WARN: PR#{entry.get('pr', '?')} poll cycle raised "
                                f"{type(_poll_err).__name__}: {_poll_err}; degrading to "
                                "retry next cycle — daemon continues, other PRs unaffected"
                            )
                            continue
            except StopSignal:
                # Clean shutdown must propagate past the per-cycle backstop too:
                # re-raise BEFORE the broad except (same ordering rule as the per-PR
                # backstop), so a signal arriving during read_registry — or any
                # cycle-scope work outside the per-PR try — still unwinds to the
                # outer handler instead of being swallowed below.
                raise
            except Exception as _cycle_err:
                # Per-CYCLE backstop: a cycle-scope failure outside the per-PR try —
                # read_registry raising on a malformed/locked registry file (a
                # concurrent session's tempfile-rename write, a transient Windows
                # file lock, corrupt JSON) — degrades to skip-this-cycle/retry-next
                # instead of unwinding past the StopSignal-only outer handler into
                # main and crashing the WHOLE daemon (the same all-PRs-stranded
                # blast radius the per-PR backstop closes; see postmortems.md).
                print(
                    f"[cycle {cycle}] WARN: cycle raised "
                    f"{type(_cycle_err).__name__}: {_cycle_err}; skipping this cycle, "
                    "retrying next — daemon continues"
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
            "Polls every registered PR via agents/scripts/core/merge-gates.sh + writes per-PR state."
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
