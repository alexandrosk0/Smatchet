#!/usr/bin/env python3
"""test_doc_anchors.py — verify every `AGENTS.md § <section>` reference resolves.

Problem (process.md 2026-05-19 orchestrator P3 — cross-link rot):
Across the repo, ~74 files reference AGENTS.md sections by name. The
redirect-stub pattern from PR #273 (AGENTS.md § Delegation lifted to
docs/agent-rules/delegation.md) is convention-only — nothing automated
catches a typo or stale anchor today.

Strategy:
  1. Collect the valid anchor set = headings (## / ###) + bold-prefix
     paragraphs (^**Name**:) from AGENTS.md and every docs/agent-rules/*.md.
  2. Walk the repo, find every `AGENTS.md § <name>` reference, extract
     <name> conservatively (stop at sentence punctuation / closing bracket).
  3. For each reference, find the longest anchor that is a substring of
     the extracted name. If no anchor matches, the reference is broken.
  4. Skip vendored / generated / archive paths.

Bucket A (CLI). Zero manual steps. Invoked from scripts/dev/test-doc-anchors.sh.
"""

from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[3]

# Files searched for `AGENTS.md §` references — limit to source / docs / scripts.
SCAN_GLOBS = (
    "*.md",
    "*.sh",
    "*.py",
    "*.h",
    "*.cpp",
    "*.hpp",
    "*.json",
    "*.yaml",
    "*.yml",
)

# Paths excluded from the reference scan (vendored / archives / generated).
EXCLUDE_GLOBS = (
    ":(exclude)build/**",
    ":(exclude).fetchcontent-src/**",
    ":(exclude).claude/worktrees/**",
    ":(exclude)docs/plans/active/_plan-locks-archive.md",
    ":(exclude)docs/plans/active/_plan-locks.generated.md",
    ":(exclude)docs/self-improvement/categories/applied.md",
    # Rotated monthly partitions of applied.md (rotate-applied-md.sh): the same
    # archive, so the same stale-section-reference expectation applies.
    ":(exclude)docs/self-improvement/categories/applied-*.md",
)

# Anchor sources — AGENTS.md + every file in docs/agent-rules/.
ANCHOR_FILES = [REPO_ROOT / "AGENTS.md"] + sorted(
    (REPO_ROOT / "docs" / "agent-rules").glob("*.md")
)

REFERENCE_RE = re.compile(r"AGENTS\.md\s*§\s*(.+)")

# Terminators that end a section-name reference: opening + closing punctuation,
# quotes, brackets, markdown-table pipes. Section names never contain these.
TERMINATOR_CHARS = re.compile(r"[\[\].,;:()+\"`<>|]")

# Connective words that often appear right after a section name in prose —
# trim them off so "X gains Y" becomes "X" alone.
# Note: words like `note`, `rule`, `comment` are NOT in this list because they
# appear in legitimate section names (`Anti-deception note`, `Post-split
# include-replication rule`, etc.).
CONNECTIVE_TAIL_RE = re.compile(
    r"\s+(?:and|or|to|for|if|when|per|so|under|above|below|after|before|with|"
    r"from|into|on|in|of|the|that|this|which|since|until|see|gains?|gained|"
    r"encoded?|documents?|documented?|covered?|adds?|extends?|naming|states?|"
    r"stating|lists?|listing|paragraph|sub-section|invocation|"
    r"here|directly|now|via|already|"
    r"alone|exists|missing|empty|present|absent|first|then|next|last|also|"
    r"only|too|either|both)\b.*$"
)


def collect_anchors() -> list[str]:
    """Return the union of headings + bold-prefix paragraph names."""
    anchors: set[str] = set()
    heading_re = re.compile(r"^\s{0,3}#{1,6}\s+(.+?)\s*$", re.MULTILINE)
    # Bold-prefix paragraphs — match `**Name**` at the start of a line / bullet /
    # numbered item. Tolerates trailing ` (...)` / ` —` / `:` because AGENTS.md
    # uses `**Available libs** (FetchContent, linked):` style.
    bold_prefix_re = re.compile(
        r"^\s*(?:[-*]|\d+\.)?\s*\*\*([A-Z][^*]+?)\*\*", re.MULTILINE
    )
    for f in ANCHOR_FILES:
        try:
            text = f.read_text(encoding="utf-8")
        except FileNotFoundError:
            continue
        for m in heading_re.finditer(text):
            heading = m.group(1).strip()
            # Strip trailing comment tokens like `<!-- tier: portable -->`.
            heading = re.sub(r"\s*<!--.*?-->\s*$", "", heading).strip()
            if heading:
                anchors.add(heading)
        for m in bold_prefix_re.finditer(text):
            name = m.group(1).strip()
            if name:
                anchors.add(name)
    return sorted(anchors, key=len, reverse=True)  # Longest first for greedy match.


