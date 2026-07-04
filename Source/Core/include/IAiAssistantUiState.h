#pragma once

// Narrow seam over the 8 `g_ui` chat fields AiAssistantController touches (all on the UI thread,
// inside MainThreadDispatcher posts). Routing through it lets the controller link + run without the
// ImGui-bound UiDrawSession, so the streaming hand-off can run under ThreadSanitizer with a fake
// (docs/plans/active/tsan-imgui-linked-target.md, slice 2b). Accessors return the same mutable
// refs the controller already used, so its cap/flush/trim logic is preserved verbatim.
//
// LIFETIME CONTRACT: a posted lambda may drain AFTER the controller is destroyed (the dispatcher
// tasks hold no controller state), so the production impl must outlive it — bind to the
// process-lifetime GetGlobalAiAssistantUiState() (AiAssistantUiStateAdapter.h), never a member.

#include <cstdint>
#include <string>
#include <vector>

#include "AiTypes.h" // AiMessage

struct IAiAssistantUiState {
    virtual ~IAiAssistantUiState() = default;

    virtual std::uint64_t AssistantTurnGen() const = 0; // stale-callback guard
    virtual bool AssistantHistoryHydrated() const = 0;  // gates persist enqueues
    virtual int AssistantHistoryMaxRows() const = 0;    // cfg value; caller defaults <= 0

    /// Increments the turn-gen counter and returns the new value, mirroring
    /// `SmatchetAiAssistantUi.cpp`'s `++d.assistantTurnGen` at the panel Send site. Any
    /// caller that Submit()s a turn MUST bump through here (not read-only AssistantTurnGen())
    /// so the turn's own dispatcher callbacks compare equal against a live gen instead of
    /// being silently dropped as stale (CPP_CODE_AUDIT.md #33 — PromptAi turns were seeded
    /// from an unrelated 1<<32 counter that could never match this one).
    virtual std::uint64_t BumpAssistantTurnGen() = 0;
    /// Rolls back a bump from BumpAssistantTurnGen() when Submit() rejects the turn — mirrors
    /// the panel's `--d.assistantTurnGen` so a later successful Submit still lands on a
    /// monotonically-fresh value.
    virtual void RollbackAssistantTurnGen() = 0;

    virtual std::string& AssistantStreamBuf() = 0;
    virtual std::vector<AiMessage>& AssistantHistory() = 0;
    virtual std::vector<std::int64_t>& AssistantHistoryRowIds() = 0;
    virtual std::string& AssistantLastError() = 0;

    virtual void SetAssistantInFlight(bool inFlight) = 0;
};
