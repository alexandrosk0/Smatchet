# The "Issues That Left Views" window has no screenshot golden

- **Category**: test
- **Priority**: P2
- **Date**: 2026-11-09
- **Observed on**: the left-view history grid (`docs/plans/active/left-view-history-grid.md`, § Verification "Manual residue").
- **Status**: open

## What happened

The bucket-E test `LeftViewHistory` checks the window's rows, actions and two-step clear against `tests/fixtures/jira_backend/left-view-history.json`, but nothing checks how it looks: column widths, the reason tooltip, the details pane, and the French strings. That is left to a user's eye, which the Visual-validation exception allows once but not as the standing check.

## Concrete next action

Add a bucket-C screenshot scenario that opens the window filled from `left-view-history.json` (rows settled, one row selected with details open) and approve its golden per `golden-image-approval.md`. The golden needs the user's sign-off, so it is not created automatically.
