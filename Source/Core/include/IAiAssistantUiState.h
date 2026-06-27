#pragma once

// IAiAssistantUiState — the narrow seam over the 8 `g_ui` (UiDrawSession) chat fields that
// AiAssistantController reads/writes. Every access happens on the UI thread, inside a
// MainThreadDispatcher post; routing them through this interface lets the controller be built
// and exercised WITHOUT linking the ImGui-bound UiDrawSession, so the streaming worker→UI
// hand-off can run under ThreadSanitizer (docs/plans/active/tsan-imgui-linked-target.md, slice 2b).
//
// Thin by design (the "thin field-accessor" shape): the accessors return mutable references to the
// same containers/strings the controller already manipulated, so the controller keeps its exact
// cap / flush / trim logic verbatim — the rewire is a pure `g_ui.assistantX` → `ui.AssistantX()`
// substitution with no behaviour change.
//
// LIFETIME CONTRACT (critical): a posted lambda may drain AFTER the AiAssistantController is
// destroyed (see AiAssistantController.cpp — the dispatcher tasks deliberately hold no
// controller/worker state). The production implementation MUST therefore be at least as long-lived
// as the global `g_ui` it forwards to — bind the controller to the process-lifetime adapter from
// GetGlobalAiAssistantUiState() (AiAssistantUiStateAdapter.h), never to a controller-owned object.

#include <cstdint>
#include <string>
#include <vector>

#include "AiTypes.h" // AiMessage

struct IAiAssistantUiState {
    virtual ~IAiAssistantUiState() = default;

    // --- reads ---
    /// Generation of the in-flight turn; a posted callback whose captured turnGen no longer
    /// matches is stale (Cancel / a newer Submit raced it) and must drop its mutation.
    virtual std::uint64_t AssistantTurnGen() const = 0;
    /// True once the SQLite chat history has been hydrated; gates persist enqueues so a
    /// final-flush during the hydrate window does not write a duplicate row.
    virtual bool AssistantHistoryHydrated() const = 0;
    /// cfg.AssistantHistoryMaxRows verbatim (the caller applies its own `> 0 ? : default`).
    virtual int AssistantHistoryMaxRows() const = 0;

    // --- mutable container/string refs (the thin seam — existing call sites work unchanged) ---
    virtual std::string& AssistantStreamBuf() = 0;
    virtual std::vector<AiMessage>& AssistantHistory() = 0;
    virtual std::vector<std::int64_t>& AssistantHistoryRowIds() = 0;
    virtual std::string& AssistantLastError() = 0;

    // --- scalar writes ---
    virtual void SetAssistantInFlight(bool inFlight) = 0;
};
