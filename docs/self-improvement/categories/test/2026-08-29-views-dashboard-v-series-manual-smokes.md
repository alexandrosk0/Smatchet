# Views dashboard V-series: 22 manual smokes never exercised, no automated coverage

- **Category**: test · **Priority**: P3 · **Filed**: 2026-08-29 (migrated from
  `backlog/MANUAL_TEST_QUEUE.md` on ledger retirement; V-series authored against the
  Views-window redesign, 2026-08-16 triage kept exactly these 22 as the actionable bucket)
- **Where**: the Views window two-pane settings editor —
  `Source/Core/src/Ui/SmatchetViewsDashboardUi*.cpp` and its grid interactions.

## Problem

The retired manual test queue's only actionable bucket was 22 pending human smokes
(V1–V22) for the Views-window redesign: sidebar selection/search (V1), tabbed editor
(V2), drag-and-drop + keyboard reorder (V3–V4), dirty tracking / discard modals (V5–V6,
V14–V15, V19), sort editing (V7), splitter persistence (V8), shortcuts (V9), toasts (V10),
grid column reorder / save gating (V11–V13, V16–V18), debounced auto-save (V20), and JQL /
user-field suggestions (V21–V22). None were ever exercised, and none have bucket-C/E
coverage. The behaviours mostly still exist but were reworked since the queue was written
(~#1620-era), so the original per-row setup notes may be stale.

## Proposed shape

Don't revive a standing manual queue. Instead, burn the list down opportunistically:
whenever a PR touches the Views dashboard, either add a bucket-E scenario covering the
V-rows nearest the diff (preferred — several, like V1/V2/V5, are scenario-harness-shaped)
or run the relevant smokes live and note it in that PR. The full V1–V22 row text with
setup/action/expected detail is preserved in git history at
`backlog/MANUAL_TEST_QUEUE.md` (removed 2026-08-29).
