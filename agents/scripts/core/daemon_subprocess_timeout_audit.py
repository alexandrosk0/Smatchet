#!/usr/bin/env python3
"""daemon_subprocess_timeout_audit — WARN-first lint for un-timed subprocess
calls in agentic daemon scripts (PR-22, daemon-loop-per-iteration-backstop-audit).

Class of bug this targets: a `subprocess.run(...)` reached from a long-lived
daemon's per-iteration poll loop with NO `timeout=` kwarg. The per-PR / per-cycle
try/except backstops in a daemon (see merge-watcher.py's poll loop) catch a
RAISED exception, but they do NOT rescue a HUNG child process — a `gh` / `git`
call that never returns wedges the whole daemon silently, stranding every unit
of work it polls. A bounded `timeout=` (paired with a `TimeoutExpired` handler)
converts that hang into a catchable exception the backstop already degrades on.

WARN-first (calibration phase, mirroring the `duplication` precedent): findings
print `[daemon-timeout] WARN ...` to stderr and the gate exits 0. It is a
per-file TEXT proxy, not a control-flow analysis — it cannot prove a given call
is loop-reachable, so it would over-block as a hard gate. The authoritative
backstop is the daemon's own runtime behaviour (and the merge-watcher bats
suite); this lint is a cheap regression tripwire so a NEW un-timed call in a
daemon script is visible at review time.

SCOPE — only scripts that contain a daemon poll loop (a `while True:` body that
sleeps), so one-shot CLI / audit / sweep tools (whose `subprocess.run` is fine
without a timeout — the process exits regardless) are NOT flagged. The loop +
sleep heuristic keeps the false-positive rate low during calibration.

Modes:
  (no args) | --check       scan agents/scripts/core/*.py; WARN per finding; exit 0.
  --selftest                dogfood: a synthetic daemon with an un-timed call in a
                            poll loop must WARN; the same call WITH timeout= must not.
                            Asserts a failure (detection) case.

Exit: 0 always in --check (WARN-first). --selftest: 0 pass / 1 self-test failed /
2 infra error.
"""

from __future__ import annotations

import ast
import os
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent  # agents/scripts/core
DEFAULT_SCAN_DIR = HERE


def _has_daemon_loop(tree: ast.AST) -> bool:
    """True when the module contains a `while True:` whose body (directly or
    nested) calls `time.sleep(...)` / `sleep(...)` — the daemon poll-loop shape.
    A bare `while True:` that breaks immediately (e.g. a lock-acquire spin) does
    not qualify unless it also sleeps on the steady path."""
    for node in ast.walk(tree):
        if not isinstance(node, ast.While):
            continue
        test = node.test
        is_true = (isinstance(test, ast.Constant) and test.value is True)
        if not is_true:
            continue
        for inner in ast.walk(node):
            if isinstance(inner, ast.Call):
                fn = inner.func
                name = ""
                if isinstance(fn, ast.Attribute):
                    name = fn.attr
                elif isinstance(fn, ast.Name):
                    name = fn.id
                if name == "sleep":
                    return True
    return False


def _untimed_subprocess_calls(tree: ast.AST) -> list:
    """Return [(lineno, callee)] for every `subprocess.run(...)` /
    `subprocess.check_output(...)` / `subprocess.call(...)` lacking a `timeout=`
    keyword. `Popen` is intentionally excluded — a Popen is often a deliberate
    fire-and-forget detached child (its timeout, if any, lives on a later
    `.communicate(timeout=)`), so flagging it would be noise."""
    findings = []
    timed_callees = {"run", "check_output", "call", "check_call"}
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        fn = node.func
        callee = ""
        if isinstance(fn, ast.Attribute) and fn.attr in timed_callees:
            mod = fn.value
            if isinstance(mod, ast.Name) and mod.id == "subprocess":
                callee = "subprocess." + fn.attr
        if not callee:
            continue
        has_timeout = any(kw.arg == "timeout" for kw in node.keywords)
        if not has_timeout:
            findings.append((node.lineno, callee))
    return findings


