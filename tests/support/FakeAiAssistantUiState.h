// FakeAiAssistantUiState — header-only IAiAssistantUiState for the TSan hand-off test. Owns the 8
// chat fields as plain members so the AiAssistantController can be exercised without g_ui/ImGui.
// All access is on the "UI" (draining) thread inside posted lambdas, mirroring production.

#ifndef SMATCHET_TESTS_FAKE_AI_ASSISTANT_UI_STATE_H
#define SMATCHET_TESTS_FAKE_AI_ASSISTANT_UI_STATE_H

#include "IAiAssistantUiState.h"

#include <cstdint>
#include <string>
#include <vector>

namespace smatchet_tests {

class FakeAiAssistantUiState : public IAiAssistantUiState {
  public:
    std::uint64_t AssistantTurnGen() const override { return turnGen; }
    bool AssistantHistoryHydrated() const override { return hydrated; }
    int AssistantHistoryMaxRows() const override { return maxRows; }

    std::uint64_t BumpAssistantTurnGen() override { return ++turnGen; }
    void RollbackAssistantTurnGen() override { --turnGen; }

    std::string& AssistantStreamBuf() override { return streamBuf; }
    std::vector<AiMessage>& AssistantHistory() override { return history; }
    std::vector<std::int64_t>& AssistantHistoryRowIds() override { return historyRowIds; }
    std::string& AssistantLastError() override { return lastError; }

    void SetAssistantInFlight(bool v) override { inFlight = v; }

    // Test-visible state (read only after the worker is joined + the final Drain).
    std::uint64_t turnGen = 0;
    bool hydrated = false;
    int maxRows = 500;
    std::string streamBuf;
    std::vector<AiMessage> history;
    std::vector<std::int64_t> historyRowIds;
    std::string lastError;
    bool inFlight = false;
};

} // namespace smatchet_tests

#endif
