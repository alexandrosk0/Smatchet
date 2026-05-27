#!/usr/bin/env python3
"""Both-target syntax check for first-party Smatchet .cpp files.

Reads build/ninja-iter-msvc/compile_commands.json and runs every matching
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


# MSYS2 UCRT64 gcc (16.x as of 2026-05) requires its bin directory on PATH so
# cc1plus.exe can load libisl / libmpfr / libgmp / libgcc dynamically. Hook
# environments inherit the user's shell PATH, which on this project's hosts
# often does NOT include C:\msys64\ucrt64\bin (the build presets fix PATH
# internally via MSYSTEM_PREFIX). Without this prepend, the silent cc1plus
# DLL-load failure surfaces as an empty-diagnostic `[syntax-check FAIL]`.
_DEFAULT_TOOLCHAIN_BIN = r"C:\msys64\ucrt64\bin"


def _ensure_toolchain_on_path(env: dict) -> dict:
    bin_dir = os.environ.get("SMATCHET_TOOLCHAIN_BIN", _DEFAULT_TOOLCHAIN_BIN)
    if not bin_dir or not Path(bin_dir).is_dir():
        return env
    path = env.get("PATH", "")
    # Compare lower-cased + backslash-normalised so the prepend is idempotent
    # across forward-slash / backslash representations.
    norm_bin = bin_dir.replace("/", "\\").lower()
    segments = [seg for seg in path.split(os.pathsep) if seg]
    if any(seg.replace("/", "\\").lower() == norm_bin for seg in segments):
        return env
    env = dict(env)
    env["PATH"] = bin_dir + os.pathsep + path
    return env


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
    cc_path = project / "build" / "ninja-iter-msvc" / "compile_commands.json"
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

    env = _ensure_toolchain_on_path(os.environ.copy())

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
            env=env,
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
