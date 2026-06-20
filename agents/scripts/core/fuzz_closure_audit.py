#!/usr/bin/env python3
"""fuzz_closure_audit.py — WARN-first gate: every fuzz target's closure .cpp
first-hop quote-include must resolve against that target's declared INCLUDES.

Class of bug this catches
-------------------------
tests/fuzz/CMakeLists.txt declares each libFuzzer driver via
smatchet_add_fuzz_target(NAME SOURCES <driver.cpp> <closure .cpp ...>
INCLUDES <dir> ... LIBS ...). The driver + every closure .cpp is compiled with
ONLY those INCLUDES dirs on the search path (plus PUBLIC dirs propagated by any
LIBS). If a closure .cpp has a first-hop `#include "Foo.h"` whose header lives
under NO declared INCLUDES dir (and isn't satisfied by a LIBS target this gate
can't see), the Linux Clang fuzz build breaks — but ONLY in the
ninja-fuzzer-linux lane, which is advisory + nightly, so the breakage is
INVISIBLE at PR time on Windows. This static check surfaces an unresolved
first-hop include at lint time, before the nightly catches it.

WARN-first (calibration)
------------------------
Quote-include resolution is a heuristic: a header may legitimately be supplied
by a PUBLIC include dir propagated from a LIBS target (md4c, nlohmann) that this
gate cannot resolve without configuring CMake. To avoid false reds it is
WARN-first: it prints unresolved includes and the heuristic exit is 0 unless
run with --strict. The nightly fuzz-smoke.yml build remains the authoritative
backstop. Headers satisfied by a known LIBS target are allow-listed below.

Modes
-----
  (no arg) | --check   scan tests/fuzz/CMakeLists.txt; print unresolved
                       first-hop includes; exit 0 (WARN-first).
  --strict             same scan but exit 1 if any unresolved include remains.
  --list               print the parsed (target -> sources / includes) model.
  --selftest           dogfood: a synthetic target whose closure .cpp includes
                       a header outside its INCLUDES must be flagged; a clean
                       one must not. Asserts a failure case.

Exit: 0 clean (or WARN-first) · 1 unresolved (--strict) / selftest failure ·
2 infra error (cannot read the CMakeLists / no targets parsed).
"""
from __future__ import annotations

import os
import re
import sys
from pathlib import Path

# Headers a fuzz target may pull in via a LIBS target's PUBLIC include dir that
# this gate cannot resolve statically. A first-hop quote-include of one of these
# is NOT counted as unresolved. Keep narrow — only headers known to ship from a
# declared LIBS target in tests/fuzz/CMakeLists.txt.
_LIBS_PROVIDED = {
    # md4c (compiled C parser, propagates its PUBLIC include dir).
    "md4c.h",
    "md4c-html.h",
}

# Quote-include first-hop matcher: `#include "path/to/Header.h"`.
_QINCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"')

# smatchet_add_fuzz_target(NAME ...) call extractor. Captures the NAME token and
# the parenthesised argument body (balanced enough for this file's flat calls).
_CALL_RE = re.compile(
    r"smatchet_add_fuzz_target\s*\(\s*([A-Za-z0-9_]+)(.*?)\)\s*$",
    re.DOTALL | re.MULTILINE,
)


def _cmake_path() -> Path:
    root = Path(
        os.environ.get("CLAUDE_PROJECT_DIR") or os.getcwd()
    ).resolve()
    return root / "tests" / "fuzz" / "CMakeLists.txt"


def _expand_vars(token: str, cmake_vars: dict) -> str:
    """Expand ${var} references using a small set of locally-set CMake vars
    (notably ${_fuzz_core} and ${CMAKE_SOURCE_DIR})."""
    out = token
    for _ in range(8):  # bounded expansion
        m = re.search(r"\$\{([A-Za-z0-9_]+)\}", out)
        if not m:
            break
        name = m.group(1)
        out = out.replace(m.group(0), cmake_vars.get(name, ""))
    return out


def _parse_cmake_vars(text: str, project_root: Path) -> dict:
    """Collect set(VAR ...) values needed to resolve SOURCES paths. Only the
    single-token forms this file uses (_fuzz_core, _fuzz_logger_srcs)."""
    cmake_vars = {"CMAKE_SOURCE_DIR": str(project_root)}
    for m in re.finditer(r"set\s*\(\s*([A-Za-z0-9_]+)\s*(.*?)\)", text, re.DOTALL):
        name = m.group(1)
        body = m.group(2)
        toks = re.findall(r'"([^"]+)"|(\S+)', body)
        vals = [a or b for (a, b) in toks]
        # Expand inline now using what we already know.
        vals = [_expand_vars(v, cmake_vars) for v in vals]
        cmake_vars[name] = ";".join(vals) if len(vals) > 1 else (vals[0] if vals else "")
    return cmake_vars


