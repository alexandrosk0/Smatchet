# Plan — Full Function-Size Compliance (zero grandfathered monoliths)

> **Slug**: `full-function-size-compliance`

> **Relationship to [`decompose-top-20-monoliths`](decompose-top-20-monoliths.md)**: that plan shipped the
> Slice-0 gate + pattern doc + every *dedicated non-UI* decomposition, then deliberately froze the ImGui-draw
> set as **perpetual ride-along** (it argued a UI-decomposition sweep is net-negative ROI). **This plan
> reverses that freeze** by maintainer direction (2026-06-02): drive the **entire** grandfathered set under
> the hard caps so the baseline goes to **zero** and the delta gate enforces compliance with no escape
> hatches left. The parent plan stays the canonical source for the **recipes** (§ Approach A/B), the
> **gate mechanics** (Slice 0), and the **pattern doc** (`docs/guides/imgui-draw-pattern.md`); this plan owns
> the **completion program** — sequencing, batching, per-PR verification for the remaining 84 functions.

## Context

The function-size gate (`agents/scripts/core/function_size_audit.py`, shipped #627) is **delta-gated**: it
fails only NEW or just-crossed functions, grandfathering everything in the base set. That stopped the bleed
but left a large frozen population. A live `function_size_audit.py --list` sweep against `develop`
(2026-06-02) shows **84 distinct functions** still over the hard caps (non-UI **120 lines** / ImGui-draw
**200 lines** / **30 branches** for all) — the informational snapshot in
[`docs/high-integrity/function-size-baseline.md`](../../high-integrity/function-size-baseline.md) lists 116
rows because a function over both caps appears twice.

Distribution of the 84:

| Bucket | Count | Owner(s) | Risk profile |
|---|---|---|---|
| ImGui window/tab draws (`Source/Core/src/Ui/`, positional) | 26 | `grid-engine`, UI generalist | **High** — Begin/End + Push/Pop ordering, golden-gated |
| Cell/field editors (`TicketFieldEditor*`, `TrackerDateTimeFieldEditor`) | 6 | `grid-engine` | High — interactive widgets |
| Markdown / syntax / text-run render | 9 | UI generalist | Mixed — some pure (bucket-A), some ImGui |
| Tracker pure-logic (parse / map / query) | 11 | `tracker-backend` | **Low** — bucket-A testable |
| App controllers / bootstrap / subprocess | 14 | orchestrator, `debug-detective` for ASan | Medium — boot/HTTP paths |
| Command registrars | 4 | `command-system` | **Low** — mechanical cut/paste |
| Plugins (Mcp / Whisper / LuaConsole) | 9 | `mcp-toolsmith`, Whisper, `lua-binder` | Medium |
| Sync ticks | 2 | `offline-sync` | Medium — perf-sensitive Tick paths |
| AI prefs / controller | 3 | orchestrator | Low-medium |

**Intended outcome — after this plan lands, `function_size_audit.py --list` on `develop` returns empty, the
baseline snapshot reads "0 oversized functions," and any future regression hard-fails CI** (the delta gate
needs no grandfather list once the base set is clean).

## Approach

**Re-apply the parent plan's proven recipes; sequence low-risk-first; one logical feature per PR.** No new
mechanism is invented here — § Approach A (ImGui `DrawCtx` + section-helper) and § Approach B (table-dispatch
/ phase-split) from [`decompose-top-20-monoliths`](decompose-top-20-monoliths.md) cover every case. The single
deviation from the parent plan is **policy**: the UI set graduates from ride-along to an actively-scheduled
phase, accepting the positional-ImGui regression risk and paying it down per-PR with bucket-C/E golden diffs
(the canary #632 is the worked reference that this is tractable).

**Sequencing principle — drain risk-ascending so momentum + test coverage build before the dangerous UI
surgery.** Phase 1 (pure-logic, bucket-A) and Phase 2 (mechanical registrars + controllers) are
low-regression and produce reusable test scaffolding; Phase 3 (editors) and Phase 4 (UI window draws) carry
the positional-ImGui risk and ride on the golden harness. The branch cap (30) is the binding constraint for
most pure-logic functions even when they're under the line cap — those decompose by **extracting branch
clusters into named sub-helpers or table dispatch**, not by line-splitting.

**Batching — one PR per file-cluster, grouped by owner, never one-PR-per-function for same-file siblings.**
Functions sharing a `.cpp` (e.g. the four `TicketFieldEditor` editors, the three `MarkdownConvert` emitters)
ship in **one** PR to respect the CodeRabbit review quota; a single 945-line monolith
(`AnnotateAnalysisUi::DrawContent`) is its own PR. Each PR carries the full do-not-pause checklist + merge
gates + verification bucket for its risk class. Target ≈ **32 PRs** across 5 phases.

## Files to modify

> Paths + line numbers are locators from the 2026-06-02 `--list` sweep; they drift — **cross-check the live
> `--list` output before starting each batch** (parent plan's standing correction: verify against the audit,
> not the named symbol). Every row decomposes to ≤ 120 (non-UI) / ≤ 200 (UI) lines **and** ≤ 30 branches.

### Phase 1 — Pure-logic, bucket-A testable (lowest risk; `tracker-backend` + core)

1. `Source/Core/src/Tracker/GitHubQueryFromJql.cpp:228` — `TranslateJqlToGitHubSearch` (185L/64br) → operator/clause-handler table. **(PR A1)**
2. `Source/Core/src/Tracker/GitHubIssueSearch.cpp:163` — `RunGraphQlIssueSearch` (133L/31br) → page-fetch ↔ parse split. *(bundle with A1 — both GitHub)*
3. `Source/Core/src/Tracker/TrackerFieldValueParser.cpp:520` — `ParseChangelog` (134L/44br) + `:780` `NormalizeTrackerFieldValue` (116L/54br) → per-field-type handler table. **(PR A2)**
4. `Source/Core/src/Tracker/JiraIssueMappingPure.cpp:76` — `AppendCachedTicketFromJiraSearchIssue` (88L/33br) + `Source/Core/src/Tracker/PlaneIssueMappingPure.cpp:52` `MapPlaneWorkItemJsonToCachedTicket` (118L/41br) → shared field-extract helpers. **(PR A3)**
5. `Source/Core/src/Tracker/JqlSuggestEngine.cpp:537` — `BuildJqlSuggestions` (125L/33br) → suggestion-category sub-builders. **(PR A4)**
6. `Source/Core/src/Tracker/IssueCreatePipeline.cpp:49` — `ApplyPostIssueSteps` (107L/31br) → per-step helpers. **(PR A5)**
7. `Source/Core/src/Tracker/JiraIssueSearch.cpp:85` — `JiraClient::FetchIssuesStreamed` (197L/48br) + `Source/Core/src/Tracker/PlaneIssueSearch.cpp:248` `PlaneClient::FetchIssuesStreamed` (186L/29br) + `Source/Core/src/Tracker/JiraUserAndMeta.cpp:188` `FetchIssueEditMeta` (98L/35br) → fetch→parse→map phase-split (mirrors shipped `FetchFieldCatalog` #643). **(PR A6)**
8. `Source/Core/src/CompactDateFormat.cpp:286` — `FormatCompactJiraDateForDisplay` (143L/36br) → unit-bucket table. **(PR A7)**
9. `Source/Core/src/TicketGridModel.cpp:111` — `ResolveRenderPlan` (84L/36br) + `:231` `CompareFieldValuesForSort` (76L/34br) → field-kind dispatch table (both branch-only, under line cap). **(PR A8, `grid-engine`)**
10. `Source/Plugins/Whisper/HotkeyParse.cpp:78` — `TokenToVirtualKey` (60L/33br) → static keyword→VK map (branch-only). **(PR A9)**
11. `Source/Core/src/Ui/CodeColorView.cpp:262` — `TokenizeWithLd` (137L/37br) + `Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp:59` `ParseImageDimensions` (116L/62br) → pure tokenizer/parser sub-helpers (in `Ui/` but non-draw, bucket-A). **(PR A10)**

### Phase 2 — Mechanical registrars + non-UI controllers / bootstrap (low-medium risk)

12. `Source/Core/src/Commands/Builtin/BuiltinCommands_Config.cpp:20` `RegisterConfigCommands` (165L/11br), `BuiltinCommands_Meta.cpp:22` `RegisterMetaCommands` (147L/15br), `BuiltinCommands_Tickets.cpp:24` `RegisterTicketsCommands` (122L/14br), `BuiltinCommands_Ai.cpp:377` `RegisterSendOnceCommand` (143L/13br) → per-command free functions (mirrors `RegisterAiCommands` #634). **(PR B1, `command-system`)**
13. `Source/Core/src/AppController.cpp:1231` `InitBackends` (138L/22br) + `:1370` `InitFieldCatalog` (139L/22br) → sub-phase helpers (line-only). **(PR B2)**
14. `Source/Core/src/AppController_CatalogAndFieldEdit.cpp:1131` `SubmitFieldEditNetworkOnly` (142L/25br) + `:193` `SetFieldCatalog` (127L/21br) → phase helpers. **(PR B3)**
15. `Source/Core/src/AppController_LuaBindings.cpp:1235` `RunAutomationJob` (133L/25br) + `Source/Core/src/AppController_LuaBindings_Draw.cpp:305` `ReplayCmdList` (121L/43br) → command-replay dispatch table. **(PR B4, `lua-binder`)**
16. `Source/Core/src/SubprocessCapture.cpp:120` `RunWindows` (251L/50br) + `:374` `RunPosix` (193L/35br) → setup/spawn/pump/reap helpers. **(PR B5; ASan)**
17. `Source/Standalone/CliCommandRunner.cpp:1059` `RunCmdInProcessImpl` (160L/29br) → command-class dispatch. **(PR B6)**
18. `Source/Standalone/main.cpp:424` `BootApplication` (153L/17br) + `:709` `RunFrameLoop` (151L/18br) → sub-helpers (both already extracted by #678, just over the 120 non-UI cap now; line-only). **(PR B7; ASan)**
19. `Source/Core/src/AiAssistantController.cpp:416` `StreamAndDispatch` (139L/16br) → chunk-handler split. **(PR B8)**
20. `Source/Core/src/AiPrefsTestConnection.cpp:23` `TriggerProbe` (164L/30br) + `Source/Core/src/AiPrefsValidator.cpp:59` `ValidateAiPrefs` (121L/33br) → per-field validators table. **(PR B9; bucket-A on validator)**
21. `Source/Core/src/Sync/OfflineQueueService.cpp:889` `TickOfflineCreates` (188L/29br) + `Source/Core/src/Sync/TicketSyncService.cpp:473` `StartStreamingSync` (140L/28br) → phase helpers. **(PR B10, `offline-sync`; profile before/after — perf-sensitive Tick paths)**

### Phase 3 — Plugins (medium risk)

22. `Source/Plugins/Mcp/McpPlugin.cpp:238` `RegisterTicketRoutes` (143L/29br) + `:409` `RegisterToolsCallRoute` (135L/21br) → per-route sub-helpers (continues #635). **(PR C1, `mcp-toolsmith`)**
23. `Source/Plugins/Whisper/ModelDownloader.cpp:190` `Start` (190L/26br), `WhisperPlugin.cpp:398` `BuildTranscribeOnceCommand` (169L/19br) + `:737` `RunHotkeyRelease_Worker` (142L/18br), `WindowsAudioCapture.cpp:331` `CaptureThreadMain` (231L/39br) → phase/loop-body helpers. **(PR C2 + C3 — split Whisper across two PRs by file-pair to respect review quota)**
24. `Source/Plugins/LuaConsole/LuaConsolePlugin.cpp:359` `OnDraw` (381L/72br) → `DrawCtx` + section helpers (ImGui — golden-gated). **(PR C4, `lua-binder`)**

### Phase 4 — Editors + Markdown/text render (high risk; golden-gated)

25. `Source/Core/src/TicketFieldEditor.cpp:913` `RenderFieldCell` (403L/80br), `:451` `RenderSingleSelectEditor` (182L/48br), `:634` `RenderMultiSelectEditor` (156L/41br), `:315` `RenderTextEditor` (135L/38br) → field-kind dispatch + per-editor `DrawCtx`. **(PR D1, `grid-engine`)**
26. `Source/Core/src/TicketFieldEditor_Modal.cpp:192` `RenderLongTextModal` (367L/79br) → section helpers. **(PR D2, `grid-engine`)**
27. `Source/Core/src/Tracker/TrackerDateTimeFieldEditor.cpp:53` `DrawCalendarPicker` (243L/44br) → grid/nav/footer helpers. **(PR D3, `grid-engine`)**
28. `Source/Core/src/Ui/MarkdownConvert.cpp:1010` `EmitAdfBlock` (141L/55br), `:778` `EmitInlineText` (98L/37br), `:1262` `ParseHtmlTag` (92L/40br) → node-type handler tables (pure → bucket-A). **(PR D4)**
29. `Source/Core/src/Ui/MarkdownPreviewRender.cpp:790` `RenderPlanBlock` (216L/51br), `:174` `PreviewEnterBlock` (151L/40br), `:646` `EmitWordsRS` (122L/33br) → block-kind dispatch (ImGui draw — golden-gated). **(PR D5)**
30. `Source/Core/src/Ui/CppSyntaxHighlight.cpp:41` `DrawColoredCppLine` (121L/59br) + `:226` `DrawColoredCallstackLine` (143L/38br) → token-class dispatch. **(PR D6)**
31. `Source/Core/src/Ui/SelectableTextRun.cpp:139` `End` (184L/48br) → state-machine sub-helpers. **(PR D7)**

### Phase 5 — ImGui window / tab draws (highest risk; bucket-C/E golden mandatory)

> One PR per row unless noted. Each follows § Approach A: reuse existing `SMATCHET_UI_PERF_SCOPE` seams
> verbatim where present (zero baseline shift); invent logical seams + regen `MARKER_INVENTORY.md` where
> absent. Hoist `static` locals to `<Foo>WindowState`; action handlers → `OnX()`.

32. `Source/Core/src/Ui/AnnotateAnalysisUi_Window.cpp:60` `AnnotateAnalysisUi::DrawContent` (945L/196br) — **largest; serial, careful, likely 3 extraction passes.** **(PR E1)**
33. `Source/Core/src/Ui/SmatchetPreferencesUi_Whisper.cpp:46` `DrawWhisperPreferencesTab` (781L/135br) — **0 perf scopes → invent seams.** **(PR E2)**
34. `Source/Core/src/Ui/SmatchetViewsDashboardUi.cpp:127` `drawViewsDashboardWindow` (778L/146br) + `SmatchetViewsDashboardUi_widgets.cpp:238` `DrawJqlQueryEditorEmbedded` (136L/36br). **(PR E3)**
35. `Source/Core/src/Ui/SmatchetUI.cpp:261` `SmatchetUI::Draw` (608L/147br) — 27 perf scopes → strong reusable seams. **(PR E4)**
36. `Source/Core/src/Ui/SmatchetOfflineQueueUi.cpp:562` `DrawUnifiedOfflineQueuesPanel` (606L/130br). **(PR E5, `offline-sync`)**
37. `Source/Core/src/Ui/SmatchetPreferencesUi_Assistant.cpp:41` `DrawAssistantPreferencesTab` (533L/87br) + `SmatchetPreferencesUi_Local.cpp:107` `DrawLocalAndAppearancePreferencesTabs` (264L/49br). **(PR E6)**
38. `Source/Core/src/Ui/SmatchetPreferencesUi_Templates.cpp:28` `DrawTemplatePreferencesTabs` (510L/78br). **(PR E7)**
39. `Source/Core/src/Ui/SmatchetUI_MainMenu.cpp:51` `drawMainMenuBar` (504L/123br) — per-menu helpers. **(PR E8)**
40. `Source/Core/src/Ui/SmatchetNewIssueDraftUi.cpp:155` `RenderNewIssueDraftRow` (467L/129br). **(PR E9, `grid-engine`)**
41. `Source/Core/src/Ui/SmatchetGridHeaderUi.cpp:42` `DrawGridHeaderToolbar` (447L/105br). **(PR E10, `grid-engine`)**
42. `Source/Core/src/Ui/SmatchetPreferencesUi.cpp:132` `drawPreferencesWindow` (403L/63br). **(PR E11)**
43. `Source/Core/src/Ui/SmatchetBulkTicketsUi.cpp:119` `drawBulkImportWindow` (399L/76br). **(PR E12)**
44. `Source/Core/src/Ui/SmatchetAiAssistantUi.cpp:386` `DrawHistoryArea` (315L/53br), `:984` `SmatchetDrawAiAssistantPanel` (287L/44br), `:763` `DrawInputAndButtons` (218L/21br). **(PR E13)**
45. `Source/Core/src/Ui/SmatchetToolbarUi.cpp:268` `RenderEditor` (288L/74br). **(PR E14)**
46. `Source/Core/src/Ui/SmatchetAttachmentPreviewUi.cpp:618` `drawAttachmentPreviewWindow` (278L/71br) — *(`ParseImageDimensions` already in PR A10)*. **(PR E15)**
47. `Source/Core/src/Ui/SmatchetBugReportUi.cpp:81` `SmatchetBugReportUi_Draw` (221L/50br). **(PR E16)**
48. `Source/Core/src/Ui/SmatchetAuditUi.cpp:202` `drawAuditWindow` (182L/35br), `SmatchetMcpServerUi.cpp:143` `SmatchetDrawMcpServerPanel` (156L/45br), `SmatchetUtilityWindowsUi.cpp:115` `drawLogWindow` (127L/34br), `SmatchetProjectPicker.cpp:46` `Draw` (132L/32br), `SmatchetAutocompleteUi.cpp:138` `TrackerQueryAcp_InputTextCallback` (118L/35br) — small over-cap draws, branch-cluster extraction. **(PR E17 — clustered; or split if diff exceeds CR file ceiling)**

### Closeout

49. `docs/high-integrity/function-size-baseline.md` — regenerate to "0 oversized" after the last batch (`--funcsize-baseline`).
50. `AGENTS.md` § Tiered enforcement + `decompose-top-20-monoliths.md` § Status — note the grandfather set is empty; optionally tee up the **soft-tier ratchet** (promote the 100-line/20-branch advisory to a hard cap) as a follow-up plan (out of scope here).

## Existing utilities reused

- `function_size_audit.py --list` / `--diff origin/develop` (`agents/scripts/core/`) — the live oversized inventory + the gate each PR must pass.
- `DrawCtx` + section-helper pattern (`docs/guides/imgui-draw-pattern.md`) — canonical ImGui recipe; every Phase-4/5 PR copies it.
- `SMATCHET_UI_PERF_SCOPE` (`Source/Core/include/UiPerfMonitor.h`) — existing scopes become section seams verbatim where present.
- Shipped exemplars to mirror: `RegisterAiCommands` split (#634, registrars), `BuildValue` table-dispatch (#633, field-type towers), `FetchFieldCatalog` phase-split (#643, fetch monoliths), `drawActiveProjectWindow` canary (#632, ImGui).
- Bucket-A test pattern (`tests/Core/*.test.cpp`) — pure helpers extracted in Phase 1/4 become directly testable; goldens captured **pre-refactor**.

## UX Pillar callouts

- **Pillar 1 (perf, 144 Hz / 6.94 ms)**: zero-impact where perf scopes are reused verbatim; any newly-added scope is an intentional `MARKER_INVENTORY.md`-tracked baseline change. Per-PR `perf-gatekeeper` diff on the matching scenario; > 0.2 ms regression = revert. Sync Tick paths (PR B10, E5) profiled before/after.
- **Pillar 2 (UI-thread never blocks > 100 ms)**: refactor is layout/dispatch reorganisation only — no new sync I/O reachable from `ImGui::*`. `MainThreadDispatcher` untouched.
- **Pillar 3 (never crash)**: each phase gated on dual-target build + sanitizer-clean for boot/HTTP/subprocess paths (PR B5, B7, C2/C3). Behaviour byte-for-byte preserved; positional-ImGui balance asserted (Begin/End, Push/Pop, BeginTable/EndTable counts identical to base).
- **Pillar 4 (accessibility)**: no change — hotkey dispatch + tab order preserved; no visual restyling.

## Perf-review-system gates (mandatory — diff touches `Source/Core/`)

Per `docs/plans/shipped/pillar-1-2-perf-review-system.md`. Applied **per PR**, not plan-wide.

1. **PR-fast CI** — each PR declares the scenario for its changed window/path (map in `agents/core/perf-gatekeeper.md`): e.g. PR E4 → `app-main-window-render`, E3 → `views-dashboard-render`, B10/E5 → the offline-queue scenario, A6 → `bulk-issue-fetch`. Pure-logic Phase-1 PRs with no per-frame path → declare `N/A — pure-logic, off the render thread` in the PR body.
2. **Pillar 2 static scanner** — N/A across all PRs; no new sync I/O reachable from `ImGui::*` (pure layout/dispatch reorg).
3. **Dispatcher drain** — N/A; `MainThreadDispatcher::Drain()` not touched.
4. **Visible-cue bucket-E harness** — N/A; no new > 100 ms sync-stall path introduced.
5. **Marker inventory** — regen `docs/perf/MARKER_INVENTORY.md` only in PRs that add a scope to a previously-scope-free draw (E2 Whisper, E3 ViewsDashboard, and any other 0-scope target).

**Pre-push local check**: each PR runs its named scenario per `docs/guides/perf-workflow.md` § Gate-check before opening.

**Override**: `perf-out-of-band` not anticipated — any PR needing it has gone wrong; halt and re-slice.

## Risks / non-goals

- **Risk (the headline): positional-ImGui regression in the 26-window Phase-5 sweep.** This is exactly the risk the parent plan avoided by keeping UI ride-along. Mitigation: each Phase-5 PR runs bucket-C screenshot diff + bucket-E interaction test against the **pre-refactor golden**; any pixel/interaction delta = revert. The canary (#632) proves this is tractable but it is the dominant cost-and-risk centre of the plan — sequenced last, one window per PR, never fanned out blindly.
- **Risk: churn vs in-flight UI feature work.** A 26-window sweep collides with any concurrent UI feature branch. Mitigation: serialize Phase 5 (don't parallel-dispatch all UI PRs); merge `develop` into each branch before merge (parent plan's CI-shallow-clone caveat — `tooling.md` P1); pause a window's decomposition if a feature PR is open on the same file.
- **Risk: branch-cap functions under the line cap need a different move.** ~15 functions (e.g. `ResolveRenderPlan` 84L/36br, `TokenToVirtualKey` 60L/33br) are only over the **branch** cap. Line-splitting won't help; they need table/dispatch extraction or sub-helper grouping of branch clusters. Mitigation: called out per-row in § Files.
- **Risk: branchy monoliths need 2nd/3rd extraction passes** (parent plan confirmed 3× on `FetchFieldCatalog`, the canary, `RegisterJsonRpcRoutes`). Expect it on every > 60-branch target (`DrawContent` 196br, `Draw` 147br, `drawViewsDashboardWindow` 146br, `DrawWhisperPreferencesTab` 135br, `DrawUnifiedOfflineQueuesPanel` 130br, `RenderNewIssueDraftRow` 129br). Budget for it.
- **Risk: `static`-local hoisting changes lifetime semantics** — identical for singleton windows (the Smatchet norm); an improvement for any future multi-instance window. Mitigation: grep + audit per PR, documented in the pattern doc.
- **Non-goal**: rewriting widget/parse logic. Layout/behaviour/state byte-for-byte preserved — pure mechanical decomposition.
- **Non-goal**: ratcheting the caps. Bringing the soft 100-line/20-branch advisory to a hard cap is a **follow-up plan** (noted in Closeout row 50), not this one. This plan only clears the *current* hard-cap set.
- **Non-goal**: touching `*_DX12` Unreal-only targets or ThirdParty (out of the audit scope already).

## Verification

Per `AGENTS.md` § Verification automation — zero manual steps where possible. **Per PR**, keyed to risk class:

- **Bucket A (pure-logic ctest, `test-rig`)**: Phase 1 (every Tracker parser/mapper/query fn + `CompactDateFormat`, `TicketGridModel`, `TokenToVirtualKey`, `ParseImageDimensions`) and Phase 4 Markdown (`EmitAdfBlock`/`EmitInlineText`/`ParseHtmlTag`) ship a test delta — one case per dispatch-table entry, goldens captured pre-refactor (mirrors `MarkdownConvert.test.cpp` #639, `BuildValue` #633). PR B9 covers `ValidateAiPrefs`.
- **Bucket E (ImGui Test Engine, `ninja-ui-test-msvc`)**: every Phase-3-UI (C4), Phase-4 editor/render (D1–D7), and **all** Phase-5 PRs gated on a screenshot-diff test vs the pre-refactor golden. Windows without existing bucket-E coverage ship the refactor + a `docs/self-improvement/categories/test.md` entry naming the missing scenario (no silent residue).
- **Bash-driver scenario / sanitizer**: PR B5 (subprocess), B7 (`main`/boot), C2/C3 (Whisper threads), B10/E5 (sync Tick) run ASan/UBSan on their path + the matching scenario.
- **Build gate**: every PR — `cmake --build --preset ninja-iter-msvc --target SmatchetStandalone SmatchetCore_DX12` (dual-target). Asserts preprocessor-guard balance for plugin PRs.
- **Gate self-check**: every PR runs `bash agents/scripts/project/test-lint-rules.sh --diff origin/develop` locally pre-push — confirms the targeted function dropped out of the oversized set and nothing new crossed.
- **Manual residue**: any window lacking bucket-E coverage → refactor ships + `test.md` deferral entry with the named scenario. No silent residue.

## Out of scope (flagged, not designed)

- **Soft-tier ratchet (100-line/20-branch → hard cap)** — follow-up plan, depends on this one zeroing the hard-cap set first. Noted in Closeout row 50.
- **`narrowing-conversions` / comment-bloat / other high-integrity rules** — orthogonal gates; not touched.
- **Retained-mode UI layer** — explicitly rejected by the parent plan; ImGui stays immediate-mode.
- **Refactoring functions under the caps but over the 40-80-line ideal** — the soft-warning tier nudges new code; no retroactive sweep of compliant-but-large functions.
- **`*_DX12` / Unreal plugin targets, ThirdParty** — outside the audit's SWEEP_ROOTS.

## Implementation log
*(populated post-ship per `AGENTS.md` § Plan revision after implementation — bullet per shipped commit: `<sha> · <one-line summary>`)*

## Deviations from plan
*(populated post-ship — what changed, removed, or deferred relative to the original plan, with one-line rationale per item)*

## Verification (actual)
*(populated post-ship — what was actually tested + result, passed / failed / not-run)*
