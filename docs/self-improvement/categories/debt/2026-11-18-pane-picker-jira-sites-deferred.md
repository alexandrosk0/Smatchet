- 2026-11-18 · code-review + CodeRabbit on #2451 (#2444) · [debt] · P2 — "+ ▾" Jira site picker: offline coverage and stale wording

Details:
Deferred from the #2444 review rounds (plan `docs/plans/pane-picker-jira-sites.md` § Deviations):
- No offline (Pillar 6) bucket-E case for the Jira site switch from the grid header caret. A pane opened on
  another site starts without the source pane's rows by design, so with the tracker unreachable it depends on
  that site's cache; nothing asserts it shows cached rows with a `DataFreshnessCue` instead of a loading-only grid.
- Comments still describe the old rule: `Source/Core/include/Ui/PaneBackendTargetsPure.h` header doc ("one row
  per credentialed tracker kind, except Jira with two or more sites"; Jira site rows now show whenever two or
  more sites are usable, even when the live site has no token) and the `SmatchetGridHeaderUi.cpp` call-site
  comments ("≥2 backends are credentialed"; the rule is ≥2 picker rows).
- The caret tooltip `pane.backend.picker.tooltip` says "Open a new pane on a different tracker backend"; with
  only two Jira sites every row is the same backend on another site.
- Row labels (`New <Backend>[ · host] pane`) are English-only, as the pre-#2444 label was (P2-M18).

Concrete next action:
- Add a `GridPanes/TwoJiraSitesRowOpensPaneOffline` case (`GlobalFakeNetwork()` offline, or a
  `test-ui-offline-first.sh` case) asserting the new pane renders the other site's cached rows with a freshness cue.
- Reword the two comments and the tooltip (tooltip touches `SmatchetLocalization.cpp`: visual-validation exception).

Status: open
Last-reviewed: 2026-11-18
