#include "AiAssistantUiStateAdapter.h"

// AI-off (DX12/Unreal, mobile light, Smatchet-light): empty TU. The sole caller — AppController's
// `#if defined(SMATCHET_WITH_AI)` controller construction — is compiled out, so no definition is
// referenced, and UiDrawSession carries no `assistant*` fields to forward to.
#if defined(SMATCHET_WITH_AI)

#include "SmatchetUiSession.h" // UiDrawSession + the global g_ui

extern UiDrawSession g_ui;

namespace {

// Thin forwarder: every accessor returns the corresponding live g_ui field. Stateless, so the
// function-local static below is safe to share process-wide and outlives every dispatcher drain.
class GlobalAiAssistantUiState final : public IAiAssistantUiState {
  public:
    std::uint64_t AssistantTurnGen() const override { return g_ui.assistantTurnGen; }
    bool AssistantHistoryHydrated() const override { return g_ui.assistantHistoryHydrated; }
    int AssistantHistoryMaxRows() const override { return g_ui.cfg.AssistantHistoryMaxRows; }

    std::uint64_t BumpAssistantTurnGen() override { return ++g_ui.assistantTurnGen; }
    void RollbackAssistantTurnGen() override { --g_ui.assistantTurnGen; }

    std::string& AssistantStreamBuf() override { return g_ui.assistantStreamBuf; }
    std::vector<AiMessage>& AssistantHistory() override { return g_ui.assistantHistory; }
    std::vector<std::int64_t>& AssistantHistoryRowIds() override { return g_ui.assistantHistoryRowIds; }
    std::string& AssistantLastError() override { return g_ui.assistantLastError; }

    void SetAssistantInFlight(bool inFlight) override { g_ui.assistantInFlight = inFlight; }
};

} // namespace

IAiAssistantUiState& GetGlobalAiAssistantUiState() {
    static GlobalAiAssistantUiState instance; // process-lifetime — outlives controller + dispatcher
    return instance;
}

#endif // SMATCHET_WITH_AI
