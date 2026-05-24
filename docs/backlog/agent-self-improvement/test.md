# Agent self-improvement — test

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

- 2026-05-23 · debug-detective · [test] · P3 — No unit test for IsDescriptionLikeFieldId predicate (planned extraction in description-tooltip-consolidation)
  Details: `docs/design/description-tooltip-consolidation.md` § Verification (Bucket A) calls for one test-rig case for `IsDescriptionLikeFieldId` covering `body`, `Body`, `description`, `customDescription`, `environment` (expected: true/true/true/true/false). The predicate does not yet exist as a named static helper — extraction is part of the consolidation plan (`Source_Core/src/TicketFieldEditor.cpp`). Without this test, the field-routing predicate can silently regress (e.g. losing the `body`/`Body` aliases used by GitHub tracker) after any rename or copy-paste.
  Concrete next action: after `description-tooltip-consolidation` ships, add `tests/Source_Core/IsDescriptionLikeFieldId.test.cpp` with 5 cases: `"body"` → true, `"Body"` → true, `"description"` → true, `"customDescription"` → true (contains "description"), `"environment"` → false. Wire in `tests/CMakeLists.txt`. ~15 min.
  Status: open
  Last-reviewed: 2026-05-23

- 2026-05-23 · debug-detective · [test] · P2 — No automated gate prevents description tooltip "long thin strip" regression
  Details: Session 2026-05-23 `description-tooltip-consolidation` investigation. Adding `opts.wrapWidth = ImGui::GetFontSize() * 48.0f` to `RenderTextEditor`'s `BeginTooltip` block was confirmed only by hovering the description cell manually; no automated gate prevents the same regression. The symptom is severe: tooltip renders as an ultra-narrow vertical strip (~25 px wide) because `MarkdownPreviewRender::Render` samples `GetContentRegionAvail().x` internally, which is near-zero in a fresh `BeginTooltip()` window. The static grep gate (`scripts/dev/test-tooltip-wrapwidth.sh`, PR #430) catches missing `opts.wrapWidth` in source, but cannot verify the tooltip actually renders at the correct width at runtime.
  Concrete next action: add `tests/ui/grid_description_tooltip_markdown.test.cpp` (ImGui Test Engine, bucket-E) that (1) opens the active-project grid with a synthetic ticket whose `description` field contains multi-paragraph markdown (heading + code-fence + bullet list), (2) hovers the description cell via the test-engine cursor API to force `IsItemHovered() == true`, (3) waits one frame for the tooltip window to spawn, (4) asserts tooltip window width > `ImGui::GetFontSize() * 30.0f` (far from ultra-narrow), (5) asserts the tooltip child's `DrawList` contains more than one Y-distinct draw command (verifying multiple lines). Re-use the scaffold at `tests/ui/views_columns_reorder.test.cpp`. ~3 h once bucket-E gains stable column-hover support. Deferred-automation note carried from `docs/design/description-tooltip-consolidation.md` § Verification.
  Status: open
  Last-reviewed: 2026-05-23

- 2026-05-21 · orchestrator · [test] · P3 — `tests/Source_Core/SmatchetThemeSyntaxColors.test.cpp:163-188` mixes ImGui-coupled fixture into pure-logic bucket
  Details: CodeRabbit on PR #353 (post-merge feedback the session-poller missed; see process.md P1 'STALE CR review on timeout fallthrough') flagged that the `TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle — every theme populates the slice-6 Identifier syntax color")` test uses `ImGuiCtxFixture` (creates an ImGui context to call `SmatchetTheme::ApplyStyle` / `GetSyntaxColors`) but lives under `tests/Source_Core/` — the pure-logic doctest bucket per `tests/CMakeLists.txt` § "Per-unit linkage". The bucket-boundary convention is: pure-logic tests in `tests/Source_Core/` (no ImGui dep), bucket-E ImGui-Test-Engine scenarios in `tests/ui/`. The file already has 3 other `ImGuiCtxFixture`-using cases (the WindowBg pin tests at lines 190+), so the boundary violation is bucket-wide, not just one case.
  Concrete next action: two-step refactor — (1) Extract a pure-data accessor on `SmatchetTheme`: `static SmatchetThemeSyntaxColors BuildSyntaxColorsForTheme(ThemeId)` that returns the per-theme RGBA constants without going through `ApplyStyle()` (no ImGui side effects). The existing `ApplyStyle()` calls `BuildSyntaxColorsForTheme` internally + then publishes to the cached `GetSyntaxColors()` static. (2) Rewrite `SmatchetThemeSyntaxColors.test.cpp` cases to call `BuildSyntaxColorsForTheme` directly + drop the `ImGuiCtxFixture` dependency. The WindowBg / theme-switch cases that genuinely need ImGui style state can move to `tests/ui/theme_apply_window_bg.test.cpp` (bucket-E). ~2 h: 30 min extract, 1 h rewrite cases, 30 min bucket-E move. Low priority — current setup works correctly, this is convention hygiene.
  Status: open
  Last-reviewed: 2026-05-21

