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

    virtual std::uint64_t AssistantTurnGen() const = 0;       // stale-callback guard
    virtual bool AssistantHistoryHydrated() const = 0;        // gates persist enqueues
    virtual int AssistantHistoryMaxRows() const = 0;          // cfg value; caller defaults <= 0

    virtual std::string& AssistantStreamBuf() = 0;
    virtual std::vector<AiMessage>& AssistantHistory() = 0;
    virtual std::vector<std::int64_t>& AssistantHistoryRowIds() = 0;
    virtual std::string& AssistantLastError() = 0;

    virtual void SetAssistantInFlight(bool inFlight) = 0;
};
