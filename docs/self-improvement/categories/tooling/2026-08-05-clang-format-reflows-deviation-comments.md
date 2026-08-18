# `clang-format` reflows a long `SMATCHET_DEVIATION` comment and silently breaks its parser

- **Category**: tooling
- **Priority**: P2
- **Date**: 2026-08-05
- **Status**: open — option 2 applied 2026-08-16, option 3 still outstanding

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

## Update — 2026-08-16 (deviation re-evaluation)

Measured rather than predicted. 73 live markers exceed `ColumnLimit`; 58 survive only because
someone hand-wrapped them in `// clang-format off` / `// clang-format on`. The remaining **15 are
rewritten by `clang-format` today** — 8 `duplication`, 4 `bare-json-parse-untrusted`, 3
`app-controller-fan-in`. Proof end-to-end on `Source/Core/src/Tracker/PlaneProjectScope.cpp` using
the real `scan_bare_json_parse_file`: in-tree → clean, after `clang-format` →
`bare-json-parse-untrusted`, gate FAILS. `scripts/dev/pre-ship.sh:429` runs `clang-format -i` on
every changed first-party TU before the gate, and no CI job checks formatting, so the drift is
invisible until someone touches one of those 12 files.

**Option 2 applied**: `.clang-format` now carries `CommentPragmas: '^ *SMATCHET_DEVIATION'`.
Verified across all 110 marker-holding TUs — marker lines clang-format would rewrite goes 15 → 0,
with no other formatting change attributable to the pragma.

**Option 3 deliberately deferred**, and the reason matters: 47 markers in the tree are *already*
wrapped and already invisible to every gate (see
[`2026-08-16-wrapped-deviation-markers-invisible-to-gate.md`](2026-08-16-wrapped-deviation-markers-invisible-to-gate.md)).
A wrapped-marker gate added today red-walls CI on all 47 at once. Sequence is: un-wrap the 47, then
add the gate. This entry stays open until option 3 lands.

Separately, the same audit found and fixed a second parser defect the original entry did not
anticipate: `DEV_RE`'s `[^)]*` body capture truncates at the first `)`, so a `reason=` containing a
parenthetical hid `revisit=` from `deviation-overdue` on 40 markers while still granting the
suppression — see [`docs/audits/DEVIATION_AUDIT_2026-08-16.md`](../../../audits/DEVIATION_AUDIT_2026-08-16.md) § S1.
