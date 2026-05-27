#!/usr/bin/env python3
"""Lint check for silent catch-all blocks in first-party C++ files.

Scans a single file for `catch (...)` blocks that are empty or lack a LOG_
call, unless suppressed by `// catch-all-ok:`. Designed to run inline from
the lint-cpp hook pipeline.

Exit 0 = clean or not applicable.
Exit 2 = findings (diagnostics on stderr, matches lint-cpp convention).

Usage: python lint-catch-all.py <file>
"""
from __future__ import annotations

import re
import sys
from pathlib import Path


_CATCH_ALL_RE = re.compile(r"catch\s*\(\s*\.\.\.\s*\)")
_LOG_RE = re.compile(r"\bLOG_(TRACE|DEBUG|INFO|WARN|ERROR)\b")
_SUPPRESS_RE = re.compile(r"//\s*catch-all-ok:")


def _scan_file(path: Path) -> list[str]:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return []

    findings: list[str] = []
    i = 0
    while i < len(lines):
        line = lines[i]
        if not _CATCH_ALL_RE.search(line):
            i += 1
            continue

        if _SUPPRESS_RE.search(line):
            i += 1
            continue

        brace_depth = 0
        body_start = -1
        body_lines: list[str] = []
        found_open = False

        for j in range(i, min(i + 30, len(lines))):
            for ch in lines[j]:
                if ch == "{":
                    if not found_open:
                        found_open = True
                        body_start = j
                    if found_open:
                        brace_depth += 1
                elif ch == "}" and found_open:
                    brace_depth -= 1

            if found_open and body_start >= 0 and j > body_start:
                body_lines.append(lines[j])

            if found_open and brace_depth == 0:
                break

        if not found_open:
            i += 1
            continue

        body_text = "\n".join(body_lines)

        suppressed = any(_SUPPRESS_RE.search(bl) for bl in body_lines)
        if suppressed:
            i += 1
            continue

        body_stripped = body_text.strip().rstrip("}")
        if not body_stripped or body_stripped.isspace():
            findings.append(
                f"{path}:{i + 1}: [error] empty catch(...) block — "
                f"add LOG or // catch-all-ok: <reason>"
            )
        elif not _LOG_RE.search(body_text):
            findings.append(
                f"{path}:{i + 1}: [warning] catch(...) without LOG_ call — "
                f"add LOG or // catch-all-ok: <reason>"
            )

        i += 1

    return findings


def main() -> int:
    if len(sys.argv) < 2:
        return 0

    path = Path(sys.argv[1])
    if path.suffix.lower() not in (".cpp", ".h"):
        return 0

    findings = _scan_file(path)
    if findings:
        for f in findings:
            sys.stderr.write(f + "\n")
        return 2

    return 0


if __name__ == "__main__":
    sys.exit(main())
