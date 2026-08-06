# `clang-format` reflows a long `SMATCHET_DEVIATION` comment and silently breaks its parser

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-05
- **Status**: open

## What happened

`.clang-format` sets `ColumnLimit: 120`. A `SMATCHET_DEVIATION(rule=…; reason=…; owner=…;
revisit=…)` comment with a descriptive `reason=` exceeds that, so `clang-format -i` wraps it
onto a second `//` line. Every deviation consumer (`dup_audit.py`, `test-lint-rules.sh`, the
`deviation-overdue` gate) matches the directive on a **single line**, so the wrapped form is
not a syntax error — it simply stops being a deviation, and the rule it was escaping fires
again with no explanation of why the comment above it exists.

Hit while adding the four duplication exemptions for the window-expand feature: the reason
strings had to be shortened to fit rather than written for the reader.

## Why it matters

Two gates disagree about the same line — the formatter, which every pre-push hook runs, and
the lint gates, which block the merge. The failure is silent in the direction that matters
(escape lost, not escape wrongly granted), and the fix pressure lands on comment prose
instead of on the tooling.

## Proposed fix

Pick one:

1. Teach the deviation parsers to join a `//` continuation line before matching, so wrapping
   is harmless. Cheapest, keeps `ColumnLimit` untouched.
2. Add `CommentPragmas: '^ SMATCHET_DEVIATION'` to `.clang-format` so the formatter leaves
   these comments alone. One line, but the long comment then visibly overruns the column
   limit.
3. Add a gate that fails on a wrapped `SMATCHET_DEVIATION(` with no closing `)` on the same
   line — turns the silent loss into a loud one without changing either tool's behaviour.

Option 2 plus option 3 is the smallest combination that is both correct and self-policing.
