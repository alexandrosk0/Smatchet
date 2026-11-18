# Plan — "+ ▾" new-pane caret with two Jira sites

**Status**: shipped (#2451)
**Branch**: `claude/plus-dropdown-two-jira-261a42`
**Issues**: fixes #2444 (bug); the full side-by-side feature is #2445
**Reported symptom**: "the down-arrow next to `+` is missing with two Jira instances"

## Context

`DrawNewPaneMenu` (`Source/Core/src/Ui/SmatchetGridHeaderUi.cpp`) drew the `▾` caret only when two or more tracker **kinds** in `ConfigManager::KnownBackendKeys()` had credentials. Multiple Jira sites (#2212) stay one `"Jira"` kind, so two Jira sites counted as one and the caret was hidden; the popup also had one row per kind, so it could never name a Jira site. The #2212 plan listed side-by-side Jira grids as out of scope.

## Approach

Option A from the scoping round (user decision, 2026-11-16): keep the Jira site a single global setting (`ActiveJiraDomain`).

- A pure helper builds the picker rows: one per credentialed kind, but one per usable Jira site (`New Jira · <host> pane`) when there are two or more. The caret shows when there are two or more rows.
- A Jira site row that is not the active site latches `PaneAddRequest::targetJiraDomain`. The pane host, after the pane loop and before the add request is applied, switches the active site via the same path as the Views - Jira Domain combo (`SelectActive` + `Save` + catalog refetch, plus a re-sync of the focused pane when it is a Jira pane). That path is now one `SmatchetUI::switchActiveJiraSite` used by both.
- A request that switched site never inherits the source pane's ticket snapshot (those rows belong to the old site).

## Interface contracts

- `smatchet::pane_targets::BuildNewPaneTargets(const TrackerConfig&) -> std::vector<NewPaneTarget>` and `ShowNewPaneCaret(targets)` in `Source/Core/include/Ui/PaneBackendTargetsPure.h`. Pure, no copy of the config (runs per frame per pane header).
- `PaneAddRequest::targetJiraDomain` (empty = no site switch).
- `SmatchetUI::switchActiveJiraSite(app, d, domain, resyncFocused) -> bool` (false = unknown host, nothing changed).

## Files to modify

1. `Source/Core/include/Ui/PaneBackendTargetsPure.h` + `Source/Core/src/Ui/PaneBackendTargetsPure.cpp` — new pure helper.
2. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp` — `DrawNewPaneMenu` draws the helper's rows.
3. `Source/Core/include/GridPane.h` — `PaneAddRequest::targetJiraDomain`.
4. `Source/Core/src/Ui/SmatchetGridPaneWindows_detail.cpp` — no snapshot inherit on a site switch.
5. `Source/Core/src/Ui/SmatchetGridPaneWindows.cpp` — host applies the site switch before the add request.
6. `Source/Core/include/Ui/SmatchetUI.h` + `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — shared `switchActiveJiraSite`.
7. `tests/Core/PaneBackendTargets.test.cpp` + `tests/CMakeLists.txt` — bucket-A.
8. `tests/ui/grid_pane_windows.test.cpp` — bucket-E caret probe.

## Existing utilities reused

`jira_backends::{NormalizeJiraHost, HostsMatch, SelectActive}`, `ConfigManager::BackendCredentialsPresent`, `BackendDisplayName`, `ApplyPaneAddAndCloseRequestsCore`.

## Extraction sizing (when this plan EXTRACTS or SPLITS code/docs)

N/A — no extraction; the shared site switch is a 15-line helper.

## UX Pillar callouts

- Pillar 1: the helper takes the config by `const&` and builds at most a handful of short strings per pane header per frame.
- Pillar 2: the site switch keeps the combo's blocking `ConfigManager::Save` (one-shot on a click, documented persist-before-sync), no new per-frame I/O.
- Pillar 6: a new pane on another site starts empty and fills from its own sync instead of showing the old site's rows.

## Perf-review-system gates (mandatory when diff touches `Source_Core/`; else `N/A — <reason>`)

1. **PR-fast CI** — header-toolbar draw only; no grid hot path. No curated scenario maps the header toolbar beyond the existing ones the Perf PR-fast lane runs.
2. **Per-frame cost** — replaces a 4-iteration credential loop with the same loop plus ≤ N site rows; no config copy.

## Risks / non-goals

- Non-goal: side-by-side panes on two Jira sites (#2445).
- Known hazard, filed as #2446 (P1): only the focused pane re-syncs on a switch, so a non-focused Jira pane keeps the old site's rows while Jira writes (which read `ConfigManager::Load()`) go to the new site. Pre-existing with the Views - Jira Domain combo; this picker makes two Jira panes common. A fix needs a re-arm of the per-pane one-shot sync kick in `AppController`, outside this UI slice.
- The site switch keeps the combo's blocking `ConfigManager::Save` on the UI thread (persist-before-sync); routing both callers through the config-save worker with a flush the sync waits on is a follow-up.
- `pane.new` (CLI / MCP) still takes a backend kind only; site selection there belongs to #2445.

## Implementation handoff

Done in this PR (single slice).

## Verification

- Bucket A: `PaneBackendTargets.test.cpp` — two sites show the caret; active site from `ActiveJiraDomain`; one site keeps the old single row; unusable second site not offered; mixed Jira + Plane order; site-switch add request does not inherit the snapshot.
- Bucket E: `GridPanes/TwoJiraSitesShowPickerCaret` — seeds a second site in memory, asserts the caret and both site rows (queried in the popup window, not `$FOCUSED`), restores, asserts baseline. `GridPanes/TwoJiraSitesRowOpensPaneOnSite` — clicks the second site's row (or its view sub-item), asserts the active site switched, a Jira pane opened, and it did not inherit the source's rows; the driver's isolated profile + scripted fixture backend contain the Save and the sync.
- Manual residue: none.
- Doc-validation suite, `grill-with-docs` (self-grilled against the #2212 plan's terms: **Jira backend instance**, **Active Jira domain**), deferral residue-sweep (the #2212 plan's out-of-scope line now points at #2445).

## Open questions

None.

## Out of scope (flagged, not designed)

- Per-pane Jira site — #2445.

## Implementation log

- 2026-11-16 — implemented per Approach.
- 2026-11-18 — merged develop (#2432), CodeRabbit round: Jira site rows no longer need the live site to carry a token (two other sites with their own tokens still get rows); plan archived.

## Deviations from plan

- Review round: added `TwoJiraSitesRowOpensPaneOnSite` (the click path was first left manual), `LOG_INFO`/`LOG_WARN` on the switch, the catalog refetch only when the focused pane is on Jira, and the stale-pane hazard filed as #2446.
- CodeRabbit round: `BuildNewPaneTargets` checks usable Jira sites before the live-site credential gate. Declined: localizing the row labels (the pre-#2444 label was English too, tracked as P2-M18) and the stale-pane write (waived, #2446). Deferred (backlog `debt/2026-11-18-pane-picker-jira-sites-deferred.md`): the offline bucket-E case for the switch, the header and call-site comments, the tooltip wording.

## Verification (actual)

See the PR #2451 test plan: doctest all pass (8 `BuildNewPaneTargets` cases + 1 `ApplyPaneAddAndCloseRequestsCore` case), GridPanes 4/4.

## Archive (post-ship — DO IN THIS PR, never a follow-up)

Done in #2451.
