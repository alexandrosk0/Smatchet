# `SMATCHET_DEVIATION` accepts free prose in `revisit=`, so a marker can never fall due

- **Category**: tooling
- **Priority**: P3
- **Date**: 2026-11-27
- **Observed on**: PR #2523 (merged 2026-11-27), the two duplication markers in `BuiltinCommands_Fields.cpp` (`revisit=when a shared command-registration builder lands`). The gate code review flagged it LOW.
- **Status**: open

## What happened

`deviation-malformed` checks that a marker is whole on one line with `rule`, `reason`, `owner` and `revisit`, but not what `revisit` holds. `deviation-overdue` can only fire on a date. A `revisit=` written as a condition is accepted and is never overdue, so the marker never comes back for review. Existing markers in the tree already use this form, so the new ones copied it.

## Proposed fix

Require `revisit=` to start with an ISO date (`YYYY-MM-DD`), optionally followed by the condition in prose (`revisit=2027-03-01 or when a shared command-registration builder lands`). Grandfather today's prose markers in a baseline and make the rule delta-gated, so only new markers must carry a date. Pin it in the lint bats suite with one dated and one prose marker.

**Enumerator + replay**: `grep -rn "SMATCHET_DEVIATION(" Source | grep -v "revisit=[0-9]\{4\}-"` lists the markers that can never fall due.
