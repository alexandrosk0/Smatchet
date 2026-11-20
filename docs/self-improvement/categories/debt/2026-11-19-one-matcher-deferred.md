- 2026-11-19 · input-ux-w2-one-matcher · [debt] · P2 — The shared matcher's highlight, grid cue and remap preview stop at locked or busy files

Details:
Input UX piece 4.7 (`docs/plans/input-ux-w2-one-matcher.md`) routed the pickers, filters and the grid
search through `TextMatchPure.h` and drew the matched letters in the shared dropdown and the command
palette (`Ui/SmatchetMatchHighlight.h`). Some of the audit's asks sit in files another branch held when it
shipped, so they were left out rather than overriding a plan-lock or racing an open PR:

- **Highlight in more rows.** The toolbar editor's command list (`SmatchetToolbarUi.cpp`, changed by the
  gesture-keys PR) ranks through the matcher but does not mark letters; the views list
  (`SmatchetViewsDashboardUi.cpp`, plan-locked by `pane-picker-jira-sites`) still filters by its own
  compare; grid cells do not mark the search words (a per-cell draw change, measured under Pillar 1 first).
- **A "close matches" cue for the grid.** When no row holds every word as written, the grid shows the rows
  whose title holds the letters in order, with no cue saying so. The cue belongs under the search box
  (`SmatchetGridSearchUi.cpp`, changed by the tolerant-parsing PR) with state on `GridPane.h` (plan-locked).
- **Remap preview (#131).** `ApplyPathRemaps` now trims, folds slashes and drive-letter case, and fixes the
  join; the audit's "Preview" field and trim-on-commit in the Annotate preferences
  (`AnnotateAnalysisUi_Preferences.cpp`, changed by two open PRs) are not built.
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
Once the locks and PRs above have landed: mark matched letters in the toolbar command list and the views
list with `SmatchetMatchHighlight::DrawText`; add the grid's "showing close matches" line under the search box
(a `GridPane` flag set by `ApplyGridFilterProjection` when the fallback ran); add the remap Preview field and
trim-on-commit; move the matcher doctests to their own registered TU; re-bootstrap the palette golden.
Each gets bucket-E coverage next to the existing dropdown / grid search / palette cases.

Status: open
Last-reviewed: 2026-11-19
