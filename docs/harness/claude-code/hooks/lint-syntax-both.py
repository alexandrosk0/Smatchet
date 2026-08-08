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


# Lines matching these patterns are always false positives in the hook
# environment: PCH built by a different toolchain / not present / version-drift,
# or system headers unreachable without the VS Developer shell. cmake --build
# catches real compile errors; this hook only surfaces first-party diagnostics.
_FP_PATTERNS = re.compile(
    r"(Cannot open precompiled header|"
    r"PCH file .* not found|"
    r"PCH file .* built from a different branch|"
    # PCH version-drift diagnostics: the cached PCH was emitted by a different
    # MSVC toolset than the one resolving on the hook's PATH (a side-by-side VS
    # toolset bump). MSVC C1853 / "Version differs in precompiled file" and the
    # clang/MSVC "was compiled for the target" message are toolchain-skew noise,
    # never a first-party source error — cmake --build with the matching toolset
    # is the authority.
    r"Microsoft Visual C/C\+\+ Version differs in precompiled file|"
    r"was compiled for the target|"
    r"Cannot open include file: '(?:cstdint|algorithm|string|vector|memory|"
    r"functional|utility|type_traits|cassert|cstring|cstdlib|cstdio|cmath|"
    r"climits|cwchar|stdexcept|iostream|sstream|fstream|chrono|mutex|thread|"
    r"atomic|tuple|array|map|set|unordered_map|unordered_set|deque|list|"
    r"iterator|numeric|bitset|locale|codecvt|regex|random|complex|valarray|"
    r"limits|ctime|iomanip|initializer_list|exception|new|typeinfo|"
    r"condition_variable|future|forward_list|stack|queue|ratio|system_error)')"
)


def _selftest() -> int:
    """Assert the FP-line filter classifies known toolchain-skew diagnostics as
    false positives and real first-party errors as non-FP. Run via --selftest.
    """
    fp_lines = [
        # PCH version-drift (the two patterns added by PR-6).
        r"foo.cpp(1): fatal error C1853: 'x.pch' precompiled header file is "
        r"from a previous version of the compiler, or the precompiled header "
        r"is C++ and you are using it from C (Microsoft Visual C/C++ Version "
        r"differs in precompiled file)",
        r"error: PCH file 'x.pch' was compiled for the target "
        r"'x86_64-pc-windows-msvc' but the current translation unit ...",
        # Pre-existing FP classes (regression guard).
        "fatal error: Cannot open precompiled header file: 'x.pch'",
        "error: PCH file 'core.pch' not found",
        "fatal error C1083: Cannot open include file: 'vector': No such file",
        # <limits> reached the allow-list late: without it every syntax-check of
        # a TU transitively including AppController.h reported a failure whose
        # only surviving line was cl.exe's filename banner.
        "AppController.h(5): fatal error C1083: Cannot open include file: "
        "'limits': No such file or directory",
    ]
    real_lines = [
        r"AppController.cpp(42): error C2065: 'undeclared_symbol': "
        r"undeclared identifier",
        r"Tracker.cpp:10:5: error: use of undeclared identifier 'foo'",
        # A first-party header that is NOT in the system-header allow-list must
        # remain a real error (don't over-broaden the include FP).
        r"error: Cannot open include file: 'JiraClient.h'",
    ]
    rc = 0
    for ln in fp_lines:
        if not _FP_PATTERNS.search(ln):
            sys.stderr.write(f"selftest FAIL — expected FP, not matched:\n  {ln}\n")
            rc = 1
    for ln in real_lines:
        if _FP_PATTERNS.search(ln):
            sys.stderr.write(f"selftest FAIL — real error wrongly matched as FP:\n  {ln}\n")
            rc = 1
    if rc == 0:
        print("lint-syntax-both --selftest: PASS — "
              f"{len(fp_lines)} FP + {len(real_lines)} real lines classified correctly.")
    return rc


def main() -> int:
    if len(sys.argv) < 2:
        return 0
    if sys.argv[1] == "--selftest":
        return _selftest()

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
            # cl.exe echoes the TU's bare filename on its own line before any
            # diagnostics — never a diagnostic itself. Left unfiltered it is the
            # residue that turns every fully-false-positive run into a reported
            # failure with an empty body (the whole file's real lines get FP-
            # filtered, the banner survives, `real_lines` is truthy).
            real_lines = [
                ln for ln in diag.splitlines()
                if ln.strip() != src.name and not _FP_PATTERNS.search(ln)
            ]
            if real_lines:
                failures.append((target, "\n".join(real_lines)))

    if failures:
        for target, diag in failures:
            sys.stderr.write(f"[syntax-check FAIL: {target}]\n{diag}\n\n")
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
