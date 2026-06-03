#!/usr/bin/env python3
"""Lightweight markdown style linter for first-party docs.

Closes the gap where markdown style issues (e.g. MD028 blockquote breaks) were
only ever surfaced by CodeRabbit's server-side review, never by a local/CI gate —
the markdown analogue of the C++ "build-green != lint-green" footgun. Wired into
scripts/dev/pre-ship.sh and the doc-validation CI job.

Curated rule set (extensible — start with the rules CodeRabbit actually flags):
  MD028 — no blank line inside a blockquote (a blank line both preceded AND
          followed by a `>` line breaks the blockquote in many renderers).

Usage:
  md_lint.py <path-or-dir>...      # lint the given files / dirs (recursive)
  md_lint.py --all                 # lint all tracked first-party *.md
  md_lint.py --selftest            # self-check the rule set

Exit codes: 0 clean, 1 violations found, 2 usage error.
"""
import os
import subprocess
import sys

EXCLUDE_SUBSTR = ("/ThirdParty/", "ThirdParty/", "/build/", ".fetchcontent-src/", "/node_modules/")


def tracked_markdown():
    out = subprocess.run(["git", "ls-files", "*.md"], capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    files = []
    for f in out.stdout.splitlines():
        if any(s in f for s in EXCLUDE_SUBSTR):
            continue
        files.append(f)
    return files


def check_md028(lines):
    """Return list of (line_no, msg) for blank lines inside a blockquote."""
    hits = []
    for i in range(1, len(lines) - 1):
        if lines[i].strip() == "" and lines[i - 1].lstrip().startswith(">") and lines[i + 1].lstrip().startswith(">"):
            hits.append((i + 1, "MD028 blank line inside blockquote (prefix with `>` or remove)"))
    return hits


RULES = [check_md028]


def lint_file(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            lines = fh.read().split("\n")
    except OSError:
        return []
    findings = []
    for rule in RULES:
        findings.extend(rule(lines))
    return sorted(findings)


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                if any(s in (root + "/") for s in EXCLUDE_SUBSTR):
                    continue
                for f in files:
                    if f.endswith(".md"):
                        out.append(os.path.join(root, f))
        elif p.endswith(".md"):
            out.append(p)
    return out


def selftest():
    good = ["> a", ">", "> b"]
    bad = ["> a", "", "> b"]
    assert check_md028(good) == [], "false positive on >-prefixed blank"
    assert check_md028(bad) == [(2, check_md028(bad)[0][1])], "missed MD028"
    print("md_lint: selftest OK (%d rule(s))" % len(RULES))


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    if argv[0] == "--selftest":
        selftest()
        return 0
    files = tracked_markdown() if argv[0] == "--all" else expand(argv)
    total = 0
    for f in sorted(set(files)):
        for ln, msg in lint_file(f):
            print("%s:%d: %s" % (f, ln, msg))
            total += 1
    if total:
        sys.stderr.write("md_lint: %d markdown violation(s)\n" % total)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