def _split_args(body: str):
    """Split a smatchet_add_fuzz_target arg body into keyword sections.
    Returns dict section -> [tokens]. NAME is consumed by the caller."""
    toks = re.findall(r'"([^"]+)"|(\S+)', body)
    flat = [a or b for (a, b) in toks]
    sections: dict = {"SOURCES": [], "INCLUDES": [], "LIBS": [], "CORPUS": []}
    cur = None
    for t in flat:
        if t in sections:
            cur = t
            continue
        if cur is not None:
            sections[cur].append(t)
    return sections


def parse_targets(text: str, project_root: Path):
    """Return a list of dicts: {name, sources(abs Paths), includes(abs Paths)}.
    Expands ${_fuzz_core} / list vars (e.g. ${_fuzz_logger_srcs})."""
    cmake_vars = _parse_cmake_vars(text, project_root)
    targets = []
    for m in _CALL_RE.finditer(text):
        name = m.group(1)
        body = m.group(2)
        sec = _split_args(body)
        sources = []
        for s in sec["SOURCES"]:
            expanded = _expand_vars(s, cmake_vars)
            # A var may expand to a ;-list (e.g. _fuzz_logger_srcs).
            for piece in expanded.split(";"):
                piece = piece.strip()
                if piece.endswith(".cpp"):
                    sources.append(_to_abs(piece, project_root))
        includes = []
        for inc in sec["INCLUDES"]:
            expanded = _expand_vars(inc, cmake_vars)
            for piece in expanded.split(";"):
                piece = piece.strip()
                if piece:
                    includes.append(_to_abs(piece, project_root))
        targets.append({"name": name, "sources": sources, "includes": includes})
    return targets


def _to_abs(p: str, project_root: Path) -> Path:
    pp = Path(p)
    return pp if pp.is_absolute() else (project_root / pp)


def _first_hop_includes(cpp: Path):
    """Quote-form first-hop includes of a single .cpp (driver lives in
    tests/fuzz, so its own dir also resolves driver-local includes)."""
    out = []
    try:
        for line in cpp.read_text(encoding="utf-8", errors="replace").splitlines():
            m = _QINCLUDE_RE.match(line)
            if m:
                out.append(m.group(1))
    except OSError:
        pass
    return out


def _resolves(inc: str, search_dirs, own_dir: Path) -> bool:
    if Path(inc).name in _LIBS_PROVIDED:
        return True
    for d in list(search_dirs) + [own_dir]:
        if (d / inc).is_file():
            return True
    return False


def audit(targets):
    """Return list of (target, cpp_rel, include) for unresolved first-hop quote
    includes."""
    unresolved = []
    for t in targets:
        for cpp in t["sources"]:
            own_dir = cpp.parent
            for inc in _first_hop_includes(cpp):
                if not _resolves(inc, t["includes"], own_dir):
                    unresolved.append((t["name"], str(cpp), inc))
    return unresolved


def run_check(strict: bool) -> int:
    cm = _cmake_path()
    if not cm.is_file():
        print(f"fuzz_closure_audit: ERROR: {cm} not found", file=sys.stderr)
        return 2
    project_root = cm.parents[2]  # <root>/tests/fuzz/CMakeLists.txt
    text = cm.read_text(encoding="utf-8", errors="replace")
    targets = parse_targets(text, project_root)
    if not targets:
        print("fuzz_closure_audit: ERROR: no fuzz targets parsed (scan broken)",
              file=sys.stderr)
        return 2
    unresolved = audit(targets)
    if not unresolved:
        print(f"fuzz_closure_audit: PASS — all first-hop quote-includes across "
              f"{len(targets)} fuzz target(s) resolve against declared INCLUDES.")
        return 0
    tag = "FAIL" if strict else "WARN"
    print(f"fuzz_closure_audit: {tag} — unresolved first-hop quote-include(s) "
          f"(the ninja-fuzzer-linux build would break in the nightly lane):",
          file=sys.stderr)
    for name, cpp, inc in unresolved:
        rel = os.path.relpath(cpp, project_root)
        print(f"    [{name}] {rel}: #include \"{inc}\" — not under any declared "
              f"INCLUDES dir (nor a known LIBS-provided header)", file=sys.stderr)
    print("  Add the header's dir to that target's INCLUDES in "
          "tests/fuzz/CMakeLists.txt, or allow-list it if a LIBS target supplies "
          "it.", file=sys.stderr)
    return 1 if strict else 0


