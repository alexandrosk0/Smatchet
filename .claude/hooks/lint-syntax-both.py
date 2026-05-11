#!/usr/bin/env python3
"""Both-target syntax check for first-party Smatchet .cpp files.

Reads build/ninja-iter-msys2/compile_commands.json and runs every matching
compile command with -fsyntax-only (no codegen / no linking). Catches changes
that compile in SmatchetStandalone but break SmatchetCore_DX12 (and vice
versa) before they slip into the codebase.

Invoked by .claude/hooks/lint-cpp.sh as: python lint-syntax-both.py <file>
Exits 0 silently on success; exits 1 with diagnostics on stderr if either
target fails to syntax-check the file.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from pathlib import Path


def _norm(p: str) -> str:
    return str(p).replace("\\", "/").lower()


def _target_name(command: str) -> str:
    # CMake-emitted paths use the host separator: forward slashes on POSIX,
    # backslashes on Windows. Match either so target labels render correctly
    # in error messages on both toolchains.
    m = re.search(r"CMakeFiles[/\\]+([^/\\.]+)\.dir", command)
    return m.group(1) if m else "unknown"


def main() -> int:
    if len(sys.argv) < 2:
        return 0

    src = Path(sys.argv[1]).resolve()
    if src.suffix.lower() != ".cpp":
        return 0  # headers are checked indirectly via dependent .cpp edits

    project = Path(os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()).resolve()
    cc_path = project / "build" / "ninja-iter-msys2" / "compile_commands.json"
    if not cc_path.is_file():
        return 0  # build not configured yet — degrade silently

    try:
        entries = json.loads(cc_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return 0

    src_key = _norm(src)
    matches = [e for e in entries if _norm(e.get("file", "")) == src_key]
    if not matches:
        return 0  # file not in compile DB yet (e.g. just added) — skip

    failures: list[tuple[str, str]] = []
    for entry in matches:
        cmd = entry["command"]
        # Strip `-o <obj>` so -fsyntax-only doesn't try to write an output.
        cmd = re.sub(r"-o\s+\S+\.obj", "", cmd)
        cmd += " -fsyntax-only"
        target = _target_name(entry["command"])
        result = subprocess.run(
            cmd,
            shell=True,
            cwd=entry["directory"],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            diag = (result.stderr or result.stdout).strip()
            failures.append((target, diag))

    if failures:
        for target, diag in failures:
            sys.stderr.write(f"[syntax-check FAIL: {target}]\n{diag}\n\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
