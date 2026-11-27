- 2026-11-19 · input-ux-w2-one-matcher · [debt] · P2 — The shared matcher's highlight, grid cue and remap preview stop at locked or busy files

Details:
Input UX piece 4.7 (`docs/plans/input-ux-w2-one-matcher.md`) routed the pickers, filters and the grid
search through `TextMatchPure.h` and drew the matched letters in the shared dropdown and the command
palette (`Ui/SmatchetMatchHighlight.h`). Some of the audit's asks sit in files another branch held when it
shipped, so they were left out rather than overriding a plan-lock or racing an open PR:

- **Highlight in grid cells.** Grid cells do not mark the search words (a per-cell draw change, measured under
  Pillar 1 first). The views list landed 2026-11-20 (input UX Wave 2 lock-deferrals PR: `ViewPickerPure.h`) and
  the toolbar editor's command list 2026-11-24 (`docs/plans/shipped/input-ux-w2-surface-leftovers.md`).
- **A "close matches" cue for the grid.** When no row holds every word as written, the grid shows the rows
  whose title holds the letters in order, with no cue saying so. The cue belongs under the search box
  (`SmatchetGridHeaderUi.cpp` draws it; the tolerant-parsing PR #2441 has merged) with a flag on `GridPane.h`
  set by `ApplyGridFilterProjection` (`SmatchetActiveProjectGridTable.cpp`). `GridPane.h` and the only other per-pane
  home, `SmatchetUiSession.h`, were both edited by PR #2452, which merged on 2026-11-22 (`a9dbeb971`), so the cue is unblocked. Wave 3's
  `docs/plans/input-ux-w3-filter-without-jql.md` (S7) also builds it, on the line it adds under the box.
- **Label suggestions (#42).** The labels picker now matches through the matcher (typo-tolerant ranking), but
  its suggestions still come only from loaded tickets; a cached tracker label lookup plus recent labels is a
  data-source change.
- **Test placement.** The matcher's doctests live in `tests/Core/TextMatchPure.cases.h`, included by
  `tests/Core/TicketGridFilterPure.test.cpp`, because `tests/CMakeLists.txt` was plan-locked. Move them into a
  `tests/Core/TextMatchPure.test.cpp` registered in both the Linux subset and `SmatchetTests`.
- **Golden.** `tests/golden/command-palette-fuzzy.png` predates the palette's match marks; bucket-C's diff
  reports the change. Re-capture it through the CI bootstrap job and have a human approve it
  (`agent-layer/docs/agent-rules/golden-image-approval.md`).

Concrete next action:
The toolbar command marks and the remap Preview with trim-on-commit shipped
(`docs/plans/shipped/input-ux-w2-surface-leftovers.md`). Left: the grid's "showing close matches" line, which
Wave 3's filter plan (S7) builds; moving the matcher doctests to their own registered TU once
`input-ux-w3-grid-s2` releases `tests/CMakeLists.txt`; re-bootstrapping the palette golden (a human approves).
Each gets bucket-E coverage next to the existing dropdown / grid search / palette cases.

Status: open
Last-reviewed: 2026-11-24
