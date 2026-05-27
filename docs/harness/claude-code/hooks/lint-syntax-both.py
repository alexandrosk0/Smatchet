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
    # Prefer ninja-iter-clang: clang-cl knows its own system headers and supports
    # -fsyntax-only natively. ninja-iter-msvc requires a VS Developer Environment
    # for cl.exe to resolve standard headers, which hook processes don't have.
    for preset in ("ninja-iter-clang", "ninja-iter-msvc"):
        cc_path = project / "build" / preset / "compile_commands.json"
        if cc_path.is_file():
            break
    else:
        return 0  # no build configured yet — degrade silently

    try:
        entries = json.loads(cc_path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return 0

    src_key = _norm(src)
    matches = [e for e in entries if _norm(e.get("file", "")) == src_key]
    if not matches:
        return 0  # file not in compile DB yet (e.g. just added) — skip

    env = _ensure_toolchain_on_path(os.environ.copy())

    # Lines matching these patterns are always false positives in the hook
    # environment: PCH built by a different toolchain / not present, or system
    # headers unreachable without the VS Developer shell. cmake --build catches
    # real compile errors; this hook only surfaces first-party diagnostics.
    _FP_PATTERNS = re.compile(
        r"(Cannot open precompiled header|"
        r"PCH file .* not found|"
        r"PCH file .* built from a different branch|"
        r"Cannot open include file: '(?:cstdint|algorithm|string|vector|memory|"
        r"functional|utility|type_traits|cassert|cstring|cstdlib|cstdio|cmath|"
        r"climits|cwchar|stdexcept|iostream|sstream|fstream|chrono|mutex|thread|"
        r"atomic|tuple|array|map|set|unordered_map|unordered_set|deque|list|"
        r"iterator|numeric|bitset|locale|codecvt|regex|random|complex|valarray)')"
    )

    failures: list[tuple[str, str]] = []
    for entry in matches:
        cmd = entry["command"]
        # Strip output flags so syntax-only doesn't write object files.
        cmd = re.sub(r"-o\s+\S+\.obj", "", cmd)
        cmd = re.sub(r"/Fo\S+", "", cmd)
        cmd = re.sub(r"/Fd\S+", "", cmd)
        # MSVC cl.exe uses /Zs (appended); clang-cl uses -fsyntax-only inserted
        # before the `--` separator (if present) so it is not mistaken for a
        # filename. Remove -c as well since -fsyntax-only implies no codegen.
        exe_lower = cmd.split()[0].lower().replace("\\", "/")
        if exe_lower.endswith("cl.exe") and "clang" not in exe_lower:
            cmd += " /Zs"
        elif " -- " in cmd:
            cmd = re.sub(r"\s+-c\b", "", cmd)
            cmd = cmd.replace(" -- ", " -fsyntax-only -- ", 1)
        else:
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
            # Filter false-positive lines; only report if real diagnostics remain.
            real_lines = [ln for ln in diag.splitlines() if not _FP_PATTERNS.search(ln)]
            if real_lines:
                failures.append((target, "\n".join(real_lines)))

    if failures:
        for target, diag in failures:
            sys.stderr.write(f"[syntax-check FAIL: {target}]\n{diag}\n\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