- 2026-05-20 · orchestrator · [test] · P2 — Bucket-E coverage missing for description grid-cell tooltip rendering markdown
  Details: `TicketFieldEditor::renderPlainText` (`Source_Core/src/TicketFieldEditor.cpp:890-906`) + the saving-state mirror (`SmatchetActiveProjectGridUi.cpp:899-911`) gate the tooltip source on `isDescriptionField`. A regression in the past hour passed `tip = nullptr` for the description column, which collapsed the tooltip text to `ResolveDisplayValue`'s flattened single-line form and silently disabled the `MarkdownPreviewRender::Mode::Tooltip` branch in `SmatchetFieldRender.cpp:53-70`. Pure-logic doctest cannot exercise it because the bug surfaces only inside an ImGui frame with hover state + the markdown preview pipeline mounted (heading-scale, code-block fonts, link-disable, wrap-pos). Fix in this commit was confirmed visually by hovering the description cell; no automated gate prevents the same regression next time. Pillar-4 visual-validation exception fired during ship.
  Concrete next action: add `tests/ui/grid_description_tooltip_markdown.test.cpp` (ImGui Test Engine, bucket-E) that (1) opens the active-project grid with a single ticket whose `description` field contains a mixed-block markdown string (heading + bold + code-fence + bulletList), (2) hovers the description cell via the test-engine cursor API to force `IsItemHovered() == true`, (3) waits one frame for the tooltip window to spawn, (4) asserts the tooltip child contains the heading-block draw (verifiable via `ImGuiTestEngine` window-name lookup + the preview pipeline's named text items), and (5) snapshots the tooltip rect for the screenshot-diff lane. Re-use the scenario scaffold at `tests/ui/views_columns_reorder.test.cpp`. ~3 h once bucket-E is unblocked; depends on the same upstream bucket-E infrastructure as the AI Assistant Preferences entry below.
  Status: open
  Last-reviewed: 2026-05-20

- 2026-05-19 · coderabbit-triage · [test] · P2 — `AgentProposalStore.test.cpp` SQLite tests live in pure-logic rig; no bucket-E SQLite lane exists yet
  Details: CodeRabbit findings #15 + #16 on PR #264 (handoff-half re-review) flagged `tests/Source_Core/AgentProposalStore.test.cpp` as violating the "tests/Source_Core/** must be pure-logic doctest only" rule — `test-rig` agent contract refuses SQLite surfaces. The tests were knowingly authored under the H10 plan as integration-coverage in the doctest rig because no bucket-E SQLite harness exists today. Moving them now has no destination lane. Triage marked DEFERRED rather than rejected because the suggestion's intent (proper lane separation) is correct; the blocker is missing infrastructure.
  Concrete next action: design a bucket-E SQLite lane — preset `ninja-test-sqlite-msys2` or extend bucket-E with SQLite-allowed test TUs. Move `AgentProposalStore.test.cpp` + any future SQLite-backed tests there. Until then, the doctest-rig location is the documented exception. ~4 h for the lane design + initial migration.
  Status: open
  Last-reviewed: 2026-05-19

- 2026-05-18 · orchestrator · [test] · P3 — CodeRabbit "✅ Addressed in commit X" notation gives false confidence
  Details: On PR #250 follow-up triage, CodeRabbit flagged 5 findings with 2 marked `✅ Addressed in commits 5e7d75b to 29c3321` / `✅ Addressed in commit 85aa69f`. Verified against the actual merged code: the Test E2E mode-routing was genuinely fixed (✓), but the banner `ImGuiCol_TextDisabled` push was NOT — the merged banner still pushed 3 colors (`WindowBg / Border / Text`), missing `TextDisabled`. CR's check matched commit message keywords rather than the diff actually addressing the finding. Acting on the green check alone would have left a known low-contrast bug shipped.
  Concrete next action: standing process rule — when CR labels a finding as addressed, ALWAYS read the cited commit's diff against the original finding to confirm the change matches the requested fix. Add a one-liner to `docs/agent-rules/REVIEW_TRIAGE.md` (or the CodeRabbit-handling section of `AGENTS.md`) so future orchestrators don't trust the green-check blindly. ~10 min doc edit.
  Status: open
  Last-reviewed: 2026-05-18

- 2026-05-17 · test-author · [process] · P3 — test.md P2 item (4) "Save / Discard + 'Assistant *' dirty-tab label" describes a UI surface that was never shipped
  Details: `test.md` P2 item (4) (the AI Assistant Preferences batch 1 + 2 entry above) names "explicit Save / Discard buttons + 'Assistant *' dirty-tab label + Save-disabled-on-validation-error + tooltip-on-hover" as a flow needing bucket-E coverage. The Assistant tab is autosave (`MarkPrefsDirty` + ~100 ms debounce via `SmatchetUiSession.h:546-568` + `SmatchetUI.cpp:768-776`). There is NO Save button on the Assistant tab, NO Discard button, NO `Assistant *` dirty-tab label. The only "Save & Sync" button (`SmatchetPreferencesUi.cpp:1679`) is scoped to the Tracker + MCP tabs. Either ship explicit Save/Discard for the Assistant tab or amend test.md item (4) to describe autosave-and-verify. Cross-reference: `tests/ui/ai_prefs_autosave_flow.test.cpp` reframed per `docs/design/ai-assistant-bucket-e-tus.md`.
  Concrete next action: triage decision — ship explicit Save/Discard (feature work, new PR) OR rewrite the item (4) bullet inside the P2 entry above to read "autosave debounce + Test-connection verify + cancel-on-close". ~10 min for the rewrite path.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — Bucket-E coverage missing for AI Assistant Preferences batch 1 + batch 2 user-visible flows
  Details: PRs #181 (`assistant UX fixes batch 1`) + #184 (`assistant Preferences batch 2`) shipped six discrete user-visible flows that have zero automated coverage because bucket-E (ImGui Test Engine) is not yet wired. Doctest coverage of the pure layers (`AiPrefsValidator` 19/19, `AgentsMdLoader` 15/15) passes but the UI integration is verified manually. Specific surfaces awaiting bucket-E: (1) Assistant panel dock-left default + swap-side button + auto-reveal of secondary side bar when toggled; (2) Enter sends + Ctrl+Enter inserts newline + post-submit `SetKeyboardFocusHere(-1)` focus restore; (3) sticky validation banner rendered above scrollable content + per-field `(!)` / `(~)` glyphs + tooltip with the issue message; (4) explicit Save / Discard buttons + `"Assistant *"` dirty-tab label + Save-disabled-on-validation-error + tooltip-on-hover; (5) Test connection button (async via `std::thread` + `MainThreadDispatcher`, cancel-on-window-close via `assistantPrefsTestCancel` atom); (6) verify-on-save Save path (probes before commit, toasts `"Saved + verified"` on success, `"Connection failed — not saved"` on failure). All six rely on dock state, async dispatch, and tab-bar coloring that doctest cannot exercise.
  Concrete next action: depends on the upstream `bucket-E` infrastructure (currently blocked by the `--spawn` timeout regression at the top of `tooling.md` — fix that first). Once unblocked, add `tests/ui/ai_assistant_panel_dock_swap.test.cpp`, `tests/ui/ai_assistant_enter_send.test.cpp`, `tests/ui/ai_prefs_save_flow.test.cpp` mirroring the `views_columns_reorder.test.cpp` shape. Estimated ~6-8 h across the three TUs once bucket-E unblocks.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · security-review · [test] · P2 — Per-client cancel-abort-within-N-chunks regression test
  Details: PR #176's parser caps + `liveCancel` trust depend on the cpr WriteCallback contract (returning `false` aborts the in-flight request). If a future cpr / curl bump changes that semantic, cancel breaks silently with no test failure. Need a doctest per client (`OpenAi` / `Anthropic` / `Ollama`) that drives a fake HTTP server (cpp-httplib already linked) emitting a slow chunked stream, sets the cancel atom mid-stream, and asserts the client returns within K chunks with `WasCancelled = true`.
  Concrete next action: add `tests/Source_Core/AiClientCancel.test.cpp` parameterised across the 3 clients; reuse the cpp-httplib server pattern from existing `MCP` tests. Estimated 2-3 h.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — Per-client error-body redaction regression test
  Details: 26d3b6f and PR #176 both fixed sibling-client redaction misses. The fix is per-client manual wiring with no test enforcing every `IAiClient` implementation routes through `RedactProviderErrorBody`. Need a regression gate: a doctest that drives each client against a fake server returning a 401 with an echoed `x-api-key` / `Authorization` header in the body, asserts the resulting `AiStreamError::Message` does not contain the literal key.
  Concrete next action: extend `AiClientCancel.test.cpp` (above) or new `AiClientErrorRedact.test.cpp` with one subcase per client. ~1.5 h on top of the cancel-test fixture.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · test-author · [test] · P2 — Headless AiAssistant streaming scenarios (Scenarios 2/4/5) not yet covered
  Details: Phase B (PR #163) shipped the assistant panel + worker + Cancel but live-API verification scenarios from `docs/design/ai-assistant-side-panel.md` § Verification — happy-path streaming (S2), 401 bad-key error path (S4), transport-down within 5s (S5) — are deferred to a `test-author` follow-up. The mechanism is a canned `httplib::Server` fixture (same scaffold as `DockGapSentinelScenario` from PR #146) driving `IAiClient::SendStreaming` directly + asserting on `g_ui.assistantHistory` + `g_ui.assistantLastError` + the cancel-atom poll cadence. Estimated 4 h (fixture + 3 scenario classes + bash driver + golden-event assertions). Same scaffold is reusable for Phase D (Anthropic + Ollama clients) verification.
  Concrete next action: add `tests/support/AiHttpFixture.h` + `Source_Core/src/Commands/Scenarios/AiAssistantSendScenario.cpp` against an in-process `httplib::Server` that emits canned SSE frames + 401 + transient-disconnect. Auto-enrol via `scripts/dev/test-ai-assistant.sh`.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — No automated coverage of `AiSseParser` (split-frame, `[DONE]`, malformed JSON, mid-frame cancel, `\r\n\r\n`)
  Details: Critical for Phase A' of the AI assistant work; deferral is in the originating commit message. The full SSE state machine has zero test surface.
  Concrete next action: verify the doctest TU lands as part of Phase A'. Estimated cost 1 h. Surfaced by retrospective code-review sweep on PR #140.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — `tests/Source_Core/TicketSyncService.test.cpp:118-140` coverage gaps on empty-fetch guard
  Details: No test for the partial/error path (non-empty `FetchError` + `FullSyncCompleted=false` + empty `freshTickets`); no test asserting the guard is bypassed on legitimate non-empty diff.
  Concrete next action: add two cases covering the partial-error path and the bypass-on-non-empty-diff path. Surfaced by retrospective code-review sweep on PR #139.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P2 — `scripts/dev/test-screenshot-diff.sh:24` `SMATCHET_TEST_PORT=58733` hardcoded; two parallel runs collide
  Details: Hard-coded port prevents parallel invocations (CI matrix, simultaneous local runs).
  Concrete next action: use ephemeral port or `$((40000 + RANDOM % 20000))`. Surfaced by retrospective code-review sweep on PR #146.
  Status: open
  Last-reviewed: 2026-05-17

- 2026-05-17 · code-review · [test] · P3 — `Plugins/Mcp/McpJsonRpcPure.cpp` anon-namespace helpers not exposed for Phase 5 dispatch tests
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
  Details: Slice 3 of `autonomous-debugging-no-creds.md` (PR #443) introduced `tests/support/FakeP4Runner.h` + fixture `tests/fixtures/p4/annotate_happy.json`. The fixture uses `exit_code = -1` as a *spawn-fail sentinel* (drives `P4RunCommand` to `return false`), but the same sentinel is reused for a `//depot/timeout.cpp` entry meant to model a p4 timeout. Real timeout behaviour in `Source_Core/src/P4Blame.cpp:65-80` populates stderr + exits via `return true` with non-zero `cap.exitCode` — i.e. completed-non-zero-exit, NOT spawn-fail. The test in `tests/Source_Core/P4BlameAnnotateE2E.test.cpp:85` asserts the loose shape `err non-empty + rows empty`, which is satisfied by either path, so V3.1–V3.3 PASS even with the semantic conflation. Production P4Blame.cpp is correct; the bug is purely in the test fake.
  Concrete next action: route to `test-author`. Reshape `tests/support/FakeP4Runner.h` so spawn-fail vs completed-non-zero-exit are distinguishable in the fixture schema (e.g. add explicit `simulate: spawn_fail` boolean OR rewrite the timeout fixture to use a non-(-1) non-zero exit code like 124 + tighten the timeout test to assert stderr-contains-"timed out"). Touch only `tests/support/FakeP4Runner.h`, `tests/fixtures/p4/annotate_happy.json`, `tests/Source_Core/P4BlameAnnotateE2E.test.cpp`. C++14 hard; no std::optional / std::variant. CR thread at https://github.com/alexandrosk0/Smatchet/pull/443#discussion_r3294396615 has the full triage notes.
  Status: open
  Last-reviewed: 2026-05-24
