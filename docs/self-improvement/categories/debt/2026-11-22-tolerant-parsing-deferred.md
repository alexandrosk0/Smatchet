- 2026-11-22 · input-ux-w2-tolerant-parsing · [debt] · P3 — Tolerant parsing leftovers: stale required-field errors, Quick Create's disabled reason, p4 path probes and a bare-number preference

Details:
`docs/plans/input-ux-w2-tolerant-parsing.md` (Wave 2 piece 4.5, PR #2441) left these out; the files that held the
first two are no longer locked:

- **#53, required-field errors go stale** in the new-issue row (`SmatchetNewIssueDraftUi.cpp`): an error stays
  after the field is filled.
- **#58, Quick Create gives no reason it is disabled** (`SmatchetQuickCreateIssueUi.cpp`): the disabled Create
  button should say which field is missing.
- **#82 / #128, p4 path checks:** a file-existence probe, Browse, Detect and Test (`p4 -V`) for the p4 paths. Each
  needs a worker-thread probe (Pillar 2) and a native file-dialog seam; the PR did the trim, unquoting and shape
  checks only.
- **Bare-number unit preference (#38):** `BareNumberUnit` is a parser parameter, and every caller passes
  `Minutes`; a Preferences row would let a user pick hours.

Already tracked elsewhere: the inline line under the grid search box (#63) is in
`docs/plans/input-ux-w3-filter-without-jql.md`; the annotate checked-field UI cases are
`test/2026-11-18-annotate-checked-field-ui-cases.md`.

Concrete next action:
#53 and #58 in one PR with bucket-E cases next to the new-issue and Quick Create tests; the p4 probes as their own
PR (worker thread, no UI-thread `p4`); the bare-number preference when a user asks for it.

Status: open
Last-reviewed: 2026-11-22
