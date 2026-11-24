- 2026-11-24 · left-view-history-stale-ci-step-name · [debt] · P3 — An active plan will quote a CI step name that PR #2486 renames

Details:
`docs/plans/active/left-view-history-grid.md` (§ Deviations, the "CI wiring for the bucket-E driver, landed late"
bullet) quotes the `.github/workflows/build-and-test.yml` step as "Run grid search + grid panes + left-view history
bucket-E (hard)". PR #2486 (Input UX Wave 2 create drafts, open when this was filed) adds the data-dependent
windows driver to that step and renames it to "Run grid search + grid panes + left-view history + data-dependent
windows bucket-E (hard)". Once #2486 merges, the quote no longer matches the workflow. That plan sits under another
plan's lock, so it was not edited from here. Nothing breaks; a reader searching the workflow for the quoted name
finds no match.

Concrete next action: if #2486 has merged, change the quoted step name the next time `left-view-history-grid`
updates its plan (or drop the quote and name the driver `test-ui-left-view-history` only). If #2486 closed
unmerged, delete this entry.

Status: open
Last-reviewed: 2026-11-24