def run_list() -> int:
    cm = _cmake_path()
    if not cm.is_file():
        print(f"fuzz_closure_audit: ERROR: {cm} not found", file=sys.stderr)
        return 2
    project_root = cm.parents[2]
    targets = parse_targets(cm.read_text(encoding="utf-8", errors="replace"),
                            project_root)
    for t in targets:
        print(f"{t['name']}")
        for s in t["sources"]:
            print(f"  SRC  {os.path.relpath(s, project_root)}")
        for i in t["includes"]:
            print(f"  INC  {os.path.relpath(i, project_root)}")
    return 0


def run_selftest() -> int:
    import tempfile
    rc = 0
    with tempfile.TemporaryDirectory() as td:
        root = Path(td)
        core = root / "Source" / "Core"
        (core / "include" / "Ui").mkdir(parents=True)
        (core / "include" / "Privacy").mkdir(parents=True)
        (core / "src" / "Ui").mkdir(parents=True)
        (root / "tests" / "fuzz").mkdir(parents=True)

        # Header that DOES live under include/Ui — the clean target resolves it.
        (core / "include" / "Ui" / "Good.h").write_text("#pragma once\n")
        # A header that lives nowhere on the search path — the bad target fails.
        # (Privacy.h is under include/Privacy, but the bad target won't list it.)
        (core / "include" / "Privacy" / "Secret.h").write_text("#pragma once\n")

        # Clean closure .cpp — include resolves under include/Ui.
        (core / "src" / "Ui" / "Good.cpp").write_text('#include "Good.h"\n')
        # Bad closure .cpp — includes a header under include/Privacy, but its
        # target only declares include/Ui, so it must be flagged.
        (core / "src" / "Ui" / "Bad.cpp").write_text('#include "Secret.h"\n')

        (root / "tests" / "fuzz" / "fuzz_good.cpp").write_text(
            '#include "Ui/Good.h"\n')
        (root / "tests" / "fuzz" / "fuzz_bad.cpp").write_text(
            '#include "Ui/Good.h"\n')

        cmake = (
            'set(_fuzz_core "${CMAKE_SOURCE_DIR}/Source/Core")\n'
            "smatchet_add_fuzz_target(fuzz_good\n"
            "    SOURCES fuzz_good.cpp\n"
            '        "${_fuzz_core}/src/Ui/Good.cpp"\n'
            "    INCLUDES\n"
            '        "${_fuzz_core}/include"\n'
            '        "${_fuzz_core}/include/Ui"\n'
            "    CORPUS good)\n"
            "smatchet_add_fuzz_target(fuzz_bad\n"
            "    SOURCES fuzz_bad.cpp\n"
            '        "${_fuzz_core}/src/Ui/Bad.cpp"\n'
            "    INCLUDES\n"
            '        "${_fuzz_core}/include"\n'
            '        "${_fuzz_core}/include/Ui"\n'
            "    CORPUS bad)\n"
        )
        cm = root / "tests" / "fuzz" / "CMakeLists.txt"
        cm.write_text(cmake)

        targets = parse_targets(cmake, root)
        if len(targets) != 2:
            print(f"selftest FAIL — parsed {len(targets)} targets, expected 2",
                  file=sys.stderr)
            rc = 1
        unresolved = audit(targets)

        # selftest: asserts-failure — the Bad target's Secret.h must be flagged.
        flagged = {(n, inc) for (n, _c, inc) in unresolved}
        if ("fuzz_bad", "Secret.h") not in flagged:
            print("selftest FAIL — bad closure include 'Secret.h' was NOT flagged",
                  file=sys.stderr)
            rc = 1
        # The clean target must NOT be flagged.
        if any(n == "fuzz_good" for (n, _c, _i) in unresolved):
            print("selftest FAIL — clean target wrongly flagged", file=sys.stderr)
            rc = 1
        # md4c allow-list smoke: a header named md4c.h must resolve even with no
        # INCLUDES dir containing it.
        if not _resolves("md4c.h", [], root):
            print("selftest FAIL — LIBS-provided md4c.h not allow-listed",
                  file=sys.stderr)
            rc = 1

    if rc == 0:
        print("fuzz_closure_audit --selftest: PASS — bad closure include flagged, "
              "clean target clean, LIBS allow-list honoured.")
    return rc


def main() -> int:
    arg = sys.argv[1] if len(sys.argv) > 1 else "--check"
    if arg in ("--check",):
        return run_check(strict=False)
    if arg == "--strict":
        return run_check(strict=True)
    if arg == "--list":
        return run_list()
    if arg == "--selftest":
        return run_selftest()
    if arg in ("-h", "--help"):
        print("usage: fuzz_closure_audit.py [--check|--strict|--list|--selftest]")
        return 0
    print("usage: fuzz_closure_audit.py [--check|--strict|--list|--selftest]",
          file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
