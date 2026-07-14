#!/usr/bin/env python3
"""
merge-watcher-cli — register / unregister / status / list subcommands.

Phase 1 of `docs/plans/shipped/smatchet-merge-watcher.md`. Per-user registry at
`%LOCALAPPDATA%/Smatchet/merge-watch/active.json` (cross-clone). The
companion daemon (`merge-watcher.py`) polls every registered PR via
`agents/scripts/core/merge-gates.sh`.

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
  merge-watch prune [--dry-run]    # unregister PRs gh reports MERGED/CLOSED
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import time


# ---------------------------------------------------------------------------
# Per-user registry primitives (shared with the daemon merge-watcher.py).
# Extracted to an import-legal module (core-scripts-python-10) so the daemon
# imports them normally instead of via importlib on this hyphenated filename.
# ---------------------------------------------------------------------------
# Ensure this script's own dir is importable so the sibling module resolves even
# when launched by absolute path from an unrelated cwd (Scheduled Task / cron).
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from merge_watcher_registry import (  # noqa: E402
    watcher_root,
    state_dir,
    registry_lock,
    read_registry,
    write_registry,
)


# ---------------------------------------------------------------------------
# Clone-path resolution (gh / git surfaces)
# ---------------------------------------------------------------------------
def resolve_clone_path(cwd: str | pathlib.Path | None = None) -> str:
    """Find the PRIMARY clone root for `cwd` (default: `os.getcwd()`).

    Canonicalizes a linked-worktree cwd (`.claude/worktrees/<slug>`) back to its
    main checkout so a watcher entry registered from a worktree de-dupes onto the
    stable main clone instead of binding to an ephemeral worktree path that
    vanishes at teardown (tooling.md 2026-06-18 `merge-watcher-register-keyed-by-
    cwd-clone` :28). `git rev-parse --git-common-dir` points at the PRIMARY
    checkout's `.git` (absolute from a worktree, relative `.git` from the main
    clone); the parent of that `.git` dir IS the main clone root. A genuinely
    separate full clone has its OWN common-dir, so distinct clones stay distinct.

    Raises if not inside a git repo.
    """
    cwd_str = str(cwd or os.getcwd())

    def _git(*rev_args: str) -> str:
        try:
            r = subprocess.run(
                ["git", "-C", cwd_str, "rev-parse", *rev_args],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=15,
            )
        except subprocess.TimeoutExpired as exc:
            raise RuntimeError(
                f"merge-watcher: `git rev-parse {' '.join(rev_args)}` in '{cwd_str}' "
                "timed out — a hung git can't be allowed to wedge the caller."
            ) from exc
        if r.returncode != 0:
            raise RuntimeError(
                f"merge-watcher: cwd '{cwd_str}' is not inside a git repository "
                f"(`git rev-parse {' '.join(rev_args)}` exited {r.returncode}): "
                f"{r.stderr.strip()}"
            )
        return r.stdout.strip()

    toplevel = _git("--show-toplevel")
    common = _git("--git-common-dir")
    # --git-common-dir is relative (".git") when invoked from the main clone;
    # resolve it against the toplevel so the parent-dir step works in both cases.
    if not os.path.isabs(common):
        common = os.path.join(toplevel, common)
    main_root = os.path.dirname(os.path.normpath(common))
    return main_root.replace(os.sep, "/")


# ---------------------------------------------------------------------------
# gh lifecycle-state probe (for `prune`)
# ---------------------------------------------------------------------------
def _resolve_gh() -> str:
    """Locate the GitHub CLI. A Scheduled Task that runs `prune` inherits a
    minimal PATH that often lacks the gh install dir, so probe the standard
    Windows locations after PATH (mirrors merge-watcher.py's _resolve_bin)."""
    import shutil

    via_path = shutil.which("gh") or shutil.which("gh.exe")
    if via_path:
        return via_path
    for candidate in (
        r"C:\Program Files\GitHub CLI\gh.exe",
        r"C:\Program Files (x86)\GitHub CLI\gh.exe",
        os.path.expandvars(r"%LOCALAPPDATA%\Programs\GitHub CLI\gh.exe"),
    ):
        if candidate and os.path.exists(candidate):
            return candidate
    return "gh"  # last resort — subprocess will FileNotFoundError loudly


_GH_BIN = _resolve_gh()


def _pr_lifecycle_state(pr: int, clone_path: str) -> str:
    """Return the PR's lifecycle state via `gh pr view --json state` — one of
    'MERGED' / 'CLOSED' / 'OPEN', or '' when gh is unavailable or errors.

    Isolated so `prune` is unit-testable by monkeypatching this seam (the bats
    suite cannot reliably stub a `gh` binary on native-Windows Python — its
    shutil.which skips extensionless PATH stubs). Returning '' on any failure
    makes `prune` KEEP the entry (fail-safe: never unregister on uncertainty).
    """
    try:
        r = subprocess.run(
            [_GH_BIN, "pr", "view", str(pr), "--json", "state", "-q", ".state"],
            cwd=clone_path or None,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired, OSError):
        return ""
    if r.returncode == 0:
        return r.stdout.strip().upper()
    return ""


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
        # STUCK highlight — surface the wedge reason + streak the daemon's
        # maybe_escalate_stuck_pr persisted, so a wedged PR is visible at a
        # glance (not buried in a generic BLOCKED). Shown whenever the registry
        # carries a non-empty stuck_reason with a positive streak.
        stuck_reason = e.get("stuck_reason", "")
        stuck_streak = int(e.get("stuck_streak", 0) or 0)
        note = ""
        if stuck_reason and stuck_streak > 0:
            flag = "STUCK" if last_state == "STUCK_NEEDS_ATTENTION" else "wedge?"
            note = f"{flag}[{stuck_reason} x{stuck_streak}]"
        rows.append(
            (
                f"#{pr}",
                pathlib.Path(e.get("clone_path", "?")).name or "?",
                last_state,
                last_poll,
                str(e.get("triage_attempts", 0)),
                note,
            )
        )
    header = ("PR", "CLONE", "LAST_STATE", "LAST_POLL", "TRIAGE", "NOTE")
    widths = [max(len(r[i]) for r in [header, *rows]) for i in range(len(header))]
    fmt = "  ".join(f"{{:<{w}}}" for w in widths)
    print(fmt.format(*header))
    print("-" * (sum(widths) + 2 * (len(header) - 1)))
    for r in rows:
        print(fmt.format(*r))
    stuck_prs = [r[0] for r in rows if r[5].startswith("STUCK[")]
    if stuck_prs:
        print(
            f"\n  WARNING: {len(stuck_prs)} PR(s) STUCK_NEEDS_ATTENTION "
            f"({', '.join(stuck_prs)}) -- wedged and will NOT merge without a human "
            f"action (rebase / fix CI / resolve threads), or `merge-watch unregister <pr>`."
        )
    return 0


def cmd_list(args: argparse.Namespace) -> int:
    print(json.dumps(read_registry(), indent=2, sort_keys=True))
    return 0


def cmd_prune(args: argparse.Namespace) -> int:
    """Sweep the registry: unregister every PR that gh reports as MERGED or
    CLOSED (and wipe its per-PR state file).

    Belt-and-suspenders to the daemon's reconcile-on-poll short-circuit: the
    daemon can only reconcile while it is RUNNING, so a janitor that runs this
    verb heals the registry even across daemon-down windows (crash, logout,
    machine off). OPEN PRs and PRs whose state can't be determined (gh
    error/offline -> '') are KEPT — fail-safe; never unregister on uncertainty.

    `--dry-run` reports the plan without mutating the registry.
    """
    dry_run = bool(getattr(args, "dry_run", False))
    entries = read_registry()
    if not entries:
        print("merge-watch prune: registry empty; nothing to do.")
        return 0
    # Query gh state OUTSIDE the registry lock — gh calls are slow and we must
    # not hold the cross-process lock across them. The actual removal re-reads
    # under the lock and deletes only the keys we resolved here.
    pruned = []   # (pr, clone_path, state, registered_at)
    kept = []     # (pr, clone_path)  — OPEN
    unknown = []  # (pr, clone_path)  — gh error / offline
    for e in entries:
        pr = int(e.get("pr", -1))
        clone_path = e.get("clone_path", "")
        state = _pr_lifecycle_state(pr, clone_path)
        if state in ("MERGED", "CLOSED"):
            pruned.append((pr, clone_path, state, e.get("registered_at")))
        elif state == "OPEN":
            kept.append((pr, clone_path))
        else:
            unknown.append((pr, clone_path))

    if pruned and not dry_run:
        # Identity includes registered_at so an entry unregistered + re-registered
        # between the gh probe (above, outside the lock) and this locked delete is
        # NOT pruned as if it were the original watch (CR #534).
        prune_keys = {(pr, cp, ra) for pr, cp, _state, ra in pruned}
        with registry_lock():
            current = read_registry()
            remaining = [
                e
                for e in current
                if (int(e.get("pr", -1)), e.get("clone_path", ""), e.get("registered_at"))
                not in prune_keys
            ]
            write_registry(remaining)
            for pr, _cp, _state, _ra in pruned:
                sf = state_dir() / f"{pr}.json"
                if sf.exists():
                    try:
                        sf.unlink()
                    except OSError:
                        pass

    verb = "would prune" if dry_run else "pruned"
    for pr, cp, state, _ra in pruned:
        print(f"merge-watch prune: {verb} #{pr} ({state}) [{pathlib.Path(cp).name}]")
    for pr, cp in unknown:
        print(
            f"merge-watch prune: kept #{pr} (state unknown — gh error/offline) "
            f"[{pathlib.Path(cp).name}]",
            file=sys.stderr,
        )
    print(
        f"merge-watch prune: {verb} {len(pruned)}, kept {len(kept)} open, "
        f"{len(unknown)} unknown."
    )
    return 0


def agent_events_path() -> pathlib.Path:
    """Per-user agent-event NDJSON sink (merge-watcher-agent-notify, PR-3). Same
    path the daemon's append_agent_event writes; under the watcher root so a
    `git clean -fx` can't nuke it."""
    return watcher_root() / "agent-events.jsonl"


def cmd_await(args: argparse.Namespace) -> int:
    """Block until the watcher emits a matching agent-event for `pr`, then print
    it as JSON (merge-watcher-agent-notify, PR-3).

    An orchestrator that just opened a PR runs this instead of an ad-hoc
    `gh pr checks --watch` + thread-fetch dance: it tails the daemon's
    agent-events NDJSON sink and returns on the next event for the PR whose state
    matches `--until`. NO daemon change — the daemon already appends events.

      --until blocking  : return on any actionable BLOCKED-class state
                          (TRIAGE_BUDGET_EXHAUSTED / STUCK_NEEDS_ATTENTION /
                          CI_FAIL / READY_FLIP_FAILED) — the orchestrator must act.
      --until terminal  : also return on GATES_PASSED / PR_CLOSED_OR_MERGED (the
                          PR reached an end state). Default.
      --timeout N       : seconds to wait (default 0 = wait forever).

    Only events APPENDED AFTER this command starts count (it records the sink's
    byte offset at start), so a stale historical event for the PR doesn't return
    instantly. Exit 0 on a matching event, 124 on timeout, 2 on bad args.
    """
    pr = int(args.pr)
    until = getattr(args, "until", "terminal")
    timeout = float(getattr(args, "timeout", 0) or 0)
    blocking_states = {
        "TRIAGE_BUDGET_EXHAUSTED", "STUCK_NEEDS_ATTENTION",
        "CI_FAIL", "READY_FLIP_FAILED", "GH_API_DOWN",
        "PAGINATION_OVERFLOW", "TIMEOUT",
    }
    terminal_states = blocking_states | {"GATES_PASSED", "PR_CLOSED_OR_MERGED"}
    want = blocking_states if until == "blocking" else terminal_states
    sink = agent_events_path()
    # Start at the current end-of-file so only NEW events count.
    start_offset = sink.stat().st_size if sink.exists() else 0
    deadline = time.monotonic() + timeout if timeout > 0 else None
    while True:
        if sink.exists():
            try:
                with open(sink, encoding="utf-8") as fh:
                    fh.seek(start_offset)
                    for line in fh:
                        start_offset += len(line.encode("utf-8"))
                        line = line.strip()
                        if not line:
                            continue
                        try:
                            ev = json.loads(line)
                        except json.JSONDecodeError:
                            continue
                        if int(ev.get("pr", -1)) == pr and ev.get("state") in want:
                            print(json.dumps(ev, sort_keys=True))
                            return 0
            except OSError:
                pass
        if deadline is not None and time.monotonic() >= deadline:
            print(
                f"merge-watch await: timed out after {timeout:g}s waiting for "
                f"PR #{pr} ({until})",
                file=sys.stderr,
            )
            return 124
        time.sleep(2.0)


def cmd_ledger_guard(args: argparse.Namespace) -> int:
    """Refuse a destructive git op when the merge-snapshots ledger has uncommitted
    rows (merge-snapshot-ledger-uncommitted-loss-risk, PR-3).

    The watcher appends each merged-PR audit row to the WORKING COPY of
    docs/self-improvement/merge-snapshots.jsonl, uncommitted, on whatever branch
    the shared tree has checked out. A `git reset`/`checkout`/`clean` of that tree
    silently destroys every un-harvested row. Run this as a pre-reset hook (or by
    hand before any destructive op on a shared tree): exit 1 if the ledger is dirty
    so the rows are harvested first; exit 0 when clean.

    Mutates nothing. Resolves the clone root from cwd (or --clone-path).
    """
    clone_path = getattr(args, "clone_path", None) or os.getcwd()
    rel = "docs/self-improvement/merge-snapshots.jsonl"
    try:
        r = subprocess.run(
            ["git", "-C", clone_path, "status", "--porcelain", "--", rel],
            capture_output=True, text=True, encoding="utf-8", errors="replace",
            timeout=15,
        )
    except subprocess.TimeoutExpired:
        # Fail-closed: a hung `git status` can't verify ledger state -> refuse,
        # same as a non-zero exit below (never greenlight a possibly-lossy reset).
        print(
            f"merge-watch ledger-guard: `git status` in '{clone_path}' timed out "
            "— cannot verify ledger state; refusing.",
            file=sys.stderr,
        )
        return 1
    if r.returncode != 0:
        # Fail-closed: cannot verify → refuse (don't greenlight a possibly-lossy reset).
        print(
            f"merge-watch ledger-guard: cannot verify ledger state in '{clone_path}' "
            f"(git status exited {r.returncode}: {r.stderr.strip()[:160]}) — refusing.",
            file=sys.stderr,
        )
        return 1
    porcelain = r.stdout.strip()
    if not porcelain:
        print(f"merge-watch ledger-guard: ledger clean in '{clone_path}'.")
        return 0
    print(
        f"merge-watch ledger-guard: REFUSING — {rel} has UNCOMMITTED rows in "
        f"'{clone_path}':\n{porcelain}\n"
        "Harvest them first (commit to a `chore(ledger)` PR) before any "
        "reset/checkout/clean, or they are lost.",
        file=sys.stderr,
    )
    return 1


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="merge-watch",
        description=(
            "Per-user CLI for smatchet-merge-watcher Phase 1 (registry CRUD). "
            "See docs/plans/shipped/smatchet-merge-watcher.md for the full design."
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

    pr = sub.add_parser(
        "prune",
        help="unregister PRs that gh reports as MERGED/CLOSED (registry janitor)",
    )
    pr.add_argument(
        "--dry-run",
        action="store_true",
        help="report what would be pruned without mutating the registry",
    )
    pr.set_defaults(func=cmd_prune)

    aw = sub.add_parser(
        "await",
        help="block until the watcher emits a matching agent-event for a PR "
        "(merge-watcher-agent-notify)",
    )
    aw.add_argument("pr", help="PR number to await")
    aw.add_argument(
        "--until",
        choices=("blocking", "terminal"),
        default="terminal",
        help="return on a BLOCKED-class state (blocking) or any end state "
        "incl. GATES_PASSED/closed (terminal, default)",
    )
    aw.add_argument(
        "--timeout",
        type=float,
        default=0,
        help="seconds to wait before giving up (default 0 = forever)",
    )
    aw.set_defaults(func=cmd_await)

    lg = sub.add_parser(
        "ledger-guard",
        help="refuse (exit 1) when the merge-snapshots ledger has uncommitted "
        "rows — a pre-reset hook (merge-snapshot-ledger-uncommitted-loss-risk)",
    )
    lg.add_argument(
        "--clone-path",
        default=None,
        help="checkout to inspect (default: cwd)",
    )
    lg.set_defaults(func=cmd_ledger_guard)

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
