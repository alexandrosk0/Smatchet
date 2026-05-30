#!/usr/bin/env python3
"""Deterministic mechanical comment stripper — Wave 1 of reduce-source-comment-bloat.

Removes ONLY the unambiguous *cut* buckets, and only on `//`-style line comments that occupy
a whole line: blank comment lines (`//` alone) and decorative banners/dividers
(`// =====`, `// 3. THE REST OF YOUR INCLUDES`). It deliberately does NOT:
  - touch `/* */` or `///` / `/** */` doc bodies or block-continuation (`*`) lines (Wave-2/LLM only),
  - touch trailing comments that share a line with code (could drop a lint-exemption marker),
  - delete commented-out code (Wave 1 only flags it; Wave 2 deletes by judgment),
  - touch anything matching the hardcoded protect-list.

The `assert-code-unchanged.sh` gate is the backstop: it proves the edit changed only comments.

Usage:
  comment_strip.py --dry-run <paths...>   # unified diff, no writes (default if no --apply)
  comment_strip.py --apply   <paths...>   # rewrite in place
Paths may be files or dirs (dirs walked for first-party C++).
"""

import argparse
import difflib
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import comment_lib as cl
import comment_audit as audit

STRIP_BUCKETS = ("cut-blank", "cut-decorative")


def strip_file_text(text):
    """Return (new_text, removed_line_numbers). Removes only safe `//`-line cut buckets."""
    kinds = cl.classify_line_kinds(text)
    had_final_nl = text.endswith("\n")
    lines = text.split("\n")
    if had_final_nl:
        lines = lines[:-1]  # drop the empty trailing element from the final newline
    keep = []
    removed = []
    for idx, line in enumerate(lines):
        kind = kinds[idx] if idx < len(kinds) else "code"
        drop = False
        if kind == "full_comment":
            stripped = line.strip()
            if stripped.startswith("//"):  # line-comment only — never block bodies
                bucket = audit.classify_comment(stripped, line)
                if bucket in STRIP_BUCKETS:
                    drop = True
        if drop:
            removed.append(idx + 1)
        else:
            keep.append(line)
    new_text = "\n".join(keep)
    if had_final_nl:
        new_text += "\n"
    return new_text, removed


def _in_scope(path):
    """Only sweep first-party C++ under the audited roots; never ThirdParty. Guards against
    `comment_strip.py --apply .` rewriting files outside the intended sweep scope."""
    rel = os.path.relpath(path).replace("\\", "/")
    if any(s in "/" + rel for s in audit.EXCLUDE_SUBSTR):
        return False
    return rel.startswith(audit.SWEEP_ROOTS) and rel.endswith(audit.CPP_EXT)


def iter_target_files(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _, names in os.walk(p):
                for nm in names:
                    cand = os.path.join(root, nm).replace("\\", "/")
                    if _in_scope(cand):
                        yield cand
        elif _in_scope(p):
            yield p
        else:
            print(f"comment_strip: skipping out-of-scope path {p}", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--apply", action="store_true", help="rewrite files in place")
    ap.add_argument("--dry-run", action="store_true", help="show diff only (default)")
    args = ap.parse_args()
    apply = args.apply and not args.dry_run

    total_removed = 0
    files_changed = 0
    for f in iter_target_files(args.paths):
        try:
            with open(f, "r", encoding="utf-8", errors="replace", newline="") as fh:
                text = fh.read()
        except OSError:
            continue
        new_text, removed = strip_file_text(text)
        if not removed:
            continue
        files_changed += 1
        total_removed += len(removed)
        if apply:
            with open(f, "w", encoding="utf-8", newline="") as fh:
                fh.write(new_text)
            print(f"stripped {len(removed)} comment line(s): {f}")
        else:
            diff = difflib.unified_diff(text.splitlines(True), new_text.splitlines(True),
                                        fromfile=f, tofile=f + " (stripped)")
            sys.stdout.writelines(diff)
    print(f"\n{'APPLIED' if apply else 'DRY-RUN'}: {total_removed} comment line(s) across "
          f"{files_changed} file(s).", file=sys.stderr)


if __name__ == "__main__":
    main()
