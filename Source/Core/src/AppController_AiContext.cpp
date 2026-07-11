// AppController_AiContext.cpp — AI-context cluster extracted from
// AppController.cpp (behavior-preserving TU split, plan
// docs/plans/active/appcontroller-clusters-followup.md). Method DECLARATIONS stay in
// AppController.h; only the definitions moved, so linkage and behavior are identical.
// The four methods are always-on (unconditional signatures, so Lua glue and the
// SMATCHET_WITH_AI=OFF build link identically); each body internally guards its
// delegation to the pImpl-owned AiAssistantController with SMATCHET_WITH_AI and
// no-ops otherwise, matching the pre-move layout. Includes are curated from what
// the moved bodies actually use; AppControllerImpl.h is needed because the bodies
// dereference impl_->aiAssistant_. No winsock preamble — this TU pulls no cpr/curl header.
// clang-format off
// SMATCHET_DEVIATION(rule=app-controller-fan-in; reason=behavior-preserving TU split of AppController.cpp, a companion TU defining the AppController AI-context methods needs the full class definition and adds no new coupling; owner=orchestrator; revisit=when AppController.h is narrowed per ADR-0020 / debt.md)
#include "AppController.h"
// clang-format on
#include "AppControllerImpl.h"

#include "Logger.h"

#include <cstdint>
#include <string>
#include <vector>

#if defined(SMATCHET_WITH_AI)
#include "AiAssistantController.h"
#include "AiAssistantUiStateAdapter.h"
#endif

void AppController::AddAiContext(const AiContextBlock& block) {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        impl_->aiAssistant_->AddAiContext(block);
    }
#else
    (void)block;
#endif
}

void AppController::ClearAiContext() {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        impl_->aiAssistant_->ClearAiContext();
    }
#endif
}

std::vector<AiContextBlock> AppController::GetAiContext() const {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        return impl_->aiAssistant_->GetAiContext();
    }
#endif
    return {};
}

void AppController::PromptAi(const std::string& prompt) {
#if defined(SMATCHET_WITH_AI)
    if (impl_->aiAssistant_) {
        // CPP_CODE_AUDIT.md #33 (PromptAi turns always discarded): route through the SAME
        // `g_ui.assistantTurnGen` counter the chat panel's own Send path bumps, via the
        // GetGlobalAiAssistantUiState() seam — not a process-local counter seeded far above
        // any value assistantTurnGen ever reaches. That old seeding (1<<32) meant the
        // stale-turn gate (`ui->AssistantTurnGen() != turnGen`) was unconditionally true for
        // every PromptAi turn, so every delta/final/error callback was silently dropped
        // after spending real API quota — including wiping the visible chat if a delta
        // landed mid-stream. Mirrors SmatchetAiAssistantUi.cpp's `++d.assistantTurnGen` /
        // `--d.assistantTurnGen` bump-then-rollback-on-rejection shape.
        //
        // Threading: same pre-existing Phase B contract as `ai.*`'s other Lua-glue calls
        // (see AppController_LuaBindings.cpp's "Threading expectation" comment above
        // LuaAiAddContextGlue) — PromptAi is expected to run on the UI thread; a
        // background-worker Lua script calling it races this write against the panel's
        // own `++d.assistantTurnGen` the same way it already races `luaContext_`. Not a
        // new risk introduced here, and no shipped script (SmatchetHooks.lua) calls
        // `ai.prompt` off the UI thread today.
        IAiAssistantUiState& uiState = GetGlobalAiAssistantUiState();
        const uint64_t turnGen = uiState.BumpAssistantTurnGen();
        const bool accepted = impl_->aiAssistant_->Submit(turnGen, prompt, impl_->aiAssistant_->GetAiContext());
        if (!accepted) {
            uiState.RollbackAssistantTurnGen();
            LOG_WARN("AppController::PromptAi: Submit rejected (no live AI provider or shutting down)");
        }
    }
#else
    (void)prompt;
#endif
}
