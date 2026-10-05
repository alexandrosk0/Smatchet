#!/usr/bin/env python3
"""Entry splitter shared by rotate-applied-md.sh and sort-applied-md.sh.

`applied.md` and its `applied-YYYY-MM.md` partitions hold two entry shapes:

  * legacy list entries: a column-0 `- YYYY-MM-DD · …` line plus everything up
    to the next entry;
  * per-entry-file blocks, appended verbatim by archive-backlog-entry.sh: a
    `# ` (occasionally `## `) title heading whose first paragraph is the entry's
    own metadata, either `**Date**:` / `**Date:**` / `**Filed**:` /
    `**Raised**:` fields (bare or as `- ` list items, alone or on a shared
    `**Category**: … · **Filed**: …` line), or a legacy `- YYYY-MM-DD · …`
    summary line.

A heading opens an entry only when that first paragraph is metadata. Section
headings inside an entry (`## Problem`), a file's own title and the `## Parked`
divider have prose under them, so they stay glued to the block they sit in. A
`# comment` inside a fenced code block is never a title. A legacy line always
opens an entry, exactly as the legacy-only splitter did, unless it is the
summary line of the heading directly above it.

A heading entry whose metadata carries no date is returned with date None;
callers decide what an undated entry does (rotation keeps it in the head, the
sort keeps it behind its predecessor).
"""

import re

LEGACY_RE = re.compile(r"^- (\d{4})-(\d{2})-(\d{2}) ")
HEADING_RE = re.compile(r"^#{1,2} \S")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
# `**Date**:` and `**Date:**` both occur in archived per-entry files.
_FIELD = r"\*\*(?:{names})(?::\*\*|\*\*:)"
META_DATE_RE = re.compile(_FIELD.format(names="Date|Filed|Raised") + r"\s*(\d{4})-(\d{2})-(\d{2})")
META_ANY_RE = re.compile(_FIELD.format(names="Category|Priority|Date|Filed|Raised"))


def _first_paragraph(lines, i):
    """Index of the first non-blank line after heading i, and its paragraph."""
    n = len(lines)
    j = i + 1
    while j < n and not lines[j].strip():
        j += 1
    k = j
    while k < n and lines[k].strip() and not HEADING_RE.match(lines[k]):
        k += 1
    return j, lines[j:k]


def _heading_entry(lines, i):
    """(date, absorbed_line_index) when heading i opens an entry, else None.

    date is "YYYY-MM-DD" or None (metadata without a date). The absorbed index
    is the legacy summary line that belongs to this heading, or -1.
    """
    j, para = _first_paragraph(lines, i)
    if not para:
        return None
    m = LEGACY_RE.match(para[0])
    if m:
        return "-".join(m.groups()), j
    if not any(META_ANY_RE.search(line) for line in para):
        return None
    for line in para:
        d = META_DATE_RE.search(line)
        if d:
            return "-".join(d.groups()), -1
    return None, -1


def split_entries(lines):
    """Split a ledger into (header_lines, [(date_or_None, block_lines), ...]).

    The header is every line before the first entry. Each block keeps its
    lines verbatim, so joining header + blocks reproduces the input exactly.
    """
    header, entries = [], []
    cur, cur_date = None, None
    in_fence = False
    absorbed = -1
    for idx, line in enumerate(lines):
        opens, date = False, None
        legacy = LEGACY_RE.match(line) if idx != absorbed else None
        if legacy:
            # A column-0 dated list line is an entry wherever it appears, as it
            # was for the legacy-only splitter; it also ends any fence left open.
            opens, date = True, "-".join(legacy.groups())
            in_fence = False
        elif idx != absorbed and not in_fence and HEADING_RE.match(line):
            found = _heading_entry(lines, idx)
            if found is not None:
                opens, (date, absorbed) = True, found
        if opens:
            if cur is not None:
                entries.append((cur_date, cur))
            cur, cur_date = [line], date
        elif cur is None:
            header.append(line)
        else:
            cur.append(line)
        if FENCE_RE.match(line):
            in_fence = not in_fence
    if cur is not None:
        entries.append((cur_date, cur))
    return header, entries


def block_key(block):
    """Identity of a block for duplicate detection (trailing blanks ignored)."""
    return "".join(block).rstrip()


def sort_latest_first(entries):
    """Stable latest-first order of (date, block) pairs.

    An undated block sorts under its predecessor's date, so it keeps its place
    directly behind that block instead of sinking to one end of the file.
    """
    keyed, last = [], "9999-99-99"
    for date, block in entries:
        if date is not None:
            last = date
        keyed.append((last, date, block))
    # reverse=True keeps the sort stable: equal dates keep their file order.
    keyed.sort(key=lambda e: e[0], reverse=True)
    return [(date, block) for _, date, block in keyed]
