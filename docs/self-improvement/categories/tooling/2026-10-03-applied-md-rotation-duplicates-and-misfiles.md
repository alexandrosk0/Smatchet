# `applied.md` rotation: the head duplicates its partitions, and per-entry blocks rotate under the wrong month

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-10-03
- **Observed on**: archiving the include-prologue tooling entry with
  `agents/scripts/core/archive-backlog-entry.sh` (the `dup_audit.py` prologue-filter PR)
- **Status**: open

## What happened

`archive-backlog-entry.sh` runs `rotate-applied-md.sh` after every append. On 2026-10-03 that
rotation turned a one-entry archival into a diff of about +3,600 / −6,300 lines across six files.
The diff looks like lost content but is not, for two reasons.

**The head duplicates its partitions.** `docs/self-improvement/categories/applied.md` still holds
248 of its 249 entries dated 2026-05 and all 268 dated 2026-06, and every one of them is already
present verbatim in `applied-2026-05.md` / `applied-2026-06.md`. The rotation's duplicate-drop
removes them from the head, which accounts for the line drop. Some earlier rotation wrote the
partitions without trimming the head, or a merge put the entries back.

**Per-entry blocks rotate under the wrong month.** The script splits entries with
`entry_re = ^- (\d{4})-(\d{2})-\d{2} `, which matches only the old monolith format. An entry
archived from a per-entry file starts with `# <title>` and carries its date as
`- **Date**: YYYY-MM-DD`, so it is glued onto the dated block above it and rotates with that one.
9 such blocks were in the head that day. The entry being archived, dated 2026-08-16, landed in
`applied-2026-07.md`.

A smaller follow-on: `archive-backlog-entry.sh` repoints inbound links at `applied.md`, and the
rotation then moves the entry into a partition. The link still resolves, but to a file that no
longer holds the entry.

## Why it matters

Every archival now drags a multi-thousand-line diff of unrelated history into whatever PR it lands
in. That burns review quota and makes a real loss impossible to spot by eye. The month partitions
are also unreliable to search, because per-entry blocks sit under whichever month preceded them.

The prologue-filter PR worked around it by archiving with a copy of the script outside
`agents/scripts/core/`: `ROTATE` is resolved next to the script, so the copy skips rotation. It
then ran `test-markdown-links.sh --all` itself.

## Concrete next action

1. Teach `rotate-applied-md.sh` both entry formats. A block starting `# ` begins an entry, and its
   date comes from its `- **Date**:` line.
2. Run the rotation once in a dedicated PR. Its description should give entry counts per month
   across all `applied*.md` before and after, showing that only duplicates disappeared.
3. Make inbound links follow the entry into its partition, or repoint them after rotating.
4. Add bats cases next to the existing archive/rotate tests: a per-entry block rotates by its own
   date, and the duplicate-drop never removes an entry whose only copy is in the head.

Status: open
Last-reviewed: 2026-10-03
