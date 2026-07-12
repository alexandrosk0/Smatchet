// ai_chat_clear_confirm.test.cpp — bucket-E coverage for the AI chat "Clear conversation" confirm
// flow (ai-chat-claude-desktop-parity Phase 7; one of the 5 mandatory chat scenarios). The header
// trash button opens a ##ConfirmClearChat modal so a stray click can't wipe history: Cancel is a
// no-op, Clear wipes assistantHistory + assistantHistoryRowIds. Drives the LIVE "Smatchet
// Assistant" panel (opened via g_ui.assistantPanelOpen, same recipe as
// ai_assistant_panel_dock_swap.test.cpp) with a seeded in-memory history.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AiTypes.h"
#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "IconsFontAwesome6.h"  // ICON_FA_TRASH — mirrors the production header-button label
#include "SmatchetImGuiFonts.h" // SmatchetAreFaIconsLoaded
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

// g_ui — the shared UI-thread state bag; the assistant panel reads/writes assistantHistory here.
extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* w = ImGui::FindWindowByName(title);
    return w != nullptr && w->Active;
}

// Seed two in-memory chat turns and mark history hydrated so the panel's LoadAiChatMessages hydrate
// path doesn't overwrite the seed. Row-ids are parallel to the history (0 = unpersisted).
void SeedTwoTurnHistory() {
    g_ui.assistantHistory.clear();
    g_ui.assistantHistoryRowIds.clear();
    AiMessage user;
    user.Role = "user";
    user.Content = "smatchet test question";
    AiMessage assistant;
    assistant.Role = "assistant";
    assistant.Content = "smatchet test answer";
    g_ui.assistantHistory.push_back(user);
    g_ui.assistantHistory.push_back(assistant);
    g_ui.assistantHistoryRowIds.assign(g_ui.assistantHistory.size(), 0);
    g_ui.assistantHistoryHydrated = true;
}

// Click the header clear button, mirroring its production label (FA trash glyph when the icon font
// is loaded, else the "Clear" text fallback), and wait for the confirm modal to go live.
bool OpenClearConfirm(ImGuiTestContext* ctx, const char* panelRef) {
    ctx->SetRef(panelRef);
    const std::string clearRef =
        SmatchetAreFaIconsLoaded() ? std::string("**/") + ICON_FA_TRASH : std::string("**/Clear");
    ctx->ItemClick(clearRef.c_str());
    return YieldUntil(ctx, [] { return WindowIsLive("##ConfirmClearChat"); });
}

void RegisterClearConversationConfirm(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiChat", "ClearConversation_ConfirmWipesCancelKeeps");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        const bool origOpen = g_ui.assistantPanelOpen;
        // Suppress the first-run ##WhisperSetupBanner, which otherwise floats over the panel header
        // and swallows the clear-button click on a clean profile.
        g_ui.cfg.WhisperSetupCompleted = true;

        SeedTwoTurnHistory();
        g_ui.assistantPanelOpen = true;
        g_ui.requestAssistantFocus = true;

        const char* kPanel = "Smatchet Assistant";
        const bool panelLive = YieldUntil(ctx, [&] { return WindowIsLive(kPanel); });
        IM_CHECK_NO_RET(panelLive);
        if (!panelLive) {
            g_ui.assistantPanelOpen = origOpen;
            return;
        }

        // CANCEL keeps the conversation: open the confirm, click Cancel, history is unchanged.
        const bool modalOpenForCancel = OpenClearConfirm(ctx, kPanel);
        IM_CHECK_NO_RET(modalOpenForCancel);
        if (modalOpenForCancel) {
            ctx->SetRef("##ConfirmClearChat");
            ctx->ItemClick("Cancel");
            ctx->Yield();
            ctx->Yield();
            IM_CHECK_NO_RET(g_ui.assistantHistory.size() == 2); // cancel is a no-op
        }

        // CONFIRM wipes it: re-open the confirm, click Clear, history + row-ids are emptied.
        const bool modalOpenForClear = OpenClearConfirm(ctx, kPanel);
        IM_CHECK_NO_RET(modalOpenForClear);
        if (modalOpenForClear) {
            ctx->SetRef("##ConfirmClearChat");
            ctx->ItemClick("Clear");
            const bool wiped = YieldUntil(ctx, [] { return g_ui.assistantHistory.empty(); });
            if (!wiped) {
                ctx->LogError("Clear-confirm did not wipe assistantHistory — the confirm popup's Clear "
                              "button no longer clears the in-memory history vectors");
                IM_CHECK(false);
            }
            IM_CHECK_NO_RET(g_ui.assistantHistoryRowIds.empty());
        }

        // Teardown: restore the panel-open flag (history stays empty — a clean end state).
        g_ui.assistantPanelOpen = origOpen;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterAiChatClearConfirmTests(ImGuiTestEngine* engine) {
    RegisterClearConversationConfirm(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
