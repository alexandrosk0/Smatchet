# AI-chat bucket-E: input widget unreachable in the headless docked panel (blocks scenarios 4–5)

- **Date**: 2026-07-12
- **Author**: orchestrator
- **Category**: test
- **Priority**: P2

## What

The AI-chat panel bucket-E suite (`tests/ui/ai_chat_panel.test.cpp`) shipped **3 of the 5**
mandatory scenarios — `ClearConversation_ConfirmWipesCancelKeeps` (#1796),
`CopyMessage_WritesContentToClipboard` (#1799), `PinBookmark_ActionRowTogglesPinnedState`
(#1802). The remaining two — **keyboard-nav** and **history-persist** — are **blocked by a
harness limitation, not by test-authoring effort**, and were NOT shipped rather than ship a
vacuously-skipping test (coverage theater).

## The blocker (verified)

Both remaining scenarios must drive the panel's **message input** (`##AiAssistantInput`, an
`InputTextMultiline` at the bottom of the panel): keyboard-nav needs to type + press Enter;
history-persist needs to send a turn so the persist worker writes it to SQLite.

In the headless bucket-E run the panel is **docked into a sidebar** (it acquires a `DockId`,
same as `ai_assistant_panel_dock_swap.test.cpp` observes). At the dockspace's default
sidebar size the input row is **clipped below the fold and never submitted to the ImGui item
table**, so `ctx->ItemExists("**/##AiAssistantInput")` returns **false** and `ctx->ItemInput`
spins the whole frame budget (the run times out). Verified directly: a diagnostic
`IM_CHECK(ctx->ItemExists("**/##AiAssistantInput"))` fails with
`SKIP: ##AiAssistantInput not reachable in this docked layout`.

The already-shipped 3 scenarios only touch the **header** (clear button) and the **history
child** (copy / pin action rows), which lay out at the TOP of the panel and are reachable —
the copy scenario's single-pinned-turn trick keeps that row on-screen. The input is the one
surface at the BOTTOM, past the reserved history height, that the short docked panel clips.

## Concrete unblock options (pick one before authoring 4–5)

1. **Float the panel at a test-controlled size.** Add a test seam so the panel opens
   undocked (e.g. a `g_ui`/config flag the bucket-E boot sets, or a
   `SmatchetActiveUiTestFloatAssistantPanel()` hook) and `ctx->WindowResize` it to a size
   that fits header + a short history + the input. `WindowResize` is a no-op on a **docked**
   window (that was tried and fails), so the panel must be floating first.
2. **Direct input-focus + programmatic submit seam.** Expose a tiny test-only entry point
   (mirroring `SmatchetActiveUiTestAppController()`) that focuses `##AiAssistantInput` and/or
   invokes the Enter-submit path (`DispatchAiSend` via the same `enterSubmitted` branch)
   without needing the widget on-screen. Keeps the assertion on the real send flow (the
   first-send **outbound-consent gate** — `assistantConsentRows` / `##AiOutboundConsent` —
   is a deterministic no-network observable that Enter submitted).
3. **Give the dockspace a taller assistant node in the bucket-E layout** so the input is
   never clipped, then drive `ItemInput` + `KeyChars` + `KeyPress(Enter)` normally.

## Scenario sketches (ready once unblocked)

- **keyboard-nav** — `SMATCHET_WITH_AI` is ON in `ninja-ui-test-msvc`, so the
  `AiAssistantController` exists and the send path is live. Type a prompt, press Enter, assert
  the first-send consent gate fires (`assistantConsentRows` non-empty / `##AiOutboundConsent`
  live) — **no network** (the gate intercepts before any POST). Force it with
  `cfg.AssistantOutboundConsentShown = false`. Ctrl+Enter-inserts-newline is the complementary
  half.
- **history-persist** — send a turn (past consent), let `SmatchetChatPersistWorker` flush to
  SQLite, then re-hydrate (`LoadAiChatMessages`) and assert the turn + its row-id round-trip.
  Needs a clean `SMATCHET_USER_DATA` tmp DB (the existing fixture-gated pattern).

## Status

**RESOLVED 2026-07-13** — unblocked via **option 1** the same day it was recorded. A new
`OpenAssistantPanelWithInput` test helper `ctx->UndockWindow`s the "Smatchet Assistant" panel to
a floating window then `ctx->WindowResize`s it to 520×700, so the bottom `##AiAssistantInput` row
is no longer clipped off the docked sidebar (`WindowResize` is a no-op on a docked window — the
undock must precede it; confirmed with a diagnostic `IM_CHECK(ItemExists("**/##AiAssistantInput"))`).
Both remaining scenarios then shipped in `tests/ui/ai_chat_panel.test.cpp`:
**keyboard-nav** (`KeyboardEnter_SubmitsThroughConsentGate` — bare Enter → offline first-send
consent gate) and **history-persist** (`HistoryPersist_AppendRoundTripsThroughSqlite` — drives
`chat_persist::EnqueueAppendAndTrim` → `LoadAiChatMessages` directly, no send/network needed).
`ui_test.run --name=AiChat --spawn` → **5/5**. All 5 mandatory ai-chat-claude-desktop-parity
scenarios are now covered.

_Was:_ open (3 of 5 scenarios shipped; 2 blocked on the input-reachability harness seam above — a
concrete deferred-automation plan, not a flat "out of scope").
