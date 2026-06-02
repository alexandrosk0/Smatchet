# Agent self-improvement — test

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-06-01 · orchestrator · [test] · P2 — `JiraFakeTrackerFixture` injects a pre-built catalog and bypasses `FetchFieldCatalog`, so field-classification / enrichment bugs have zero fixture coverage
  Details: The components-rendered-as-text bug (PR #672) was a `FetchFieldCatalog` defect — `components` stayed `Family=Text` (→ grid text editor instead of a multi-select dropdown) because the unscoped catalog fetch skips Phase-3 component enrichment AND `ClassifyTrackerFieldFamily` had no opts-independent case for components. No test caught it: `JiraFakeTrackerFixture` injects a **pre-built** `TrackerFieldCatalogResult` and bypasses the live `FetchFieldCatalog` assembly entirely, so the fake exercises catalog *consumption* but never catalog *build*. The whole enrichment + classification pipeline (scoped vs unscoped fetch, per-project component options, family-classification ordering) is fixture-invisible; the bug was found only by live debugging against issue BLUUP-1. Same blind spot would hide any future tracker catalog-build regression.
  Concrete next action: add a fixture variant that drives the real `FetchFieldCatalog` / `MergeProjectComponentsFromEndpoint` assembly against scripted HTTP responses (cpp-httplib already linked for fakes) so catalog-build classification can be asserted — e.g. an unscoped fetch leaves `components` dropdown-eligible, a scoped fetch populates per-project options, and `customfield_*` arrays classify per their schema. Pair with the pure `ClassifyTrackerFieldFamily` tests already added in #672. ~3-4 h (fixture design + first cases).
  Status: open
  Last-reviewed: 2026-06-01

- 2026-05-24 · test-author · [test] · P2 — `VerifyOnSave_TestConnection_SetsResult` bucket-E test fails under `--spawn` ephemeral runner (slice-9 ship-loop observation)
  Details: Slice 9 of `docs/plans/shipped/autonomous-debugging-no-creds.md` aggregate UI-test run (34/35 pass) revealed a single pre-existing failure: `VerifyOnSave_TestConnection_SetsResult` from `tests/ui/ai_prefs_autosave_flow.test.cpp:215`. The variant depends on `SmatchetActiveUiTestAppController()` returning a non-null `AppController*` so it can call `AiPrefsTestConnection::TriggerProbe`. Under `--spawn --ephemeral` the AppController seam is wired (other variants in the same TU pass), but the worker-thread `ProbeReachability` succeeds, then the result-callback dispatched to the main thread doesn't always run before the test budget (240 yields) expires. Sibling variant `VerifyOnSave_CancelOnClose_ShortCircuits` passes consistently because cancel-then-yield is deterministic. Not a slice-9 regression — slice-9's own 18 new variants all pass.
  Concrete next action: replace the 240-yield poll loop with a deterministic wait — either (a) drive the dispatcher tick from inside the test via `app.MainThreadDispatcher().DrainOnce()` after the worker join, or (b) gate `assistantPrefsTestInFlight=false` via a deterministic post-condition the test arms before TriggerProbe rather than waiting for the dispatched callback. ~1 h.
  Status: open
  Last-reviewed: 2026-05-24

- 2026-05-23 · debug-detective · [test] · P3 — No unit test for IsDescriptionLikeFieldId predicate (planned extraction in description-tooltip-consolidation)
  Details: `docs/plans/shipped/description-tooltip-consolidation.md` § Verification (Bucket A) calls for one test-rig case for `IsDescriptionLikeFieldId` covering `body`, `Body`, `description`, `customDescription`, `environment` (expected: true/true/true/true/false). The predicate does not yet exist as a named static helper — extraction is part of the consolidation plan (`Source/Core/src/TicketFieldEditor.cpp`). Without this test, the field-routing predicate can silently regress (e.g. losing the `body`/`Body` aliases used by GitHub tracker) after any rename or copy-paste.
  Concrete next action: after `description-tooltip-consolidation` ships, add `tests/Core/IsDescriptionLikeFieldId.test.cpp` with 5 cases: `"body"` → true, `"Body"` → true, `"description"` → true, `"customDescription"` → true (contains "description"), `"environment"` → false. Wire in `tests/CMakeLists.txt`. ~15 min.
  Status: open
  Last-reviewed: 2026-05-23

- 2026-05-23 · debug-detective · [test] · P2 — No automated gate prevents description tooltip "long thin strip" regression
  Details: Session 2026-05-23 `description-tooltip-consolidation` investigation. Adding `opts.wrapWidth = ImGui::GetFontSize() * 48.0f` to `RenderTextEditor`'s `BeginTooltip` block was confirmed only by hovering the description cell manually; no automated gate prevents the same regression. The symptom is severe: tooltip renders as an ultra-narrow vertical strip (~25 px wide) because `MarkdownPreviewRender::Render` samples `GetContentRegionAvail().x` internally, which is near-zero in a fresh `BeginTooltip()` window. The static grep gate (`scripts/dev/test-tooltip-wrapwidth.sh`, PR #430) catches missing `opts.wrapWidth` in source, but cannot verify the tooltip actually renders at the correct width at runtime.
  Concrete next action: add `tests/ui/grid_description_tooltip_markdown.test.cpp` (ImGui Test Engine, bucket-E) that (1) opens the active-project grid with a synthetic ticket whose `description` field contains multi-paragraph markdown (heading + code-fence + bullet list), (2) hovers the description cell via the test-engine cursor API to force `IsItemHovered() == true`, (3) waits one frame for the tooltip window to spawn, (4) asserts tooltip window width > `ImGui::GetFontSize() * 30.0f` (far from ultra-narrow), (5) asserts the tooltip child's `DrawList` contains more than one Y-distinct draw command (verifying multiple lines). Re-use the scaffold at `tests/ui/views_columns_reorder.test.cpp`. ~3 h once bucket-E gains stable column-hover support. Deferred-automation note carried from `docs/plans/shipped/description-tooltip-consolidation.md` § Verification.
  Status: open
  Last-reviewed: 2026-05-23

- 2026-05-21 · orchestrator · [test] · P3 — `tests/Core/SmatchetThemeSyntaxColors.test.cpp:163-188` mixes ImGui-coupled fixture into pure-logic bucket
  Details: CodeRabbit on PR #353 (post-merge feedback the session-poller missed; see process.md P1 'STALE CR review on timeout fallthrough') flagged that the `TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle — every theme populates the slice-6 Identifier syntax color")` test uses `ImGuiCtxFixture` (creates an ImGui context to call `SmatchetTheme::ApplyStyle` / `GetSyntaxColors`) but lives under `tests/Core/` — the pure-logic doctest bucket per `tests/CMakeLists.txt` § "Per-unit linkage". The bucket-boundary convention is: pure-logic tests in `tests/Core/` (no ImGui dep), bucket-E ImGui-Test-Engine scenarios in `tests/ui/`. The file already has 3 other `ImGuiCtxFixture`-using cases (the WindowBg pin tests at lines 190+), so the boundary violation is bucket-wide, not just one case.
  Concrete next action: two-step refactor — (1) Extract a pure-data accessor on `SmatchetTheme`: `static SmatchetThemeSyntaxColors BuildSyntaxColorsForTheme(ThemeId)` that returns the per-theme RGBA constants without going through `ApplyStyle()` (no ImGui side effects). The existing `ApplyStyle()` calls `BuildSyntaxColorsForTheme` internally + then publishes to the cached `GetSyntaxColors()` static. (2) Rewrite `SmatchetThemeSyntaxColors.test.cpp` cases to call `BuildSyntaxColorsForTheme` directly + drop the `ImGuiCtxFixture` dependency. The WindowBg / theme-switch cases that genuinely need ImGui style state can move to `tests/ui/theme_apply_window_bg.test.cpp` (bucket-E). ~2 h: 30 min extract, 1 h rewrite cases, 30 min bucket-E move. Low priority — current setup works correctly, this is convention hygiene.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-17 · test-author · [process] · P3 — test.md P2 item (4) "Save / Discard + 'Assistant *' dirty-tab label" describes a UI surface that was never shipped
  Details: `test.md` P2 item (4) (the AI Assistant Preferences batch 1 + 2 entry above) names "explicit Save / Discard buttons + 'Assistant *' dirty-tab label + Save-disabled-on-validation-error + tooltip-on-hover" as a flow needing bucket-E coverage. The Assistant tab is autosave (`MarkPrefsDirty` + ~100 ms debounce via `SmatchetUiSession.h:546-568` + `SmatchetUI.cpp:768-776`). There is NO Save button on the Assistant tab, NO Discard button, NO `Assistant *` dirty-tab label. The only "Save & Sync" button (`SmatchetPreferencesUi.cpp:1679`) is scoped to the Tracker + MCP tabs. Either ship explicit Save/Discard for the Assistant tab or amend test.md item (4) to describe autosave-and-verify. Cross-reference: `tests/ui/ai_prefs_autosave_flow.test.cpp` reframed per `docs/plans/shipped/ai-assistant-bucket-e-tus.md`.
  Concrete next action: triage decision — ship explicit Save/Discard (feature work, new PR) OR rewrite the item (4) bullet inside the P2 entry above to read "autosave debounce + Test-connection verify + cancel-on-close". ~10 min for the rewrite path.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [test] · P2 — Per-client cancel-abort-within-N-chunks regression test
  Details: PR #176's parser caps + `liveCancel` trust depend on the cpr WriteCallback contract (returning `false` aborts the in-flight request). If a future cpr / curl bump changes that semantic, cancel breaks silently with no test failure. Need a doctest per client (`OpenAi` / `Anthropic` / `Ollama`) that drives a fake HTTP server (cpp-httplib already linked) emitting a slow chunked stream, sets the cancel atom mid-stream, and asserts the client returns within K chunks with `WasCancelled = true`.
  Concrete next action: add `tests/Core/AiClientCancel.test.cpp` parameterised across the 3 clients; reuse the cpp-httplib server pattern from existing `MCP` tests. Estimated 2-3 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — Per-client error-body redaction regression test
  Details: 26d3b6f and PR #176 both fixed sibling-client redaction misses. The fix is per-client manual wiring with no test enforcing every `IAiClient` implementation routes through `RedactProviderErrorBody`. Need a regression gate: a doctest that drives each client against a fake server returning a 401 with an echoed `x-api-key` / `Authorization` header in the body, asserts the resulting `AiStreamError::Message` does not contain the literal key.
  Concrete next action: extend `AiClientCancel.test.cpp` (above) or new `AiClientErrorRedact.test.cpp` with one subcase per client. ~1.5 h on top of the cancel-test fixture.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [test] · P2 — Headless AiAssistant streaming scenarios (Scenarios 2/4/5) not yet covered
  Details: Phase B (PR #163) shipped the assistant panel + worker + Cancel but live-API verification scenarios from `docs/plans/shipped/ai-assistant-side-panel.md` § Verification — happy-path streaming (S2), 401 bad-key error path (S4), transport-down within 5s (S5) — are deferred to a `test-author` follow-up. The mechanism is a canned `httplib::Server` fixture (same scaffold as `DockGapSentinelScenario` from PR #146) driving `IAiClient::SendStreaming` directly + asserting on `g_ui.assistantHistory` + `g_ui.assistantLastError` + the cancel-atom poll cadence. Estimated 4 h (fixture + 3 scenario classes + bash driver + golden-event assertions). Same scaffold is reusable for Phase D (Anthropic + Ollama clients) verification.
  Concrete next action: add `tests/support/AiHttpFixture.h` + `Source/Core/src/Commands/Scenarios/AiAssistantSendScenario.cpp` against an in-process `httplib::Server` that emits canned SSE frames + 401 + transient-disconnect. Auto-enrol via `scripts/dev/test-ai-assistant.sh`.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — No automated coverage of `AiSseParser` (split-frame, `[DONE]`, malformed JSON, mid-frame cancel, `\r\n\r\n`)
  Details: Critical for Phase A' of the AI assistant work; deferral is in the originating commit message. The full SSE state machine has zero test surface.
  Concrete next action: verify the doctest TU lands as part of Phase A'. Estimated cost 1 h. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — `tests/Core/TicketSyncService.test.cpp:118-140` coverage gaps on empty-fetch guard
  Details: No test for the partial/error path (non-empty `FetchError` + `FullSyncCompleted=false` + empty `freshTickets`); no test asserting the guard is bypassed on legitimate non-empty diff.
  Concrete next action: add two cases covering the partial-error path and the bypass-on-non-empty-diff path. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — `scripts/dev/test-screenshot-diff.sh:24` `SMATCHET_TEST_PORT=58733` hardcoded; two parallel runs collide
  Details: Hard-coded port prevents parallel invocations (CI matrix, simultaneous local runs).
  Concrete next action: use ephemeral port or `$((40000 + RANDOM % 20000))`. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P3 — `Source/Plugins/Mcp/McpJsonRpcPure.cpp` anon-namespace helpers not exposed for Phase 5 dispatch tests
  Details: `BasenameForDisplay`, `TrimAsciiWhitespace`, `ToLowerAscii`, `AppendAllowlistedArgKvs` live in an anonymous namespace.
  Concrete next action: consider promoting to `pure::detail::` namespace so Phase 5 dispatch tests can reach them. Surfaced by retrospective code-review sweep on PR #141.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-15 · orchestrator · [test] · P3 — bucket-E coverage missing for inline Command Palette typing path
  Details: PR #79 fixed a bug where typing into the menu-bar inline palette input did not update the modal filter until Enter (return value of `InputTextWithHint` was gated by `ImGuiInputTextFlags_EnterReturnsTrue`, so `IsItemEdited()` was needed alongside `IsItemActivated() / committed`). Verified only manually. Bucket-E (`tests/ui/views_columns_reorder.test.cpp` shape) is the right home, but the inline-palette path drags `AppController` + `CommandRegistry` + `CommandPaletteUi` modal state into the test harness — heavier than the columns-reorder replica which only re-creates the loop body.
  Concrete next action: add `tests/ui/command_palette_inline_typing.test.cpp` that wraps a minimal `CommandRegistry` (one or two synthetic commands) and exercises the inline-input → modal-open → filter-applied path via `ItemInput` + assertion on `commandPalette_.FilterText()`. Surface a `FilterText()` accessor on `CommandPaletteUi` if not already present.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-24 · coderabbit-triage · [test] · P2 — FakeP4Runner timeout vs spawn-fail fixture semantics conflated
  Details: Slice 3 of `autonomous-debugging-no-creds.md` (PR #443) introduced `tests/support/FakeP4Runner.h` + fixture `tests/fixtures/p4/annotate_happy.json`. The fixture uses `exit_code = -1` as a *spawn-fail sentinel* (drives `P4RunCommand` to `return false`), but the same sentinel is reused for a `//depot/timeout.cpp` entry meant to model a p4 timeout. Real timeout behaviour in `Source/Core/src/P4Blame.cpp:65-80` populates stderr + exits via `return true` with non-zero `cap.exitCode` — i.e. completed-non-zero-exit, NOT spawn-fail. The test in `tests/Core/P4BlameAnnotateE2E.test.cpp:85` asserts the loose shape `err non-empty + rows empty`, which is satisfied by either path, so V3.1–V3.3 PASS even with the semantic conflation. Production P4Blame.cpp is correct; the bug is purely in the test fake.
  Concrete next action: route to `test-author`. Reshape `tests/support/FakeP4Runner.h` so spawn-fail vs completed-non-zero-exit are distinguishable in the fixture schema (e.g. add explicit `simulate: spawn_fail` boolean OR rewrite the timeout fixture to use a non-(-1) non-zero exit code like 124 + tighten the timeout test to assert stderr-contains-"timed out"). Touch only `tests/support/FakeP4Runner.h`, `tests/fixtures/p4/annotate_happy.json`, `tests/Core/P4BlameAnnotateE2E.test.cpp`. C++14 hard; no std::optional / std::variant. CR thread at https://github.com/alexandrosk0/Smatchet/pull/443#discussion_r3294396615 has the full triage notes.
  Status: open
  Last-reviewed: 2026-05-24