def collect_references() -> list[tuple[str, str, int]]:
    """Return [(extracted_name, file_path, line_number), ...]."""
    cmd = [
        "git",
        "grep",
        "-n",
        "-E",
        r"AGENTS\.md\s*§\s*[A-Za-z]",
        "--",
        *SCAN_GLOBS,
        *EXCLUDE_GLOBS,
    ]
    try:
        out = subprocess.check_output(cmd, cwd=REPO_ROOT, text=True, encoding="utf-8")
    except subprocess.CalledProcessError as exc:
        if exc.returncode == 1:
            return []  # No matches.
        raise
    refs: list[tuple[str, str, int]] = []
    for line in out.splitlines():
        # Format: <file>:<line>:<content>
        try:
            file_path, line_no_str, content = line.split(":", 2)
            line_no = int(line_no_str)
        except ValueError:
            continue
        match = REFERENCE_RE.search(content)
        if not match:
            continue
        raw = match.group(1)
        # Stop at the first terminator character.
        term = TERMINATOR_CHARS.search(raw)
        if term:
            raw = raw[: term.start()]
        # Trim connective tail words like "gains a", "encoded as", etc.
        raw = CONNECTIVE_TAIL_RE.sub("", raw)
        # Final trim.
        name = raw.strip()
        if name:
            refs.append((name, file_path, line_no))
    return refs


def resolves(name: str, anchors: list[str]) -> str | None:
    """Return the matching anchor (longest), or None if no match."""
    # Direct match — name IS an anchor.
    if name in anchors:
        return name
    # Substring match — an anchor is a prefix / inside the extracted name.
    # Anchors are sorted longest-first so we get the most specific hit.
    for anchor in anchors:
        if anchor in name:
            return anchor
    # Reverse substring — extracted name is shorter than an anchor (partial).
    for anchor in anchors:
        if name in anchor:
            return anchor
    return None


def main() -> int:
    anchors = collect_anchors()
    refs = collect_references()

    # Dedup the references by extracted name to keep the output compact while
    # preserving one example file:line per broken name.
    by_name: dict[str, tuple[str, int]] = {}
    for name, file_path, line_no in refs:
        by_name.setdefault(name, (file_path, line_no))

    failed: list[tuple[str, str, int]] = []
    resolved_count = 0
    for name, (file_path, line_no) in sorted(by_name.items()):
        if resolves(name, anchors):
            resolved_count += 1
        else:
            failed.append((name, file_path, line_no))

    total = len(by_name)
    print("test-doc-anchors — cross-link audit for AGENTS.md sections")
    print(f"  Anchor source: AGENTS.md + {len(ANCHOR_FILES) - 1} file(s) under docs/agent-rules/")
    print(f"  Distinct anchors:       {len(anchors)}")
    print(f"  Distinct references:    {total}")
    print(f"  Resolved:               {resolved_count}")
    print(f"  Broken:                 {len(failed)}")

    if failed:
        print()
        print(f"FAILED — {len(failed)} reference(s) name a section that doesn't")
        print("match any anchor in AGENTS.md or docs/agent-rules/*.md:")
        for name, file_path, line_no in failed:
            print(f"  - {name!r}")
            print(f"      first occurrence: {file_path}:{line_no}")
        print()
        print("Fix options:")
        print("  1. Add the section to AGENTS.md or docs/agent-rules/<X>.md.")
        print("  2. Rename the reference to match an existing section name.")
        print("  3. Update AGENTS.md redirect stub to mention the moved name.")
        return 1

    print()
    print("PASS — every AGENTS.md § <section> reference resolves.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
