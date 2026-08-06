# Plan — Quick-create issue popup (Ctrl+Shift+T) with configurable Unreal Engine context

> **Slug**: `quick-create-issue-unreal-context`
>
> **Status**: `active`

## Context

Inside the Unreal Editor, filing a tracker issue about something just observed (a broken actor, a
PIE glitch, an error in the Output Log) requires opening the overlay, navigating to the full
new-issue flow, and hand-copying engine facts into the description. Users asked for a keyboard
shortcut that opens a lightweight create-issue popup with the issue type / summary / description,
where the description arrives prefilled with context gathered from the running Unreal instance —
and for that context set to be configurable in Preferences. After this lands: pressing
**Ctrl+Shift+T** in the visible overlay opens a quick-create popup whose description is seeded with
engine version, project, platform/build config, current map, PIE state, selected actors, and an
Output Log tail — each item toggleable in a new embedded-only Preferences tab.

User decisions (session 2026-07-13): `Ctrl+Shift+J` remains the overlay toggle untouched; the new
shortcut is `Ctrl+Shift+T` (rebindable); the default context set is ALL items, each with its own
checkbox plus a configurable log-line count.

## Approach

Register one new command, `issue.quick_create.open`, in the unified registry (auto-surfaced in
palette/CLI/MCP/Lua/Unreal console) and bind it to `Ctrl+Shift+T` in `KeybindingsConfig::Defaults()`
with a one-shot seed migration for existing configs. The popup is a new `UiDrawSession`-flag-gated
ImGui window following the bug-report modal *shape* (open latch, background submit, toast), sourcing
issue types from the field catalog's `issuetype` `AllowedValueOptions` and submitting through the
backend-agnostic `AppController::CreateIssueAsync`, with `QueueCreateOffline` as the transient-error
fallback.

Unreal context crosses the C ABI as a JSON snapshot: the plugin pushes
`SmatchetHost_SetHostContextJson(...)` at 1 Hz from a game-thread `FTSTicker` while the overlay is
visible (a synchronous core→UE provider callback was rejected — host frames run on the render
thread under `ImGuiMutex`, and `ShutdownModule` flushes rendering commands, so a blocking callback
risks deadlock). Core lands the snapshot in a new mutex-guarded `smatchet::hostctx` seam; a pure
formatter (`BuildEngineContextMarkdown`) filters it through the Preferences toggles into markdown on
a worker thread when the popup opens. The standalone build never writes the snapshot, so the popup
degrades to an empty prefill there. A prerequisite fix rides along: the Unreal input processor's
`ToImGuiKey` maps only A/C/V/X/Y/Z today, so no letter chord besides those ever reaches core in the
overlay — it is widened to all letters/digits/F-keys/punctuation, which enables `Ctrl+Shift+T` (and
every other rebindable chord) inside Unreal.

## Files to modify

Core — Ui (light zone):

1. `Source/Core/include/Ui/SmatchetUiSession.h` — `quickCreate*` state cluster. (The old `PreferencesActiveTab::QuickCreate` enumerator is gone: the preferences-IA re-segmentation replaced the tab bar with 8 categories, and the quick-create rows now draw as an `SMATCHET_EMBEDDED_IN_UNREAL`-gated section under `PreferencesCategory::Editing`.)
2. `Source/Core/include/Ui/SmatchetQuickCreateIssueUi.h` + `Source/Core/src/Ui/SmatchetQuickCreateIssueUi.cpp` (new) — the popup; helpers split per the ImGui-draw pattern.
3. `Source/Core/src/Ui/SmatchetUI.cpp` — draw call beside `SmatchetBugReportUi_Draw`; future drain on teardown.
4. `Source/Core/src/Ui/SmatchetPreferencesUi_QuickCreate.cpp` (new) + `SmatchetPreferencesUi.cpp` + `SmatchetPreferencesUi_detail.h` — embedded-only "Unreal" section under Editing.
5. `Source/Core/src/SmatchetLocalization.cpp` — en/fr rows for all new visible strings.
6. `Source/Core/include/Ui/SmatchetImGuiHostC.h` + `Source/Core/include/Ui/SmatchetImGuiHost.h` + `Source/Core/src/Ui/SmatchetImGuiHost.cpp` — `SmatchetHost_SetHostContextJson` C ABI + host method.

Core — strict zones:

7. `Source/Core/src/Commands/AppViewCommands.cpp` — register `issue.quick_create.open`.
8. `Source/Core/src/Config/KeybindingsConfig.cpp` — default binding row.
9. `Source/Core/include/Config/ConfigManager.h` + `src/Config/ConfigManager.cpp` + `src/Config/ConfigManager_Load.cpp` — 7 context bools, log-line count (+clamp), `MigratedQuickCreateHotkeyV1` seed migration.

