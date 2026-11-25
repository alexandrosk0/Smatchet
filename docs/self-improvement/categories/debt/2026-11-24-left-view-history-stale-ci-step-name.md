- 2026-11-24 · left-view-history-stale-ci-step-name · [debt] · P3 — An active plan quotes a CI step name that PR #2486 renamed

Details:
`docs/plans/active/left-view-history-grid.md` (§ Deviations, the "CI wiring for the bucket-E driver, landed late"
bullet) quotes the `.github/workflows/build-and-test.yml` step as "Run grid search + grid panes + left-view history
bucket-E (hard)". PR #2486 (Input UX Wave 2 create drafts, merged as 4eed00a06) added the data-dependent windows
driver to that step and renamed it to "Run grid search + grid panes + left-view history + data-dependent windows
bucket-E (hard)". That plan sits under another plan's lock, so it was not edited from here. Nothing breaks; a reader
searching the workflow for the quoted name finds no match.

Concrete next action: the next time `left-view-history-grid` updates its plan, change the quoted step name to the
current one (or drop the quote and name the driver `test-ui-left-view-history` only).

Status: open
Last-reviewed: 2026-11-25