def _scan_file(path: pathlib.Path) -> list:
    """Return WARN findings for one file, or [] (also [] when it is not a daemon)."""
    try:
        src = path.read_text(encoding="utf-8")
    except OSError as exc:
        raise RuntimeError(f"daemon-timeout: cannot read {path}: {exc}") from exc
    try:
        tree = ast.parse(src, filename=str(path))
    except SyntaxError:
        # A non-parseable .py is not this gate's concern (py_compile / other gates
        # own syntax). Skip rather than fail-closed — WARN-first stays advisory.
        return []
    if not _has_daemon_loop(tree):
        return []
    return _untimed_subprocess_calls(tree)


def run_check(scan_dir: pathlib.Path) -> int:
    if not scan_dir.is_dir():
        sys.stderr.write(f"daemon-timeout: scan dir not found: {scan_dir}\n")
        return 2
    total = 0
    for path in sorted(scan_dir.glob("*.py")):
        if path.name == os.path.basename(__file__):
            continue
        findings = _scan_file(path)
        for lineno, callee in findings:
            total += 1
            sys.stderr.write(
                f"[daemon-timeout] WARN {path.name}:{lineno} — {callee}(...) in a "
                "daemon poll loop carries no `timeout=`; a hung child wedges the "
                "whole daemon (the per-iteration backstop catches raises, not hangs). "
                "Add `timeout=` + handle subprocess.TimeoutExpired. "
                "(WARN-first calibration; not blocking.)\n"
            )
    if total == 0:
        print("daemon-timeout: PASS — no un-timed subprocess calls in daemon poll loops.")
    else:
        print(f"daemon-timeout: {total} advisory WARN(s) (calibration phase; exit 0).")
    return 0


def run_selftest() -> int:
    import tempfile

    rc = 0
    daemon_untimed = (
        "import subprocess, time\n"
        "def loop():\n"
        "    while True:\n"
        "        subprocess.run(['gh', 'pr', 'view'])\n"
        "        time.sleep(5)\n"
    )
    daemon_timed = (
        "import subprocess, time\n"
        "def loop():\n"
        "    while True:\n"
        "        subprocess.run(['gh', 'pr', 'view'], timeout=15)\n"
        "        time.sleep(5)\n"
    )
    oneshot_untimed = (
        "import subprocess\n"
        "def main():\n"
        "    subprocess.run(['git', 'status'])\n"
    )

    def findings_for(src: str) -> list:
        with tempfile.NamedTemporaryFile(
            "w", suffix=".py", delete=False, encoding="utf-8"
        ) as fh:
            fh.write(src)
            name = fh.name
        try:
            return _scan_file(pathlib.Path(name))
        finally:
            try:
                os.unlink(name)
            except OSError:
                pass

    # selftest: asserts-failure — an un-timed subprocess call in a daemon poll
    # loop MUST be detected (the gate's whole job).
    if not findings_for(daemon_untimed):
        sys.stderr.write(
            "daemon-timeout selftest: FAIL — un-timed daemon-loop call NOT flagged\n"
        )
        rc = 1

    # A timed call in the same loop must NOT be flagged.
    if findings_for(daemon_timed):
        sys.stderr.write(
            "daemon-timeout selftest: FAIL — timed daemon-loop call wrongly flagged\n"
        )
        rc = 1

    # A one-shot (no poll loop) un-timed call must NOT be flagged.
    if findings_for(oneshot_untimed):
        sys.stderr.write(
            "daemon-timeout selftest: FAIL — one-shot un-timed call wrongly flagged\n"
        )
        rc = 1

    if rc == 0:
        print("daemon_subprocess_timeout_audit --selftest: PASS")
    return rc


def main(argv: list) -> int:
    arg = argv[1] if len(argv) > 1 else "--check"
    if arg == "--check":
        return run_check(DEFAULT_SCAN_DIR)
    if arg == "--selftest":
        return run_selftest()
    if arg in ("-h", "--help"):
        print("usage: daemon_subprocess_timeout_audit.py [--check|--selftest]")
        return 0
    sys.stderr.write(
        "usage: daemon_subprocess_timeout_audit.py [--check|--selftest]\n"
    )
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