Core — Diagnostics (new TUs; GLOB'd, no CMake edit):

10. `Source/Core/include/Diagnostics/EngineHostContext.h` + `src/Diagnostics/EngineHostContext.cpp` (new) — mutex-guarded snapshot store.
11. `Source/Core/include/Diagnostics/EngineContextFormat.h` + `src/Diagnostics/EngineContextFormat.cpp` (new) — pure toggles→markdown formatter.

Unreal plugin (compiles only on Windows/UE):

12. `Source/UnrealPlugins/SmatchetImGuiPlugin/ThirdParty/Smatchet/include/SmatchetImGuiHostC.h` — identical C ABI addition (packaged copy).
13. `Source/UnrealPlugins/.../Private/SmatchetOutputLogTail.h/.cpp` (new) — GLog ring buffer.
14. `Source/UnrealPlugins/.../Private/SmatchetHostContextCollector.h/.cpp` (new) — game-thread JSON builder.
15. `Source/UnrealPlugins/.../Private/SmatchetImGuiPluginModule.cpp` — ticker push + log-device lifecycle.
16. `Source/UnrealPlugins/.../Private/SmatchetImGuiInputProcessor.cpp` — `ToImGuiKey` widening.
17. `Source/UnrealPlugins/.../SmatchetImGuiPlugin.Build.cs` — conditional `UnrealEd` dep for editor selection APIs.

Tests:

18. `tests/Core/EngineContextFormat.test.cpp` (new) + `tests/CMakeLists.txt` registration.
19. `tests/Core/KeybindingsConfig.test.cpp`, `tests/Core/ConfigMigration.test.cpp`, `tests/Core/ConfigManager.test.cpp`, `tests/Commands/BuiltinCommandsDispatch.test.cpp` — extend for the new row/keys/command.

## Existing utilities reused

- Bug-report modal shape (flag gate, open latch, background submit, toast): `SmatchetBugReportUi_Draw` — `Source/Core/src/Ui/SmatchetBugReportUi.cpp` (shape only; no textual clone, dup_audit is blocking).
- Issue-type dropdown sourcing from the field catalog: `SmatchetNewIssueDraftUi.cpp:408-460` pattern (`issuetype` → `AllowedValueOptions`).
- `AppController::CreateIssueAsync` / `QueueCreateOffline` — backend-agnostic create + offline fallback.
- `SmatchetProjectPicker` — project selection state/widget.
- `json_safe::ParseBounded` — `Source/Core/include/Json/BoundedJsonParse.h:122` (snapshot JSON is external input).
- `MigratedMenuShortcutsV1` migration-flag precedent — `Source/Core/include/Config/ConfigManager.h:258`.
- `app.bug_report.open` registration precedent — `Source/Core/src/Commands/AppViewCommands.cpp:154`.
- `SmatchetToastManager`, `MarkPrefsDirty`, `BoundHotkeyDisplay`, `SmatchetLocalizedImGui`.

## Extraction sizing

N/A — this plan extracts/splits nothing; it adds new TUs and small edits.

## UX Pillar callouts

- **Pillar 1 (perf)**: no steady-state impact — the popup draws only while open; the host context
  setter is a small string copy at 1 Hz behind a mutex touched nowhere in the frame loop.
- **Pillar 2 (UI never freezes)**: snapshot parse + markdown formatting run on
  `LaunchBackgroundTask`; create runs via `CreateIssueAsync` future polled per frame; no sync I/O on
  the UI thread.
- **Pillar 3 (never crash)**: snapshot parsed with `json_safe::ParseBounded` under a 128 KB inbound
  cap; C ABI setter NULL-safe; UE log ring is `FCriticalSection`-guarded and allocation-light in
  `Serialize`; GLog device removed before host destroy.
- **Pillar 4 (accessibility)**: popup is fully keyboard-operable (Esc cancel, Ctrl+Enter submit,
  tab order follows widget order).

## Perf-review-system gates

1. **PR-fast CI** — no scenario directly exercises a closed popup; nearest is the standard UI
  steady-state set, unaffected (popup draws nothing when `showQuickCreateIssue` is false).
2. **Pillar 2 static scanner** — no new sync-I/O reachable from `ImGui::*`; prefill + submit are
  worker-threaded.
3. **Dispatcher drain** — untouched.
4. **Visible-cue bucket-E harness** — no new >100 ms sync stall path.
5. **Marker inventory** — no new `SMATCHET_UI_PERF_SCOPE` markers.

## Risks / non-goals

- `ToImGuiKey` widening changes which keys reach ImGui in the overlay for all letters/digits/F-keys
  — intended (it is the enabler); mitigation: chars with modifiers held are still not injected as
  text (existing `HasTextInputBlockingModifier` guard), manual editor pass advised.
- Two-copy C ABI header drift — both copies edited in the same commit; the Build.cs staleness guard
  forces a repackage before UE consumes it.
- Jira projects with extra required create fields: quick-create surfaces the server error with a
  hint to the full new-issue flow — accepted (rendering arbitrary required-field editors is the full
  flow's job).
- Snapshot staleness ≤ 1 s — accepted; popup re-seeds on open and offers "Refresh context".
- Non-goals: screenshots/attachments in quick-create; non-Unreal host contexts (the seam is generic
  but only the UE plugin feeds it); a global OS-level hotkey (works when the overlay is visible).

## Verification

- **Bucket A (pure-logic ctest, `test-rig`)**: `EngineContextFormat.test.cpp` (per-toggle filtering,
  log truncation edges, malformed/empty snapshot → empty, actor cap); `KeybindingsConfig.test.cpp`
  default-row parity; `ConfigMigration.test.cpp` seed-once idempotence (user deletion respected);
  `ConfigManager.test.cpp` round-trip + clamp; `BuiltinCommandsDispatch.test.cpp` registration +
  dispatch of `issue.quick_create.open`.
- **Bucket E (ImGui Test Engine)**: none added — the popup is exercised via bucket A logic tests +
  the manual Unreal checklist; a bucket-E driver is a follow-up if the popup grows logic.
- **Bash-driver scenario / screenshot / sanitizer**: nightly sanitizer build covers the new TUs.
- **Build gate**: `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12`
  (dual-target; Windows). In the Linux authoring environment: `cmake --preset posix-core-check &&
  cmake --build --preset posix-core-check` compiles all Core TUs.
- **Doc validation**: `scripts/dev/test-docs.sh` green.
- **Plan stress-test — `grill-with-docs`**: run before finalising; outcome recorded in § Deviations.
- **Manual residue**: UE-editor behavior (overlay → Ctrl+Shift+T popup, prefill contents, PIE /
  selection / log tail, Jira create) is manually verified on a Windows dev box — deferred-automation
  action: a UE automation-spec smoke is backlogged under
  `docs/self-improvement/categories/tooling/` (no headless UE in CI today).

## Out of scope (flagged, not designed)

- Attachments/screenshot capture in the quick-create popup — follow-up; the full new-issue flow and
  bug-report modal already cover attachment needs.
- Feeding the host-context snapshot into the AI assistant's auto-context blocks — natural follow-up
  reusing `smatchet::hostctx`, not designed here.
- Per-backend context templates (e.g. ADF-native rendering for Jira) — description stays markdown;
  existing `MarkdownToAdf` conversion applies downstream.

## Implementation log

- `5b3a62a` · feat(quick-create): Ctrl+Shift+T issue popup with engine-context prefill (core) — command,
  keybinding + seed migration, popup UI, config toggles, hostctx seam, markdown formatter, embedded-only
  "Unreal" Preferences tab, en/fr strings, tests.
- `0a28a95` · feat(unreal): push engine context snapshot to core + full hotkey coverage — C ABI
  `SmatchetHost_SetHostContextJson` (both header copies), GLog ring-buffer tail, 1 Hz game-thread
  snapshot ticker, `ToImGuiKey` widening, conditional UnrealEd dep.

## Deviations from plan

- `RegisterAppViewCommands` was decomposed (new file-static `RegisterQuickCreateOpen`) — the inline
  registration pushed the function past the 120-line cap.
- The pre-existing include-block duplication marker in `SmatchetUI.cpp` was re-anchored (the new
  include shifted the clone window past the old marker line).
- Quick-create submit reuses `IssueDraftHelpers::MissingRequiredFields` client-side before the async
  create (mirrors the grid draft flow) instead of relying purely on the server error, in addition to
  the planned server-error surface + full-flow hint.

## Verification (actual)

- Linux (this environment): `posix-core-check` configure + build green — all 493 core TUs including the
  4 new ones (curl FetchContent tarball is proxy-blocked here; overridden with
  `-DFETCHCONTENT_SOURCE_DIR_CURL=<git clone of curl-7_80_0>`, same pinned version).
- Lint gates (`test-lint-rules.sh --diff origin/develop`): all green (advisory tu-line-ceiling WARNs on
  pre-existing whales only).
- Doc validation (`scripts/dev/test-docs.sh`): 16/16 green.
- Targeted doctest run (new/changed test TUs — EngineContextFormat, KeybindingsConfig, ConfigMigration,
  ConfigManager, I18nSweepLocalization — linked against the built core archive): 58/58 cases,
  624 assertions, green.
- Not run here (no Windows/UE): dual-target `SmatchetStandalone`+`SmatchetCore_DX12` build, plugin
  repackage, in-editor manual checklist (overlay → Ctrl+Shift+T popup, prefill contents, PIE/selection/
  log tail, Jira create). Runs on Windows CI + a Windows dev box per § Verification.

## Archive (post-ship — DO IN THIS PR, never a follow-up)
1. *flip the § Status header to `shipped`,*
2. *`git mv docs/plans/active/<slug>.md docs/plans/shipped/<slug>.md`,*
3. *regen the index: `bash agents/scripts/core/test-plan-index.sh --fix`.*
