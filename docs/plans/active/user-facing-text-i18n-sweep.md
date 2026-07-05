# Plan — user-facing text i18n completeness sweep

> **Slug**: `user-facing-text-i18n-sweep`
>
> **Status**: `active`

## Context

The localization layer (`SmatchetLocalization` + the `SmatchetLocalizedImGui` wrapper)
covers most app-owned UI text, but a static sweep of `Source/Core` and
`Source/Standalone` found user-facing string literals that **bypass the lookup
entirely**: raw `ImGui::` calls in never-aliased TUs (Command Palette, toolbar
customization editor, icon picker, Notification Center, selectable-text context
menu, plan-doc viewer), deliberate `::ImGui::` bypasses of visible text in
aliased TUs (mobile shell), unlocalized `SmatchetToastManager::Push` titles and
messages, and combo **item arrays** (only the combo label routes through the
wrapper; items render raw). After this lands, every *clear* user-facing literal
in those surfaces routes through the lookup with en-US + fr-FR entries, and the
ambiguous cases are recorded in a report section below instead of being guessed
at.

Explicitly NOT localized, per README § Localization: user data, tracker values,
paths, backend error details, `LOG_*` messages, CLI/MCP protocol output, and
dev-only debug windows.

## Approach

Follow the established conventions, chosen per TU by how much user *data* the TU
renders:

1. **Explicit wraps** for data-heavy or strict-zone TUs (`Commands/CommandPaletteUi.cpp`
   — the `#define ImGui SmatchetLocalizedImGui` alias is banned there by the
   `define-imgui` lint rule — plus `SmatchetToolbarUi.cpp`, `SelectableTextRun.cpp`,
   `SmatchetOmnibarUi.cpp`, and the mobile shell's `::ImGui::` text sites):
   `SmatchetLocalization::T("key", "English")` for plain strings passed to raw
   ImGui calls / toast pushes, `SmatchetLocalization::Format` for printf-built
   text, matching `SmatchetWhisperSetupBanner.cpp`'s existing pattern. In the
   mobile shell the deliberate `::ImGui::` qualification is kept and only the
   string argument is wrapped, so the TU keeps a single visual idiom.
2. **TU-wide alias** for chrome-only TUs (`SmatchetIconPickerUi.cpp`,
   `SmatchetNotificationCenterUi.cpp`): the standard
   `#define ImGui SmatchetLocalizedImGui`; data-bearing calls inside them
   (per-row history entries, glyph buttons) are explicitly `::`-qualified or use
   `SelectableRaw` so tracker data never enters `TranslateSource`. Window
   titles keep their `###` stable-ID suffixes so dock/layout/popup IDs are
   language-independent. The wrapper gains `OpenPopup`/`BeginPopup` overloads so
   a translated popup title keeps hashing to the same ID as its `Begin*`
   counterpart — per-site pairing hacks are no longer load-bearing.
3. **Combo item arrays**: translate each item via `T()`; items are display-only
   (indices are what is stored), so translation cannot corrupt persisted state.
   Arrays holding *persisted tokens* (`kEffortIds`, `kLayouts`) or format names
   (`CSV`/`TSV`/`JSON`) are left alone.

Toast note: `SmatchetToastManager::Render` also runs title/message through
`TranslateSource` at render time (exact-English match). Call-site translation is
still the right layer — it covers `Format`-parameterized messages and the
Notification Center history, and an already-translated string passes through the
render-time lookup unchanged — but the render-time pass stays load-bearing for
legacy routes (`TicketSyncService` sync toasts), so do not remove it.

New catalog entries land in `kEntries` (both languages; French faithful to the
existing register — vouvoiement, "tickets", "le suivi" for Tracker, "vues",
"disposition", "file d'attente hors ligne"). Toast titles reuse existing
`toast.*` keys where they already exist ("Tracker", "Active Project", "Success",
"Queued Offline", "Lua Action").

The larger, *separate* gap — ~469 literals in aliased TUs that already route
through `TranslateSource` but have **no French catalog entry** (they silently
render English in fr-FR) — is out of scope here: it is a pure translation
backlog, not a routing defect. Quantified per-file in § Report so a follow-up
translation pass has a baseline.

## Files to modify

Grouped by change kind.

Catalog:
1. `Source/Core/src/SmatchetLocalization.cpp` — new `kEntries` for every string
   converted below (en + fr).

Never-aliased TUs (explicit `T()` / `Format()` at visible-literal sites):
2. `Source/Core/src/Commands/CommandPaletteUi.cpp` — "Parameters:", "Run",
   "Cancel", input hint, footer hint, "Set shortcut..." (strict zone — explicit
   calls only).
3. `Source/Core/src/Ui/SmatchetToolbarUi.cpp` — toolbar context menu, the whole
   "Customize Toolbar" editor modal, "Loading tracker toolbar buttons...",
   `kinds[]` combo items.
4. `Source/Core/src/Ui/SmatchetIconPickerUi.cpp` — "Pick Icon" modal title
   (stable `##SmatchetIconPicker` suffix kept), "Search icons..." hint, "Cancel".
5. `Source/Core/src/Ui/SmatchetNotificationCenterUi.cpp` — "Notifications"
   window title (stable-ID form), "Clear all", count line (pluralized →
   `Format`), "No notifications yet.".
6. `Source/Core/src/Ui/SelectableTextRun.cpp` — context menu "Copy" /
   "Select all".
7. `Source/Core/src/Ui/SmatchetOmnibarUi.cpp` — `OmnibarModeTooltip` strings +
   the two Search toasts.

Aliased TUs (bypass flips / item arrays):
8. `Source/Core/src/Ui/SmatchetMobileShellUi.cpp` — drawer headers
   ("Pages"/"Views"), empty-state lines, "Tickets###MobileGridList" /
   "Details###MobileGridDetail" titles, app-bar title, AI-not-built notice.
9. `Source/Core/src/Ui/SmatchetPreferencesUi_Local.cpp` — storage-mode,
   date-format, and UI-mode combo items; "Storage" toast title.
10. `Source/Core/src/Ui/SmatchetAuditUi.cpp` — "Result" filter combo items
    (`All`/`Success`/`Failure`).

Toast call sites (titles/messages → `T()` / `Format()`):
11. `Source/Core/src/SmatchetGridNotifications.cpp` — reuse `toast.tracker`,
    `toast.active_project`.
12. `Source/Core/src/SmatchetTicketChangeNotifications.cpp` — "Tickets" title.
13. `Source/Core/src/SmatchetGridFieldEditPipeline.cpp` — reuse
    `toast.queued_offline`, `toast.success`, `toast.field_update_saved`; new
    offline-edit message.
14. `Source/Core/src/Ui/SmatchetUI.cpp` — "Updates" toasts + "User Info" toasts.
15. `Source/Core/src/Ui/SmatchetUI_Layout.cpp` — "Layout reset" toast.
16. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp` — "View saved" /
    "Discarded changes" / "View deleted" titles (messages are view names — user
    data, untouched).
17. `Source/Core/src/Ui/SmatchetActiveProjectGridUi.cpp` — "View saved" /
    "Reverted layout" titles.
18. `Source/Core/src/Ui/SmatchetBugReportUi.cpp` — "Bug report" title +
    "Filed %s" message.
19. `Source/Core/src/Ui/SmatchetGridUiSupport.cpp` — "Queued: %s" /
    "Posting to %s" messages (reuse `toast.lua_action`, comments-queued title).

Tests:
20. `tests/Core/` — extend a localization doctest pinning a sample of the new
    key→fr mappings (same shape as `AnnotateLocalization.test.cpp`).

## Existing utilities reused

- `SmatchetLocalization::T` / `Format` / `Label` — `Source/Core/src/SmatchetLocalization.cpp:1107,1150,1218` — the whole point of the change.
- `SmatchetLocalizedImGui` wrappers — `Source/Core/include/SmatchetLocalizedImGui.h` — aliased-TU bypass flips.
- Existing `toast.*` catalog keys — avoid duplicate entries for "Tracker", "Success", etc.

## Extraction sizing

N/A — no file split or extraction; adds catalog rows + call-site wraps only.

## UX Pillar callouts

- **Pillar 1 (perf)**: `T()`/`TranslateSource` are hash-map lookups under a mutex, already on every localized widget per frame; the converted sites add the same constant cost. No new per-frame allocation beyond the existing `StoreTempString` ring.
- **Pillar 2 (UI-thread blocks)**: no I/O added; override JSON loads only on language switch (existing path).
- **Pillar 3 (never crash)**: printf-sink conversions use `Format`/existing specifier-match guard; no new format strings reach a printf sink unguarded (CPP_CODE_AUDIT #7 pattern respected).
- **Pillar 4 (accessibility)**: unchanged behaviour; French text may be longer — all touched widgets auto-size or wrap.

## Perf-review-system gates

1. **PR-fast CI** — no hot-path change; grid-draw scenarios cover the touched draw paths incidentally. N/A beyond the standard lanes.
2. **Pillar 2 static scanner** — no new sync I/O reachable from `ImGui::*`.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new stall path.

## Verification

- `cmake --preset posix-core-check && cmake --build` (Linux container lane) +
  `ctest` for the localization doctests.
- `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` (no
  `define-imgui` hits in strict zone; comment rules).
- UI-behavioral validation (golden images / ImGui Test Engine buckets) happens
  in CI — treated as the gate for this PR.

## Report — ambiguous / deliberately-skipped cases

- **Missing-French backlog (not a routing defect)**: ~469 literals across 34
  aliased TUs already route through `TranslateSource` but have no fr-FR entry;
  top files: `AnnotateAnalysisUi_Window.cpp` (48), `SmatchetUI_MainMenu.cpp`
  (44), `SmatchetOfflineQueueUi.cpp` (39), `SmatchetGridUiSupport.cpp` (36),
  `SmatchetPreferencesUi_Local.cpp` (36), `SmatchetViewsDashboardUi.cpp` (35),
  `SmatchetPreferencesUi.cpp` (34). Follow-up: pure translation pass, no code
  motion.
- **`SmatchetPlanDocViewerUi.cpp`** ("Plan docs" window) — maintainer/agent
  tooling surfaced in-app; audience ambiguous → left as-is, listed here.
- **`SmatchetUI.cpp` DockDebug window** — dev-only debug overlay behind a
  hotkey → not user-facing, skipped.
- **`Source/Core/src/Commands/Scenarios/*`** — ImGui Test Engine scenario
  windows (`##scenario_*`) → test harness, skipped.
- **`SmatchetPerfUi.cpp`** — Performance Monitor is reachable from the menu but
  is a diagnostic surface; its 12 missing-entry strings fold into the
  translation backlog above.
- **Persisted-token arrays** — `kEffortIds` (`low/medium/high`),
  `kLayouts` (`unified/separate`): stored values, not display text → must stay
  untranslated as written; a display-name indirection is a follow-up if wanted.
- **Format-name arrays** — `CSV`/`TSV`/`JSON`/`Auto` in `SmatchetBulkTicketsUi.cpp`:
  format names are proper nouns; only "Auto" is translatable → skipped as a set.
- **`Source/Standalone/`** — no ImGui string literals; pre-logger stderr output
  is console/log, not UI → out of scope by charter.
- **Generic-English source-map exposure (pre-existing design tension)**: every
  catalog entry's English feeds `EntriesByEnglish`, so an aliased-TU widget fed
  tracker *data* that exactly equals a catalog English string renders translated
  ("Offline" → "Hors ligne"). The class predates this PR ("All", "Save",
  "Success"…); this PR flips the one flagged data sink
  (`TrackerGridFieldDisplay.cpp` attachment display) to raw `::ImGui::` and
  keeps data out of the wrapper in the TUs it touches. Follow-up candidate: an
  audit of aliased-TU wrapper calls fed non-literal strings + a
  `TextUnformattedRaw` convention.
- **Context-scoped duplicate rows kept deliberately**: `toast.tickets` /
  `mobile.page.tickets` ("Tickets") and `mobile.page.views` / `window.views`
  ("Views") stay separate keys — different UI contexts may legitimately need
  different translations in future locales, matching existing catalog practice
  (`common.clear` vs `lua.clear` vs `keybindings.editor.clear`). Exact
  *same-context* duplicates found in review (audit filter items vs the
  pre-existing `audit.*` rows, `prefs.dateformat.*` vs `prefs.date_*`,
  `updates.newer_available` vs `update.banner.short`) were deduplicated onto the
  pre-existing keys.
- **Cell-status tokens** (`CellWriteFeedback.Message = "Queued"/"Saved"`,
  `SmatchetOfflineQueueUi` state strings, `badgeTooltip = "Saved"`) — short
  status tokens rendered through data paths; localizing them at set-time bakes a
  language into session state. Left as-is, listed for a follow-up decision.
- **Mobile nav button IDs follow the translated label** (raw `::ImGui::Button`
  with `PushID(i)` scoping): ImGui Test Engine automation addressing nav
  buttons by English label only works under en-US. CI buckets run en-US;
  persisted config stores ids, not labels — accepted.

## Implementation log

_(post-ship)_

## Deviations

_(post-ship)_
