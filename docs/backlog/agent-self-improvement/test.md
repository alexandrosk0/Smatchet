# Agent self-improvement — test

> Format / categories / workflow / priority / triage: see
> [`../AGENT_SELF_IMPROVEMENT.md`](../AGENT_SELF_IMPROVEMENT.md) (index + spec).
> Sibling categories: bug · process · tooling · infra · test · security · external-blockers · applied.
> Live entries only. `applied` entries archive immediately to `applied.md`.

<!-- Latest first. Append new entries at the top. -->

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
