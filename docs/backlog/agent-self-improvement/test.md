# Agent self-improvement — test

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

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

- 2026-05-17 · code-review · [test] · P1 — `scripts/dev/test-screenshot-diff.sh:104` `sleep 0.5` race between spawn-mode CLI return and PPM read
  Details: Hard-coded sleep races against actual capture-write completion. On slow CI runners the PPM may not be flushed when the read attempts.
  Concrete next action: replace with `for i in {1..40}; do [ -s "$captured" ] && break; sleep 0.05; done` deterministic wait. Surfaced by retrospective code-review sweep on PR #146.
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
