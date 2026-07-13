// ai_chat_panel.test.cpp — bucket-E coverage for the AI chat panel's message-interaction scenarios
// (ai-chat-claude-desktop-parity; the 5 mandatory chat scenarios). All cases drive the LIVE
// "Smatchet Assistant" panel (opened via g_ui.assistantPanelOpen, same recipe as
// ai_assistant_panel_dock_swap.test.cpp) over a seeded in-memory g_ui.assistantHistory via the
// shared SeedAndOpenPanel helper. Covered so far:
//   - ClearConversation_ConfirmWipesCancelKeeps — the header trash button's ##ConfirmClearChat
//     modal: Cancel is a no-op, Clear wipes assistantHistory + assistantHistoryRowIds.
//   - CopyMessage_WritesContentToClipboard — the per-turn Copy button writes the message content to
//     the ImGui clipboard (turn seeded Pinned so its action row stays interactive without a hover).
// Remaining (follow-up): pin-bookmark, history-persist, keyboard-nav.

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

// The first (user) turn's content — asserted verbatim by the copy-to-clipboard scenario.
const char* kFirstTurnContent = "smatchet test question";

// Seed `turnCount` in-memory chat turns (1 → a lone user turn; ≥2 → user + assistant) and mark
// history hydrated so the panel's LoadAiChatMessages hydrate path doesn't overwrite the seed.
// Row-ids are parallel to the history (0 = unpersisted). When `pinFirst` is set the first turn is
// Pinned, which keeps its per-turn action row (Copy / Pin) visible + interactive without a hover
// (showRow = Pinned || wasActive in RenderTurnActionRow). A single pinned turn also sits at
// scroll-top so it's always on-screen — never off-screen-culled — for a stable, findable Copy row.
void SeedHistory(int turnCount, bool pinFirst) {
    g_ui.assistantHistory.clear();
    g_ui.assistantHistoryRowIds.clear();
    AiMessage user;
    user.Role = "user";
    user.Content = kFirstTurnContent;
    user.Pinned = pinFirst;
    g_ui.assistantHistory.push_back(user);
    if (turnCount >= 2) {
        AiMessage assistant;
        assistant.Role = "assistant";
        assistant.Content = "smatchet test answer";
        g_ui.assistantHistory.push_back(assistant);
    }
    g_ui.assistantHistoryRowIds.assign(g_ui.assistantHistory.size(), 0);
    g_ui.assistantHistoryHydrated = true;
}

// Open the LIVE "Smatchet Assistant" panel (history must already be seeded), wait for the window to
// go live, then settle several frames so the dockspace docks the panel into its sidebar and the
// history child lays out its turns — interacting before it settles races the layout (the dock-swap
// test uses the same 6-frame wait). Returns true on success. The caller saves + restores the
// mutated g_ui flags for teardown (assistantPanelOpen, cfg.WhisperSetupCompleted).
bool OpenAssistantPanel(ImGuiTestContext* ctx) {
    // Suppress the first-run ##WhisperSetupBanner, which floats over the panel header and swallows
    // the header-button clicks on a clean profile.
    g_ui.cfg.WhisperSetupCompleted = true;
    g_ui.assistantPanelOpen = true;
    g_ui.requestAssistantFocus = true;
    if (!YieldUntil(ctx, [] { return WindowIsLive("Smatchet Assistant"); })) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        ctx->Yield();
    }
    return WindowIsLive("Smatchet Assistant");
}

// Convenience for the two-turn scenarios: seed a user + assistant turn, then open the panel.
bool SeedAndOpenPanel(ImGuiTestContext* ctx, bool pinFirst) {
    SeedHistory(/*turnCount=*/2, pinFirst);
    return OpenAssistantPanel(ctx);
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
        const bool origWhisperSetup = g_ui.cfg.WhisperSetupCompleted;
        const char* kPanel = "Smatchet Assistant";
        const bool panelLive = SeedAndOpenPanel(ctx, /*pinFirst=*/false);
        IM_CHECK_NO_RET(panelLive);
        if (!panelLive) {
            g_ui.assistantHistory.clear();
            g_ui.assistantHistoryRowIds.clear();
            g_ui.assistantPanelOpen = origOpen;
            g_ui.cfg.WhisperSetupCompleted = origWhisperSetup;
            return;
        }

        // CANCEL keeps the conversation: open the confirm, click Cancel, history is unchanged.
        const bool modalOpenForCancel = OpenClearConfirm(ctx, kPanel);
        IM_CHECK_NO_RET(modalOpenForCancel);
        if (modalOpenForCancel) {
            ctx->SetRef("##ConfirmClearChat");
            ctx->ItemClick("Cancel");
            // Confirm the modal actually closed before re-opening, so the next OpenClearConfirm can't
            // click through a stale popup.
            YieldUntil(ctx, [] { return !WindowIsLive("##ConfirmClearChat"); });
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

        // Teardown: clear history unconditionally (the success path already emptied it, but a failed
        // Clear would otherwise leak the 2 seeded turns into g_ui) + restore the mutated flags.
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantPanelOpen = origOpen;
        g_ui.cfg.WhisperSetupCompleted = origWhisperSetup;
        ctx->Yield();
    };
}

// Copy-to-clipboard: the per-turn Copy button writes the message content to the ImGui clipboard.
// The first turn is seeded Pinned so its action row stays visible + interactive without a hover.
void RegisterCopyMessageToClipboard(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiChat", "CopyMessage_WritesContentToClipboard");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        const bool origOpen = g_ui.assistantPanelOpen;
        const bool origWhisperSetup = g_ui.cfg.WhisperSetupCompleted;
        ImGui::SetClipboardText(""); // clear any prior clipboard so the assertion is unambiguous

        // Seed a single Pinned turn: it sits at scroll-top so it's always on-screen (never
        // off-screen-culled) and its Copy / Pin action row is submitted + interactive.
        SeedHistory(/*turnCount=*/1, /*pinFirst=*/true);
        const bool panelLive = OpenAssistantPanel(ctx);
        IM_CHECK_NO_RET(panelLive);
        if (panelLive) {
            ctx->SetRef("Smatchet Assistant");
            // Click the Copy button for turn 0. Its id mirrors the production label
            // "<copyLabel>##AiCopy0" (FA copy glyph when the icon font is loaded, else "Copy").
            const std::string copyLabel = SmatchetAreFaIconsLoaded() ? std::string(ICON_FA_COPY) : std::string("Copy");
            const std::string copyRef = std::string("**/") + copyLabel + "##AiCopy0";
            ctx->ItemClick(copyRef.c_str());
            const bool copied = YieldUntil(ctx, [] {
                const char* clip = ImGui::GetClipboardText();
                return clip != nullptr && std::string(clip) == kFirstTurnContent;
            });
            if (!copied) {
                ctx->LogError("Copy button did not write the turn's content to the clipboard — the "
                              "per-turn Copy action's SetClipboardText(msg.Content) is missing");
                IM_CHECK(false);
            }
        }

        // Teardown: drop the seeded history + restore the mutated flags.
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantPanelOpen = origOpen;
        g_ui.cfg.WhisperSetupCompleted = origWhisperSetup;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterAiChatPanelTests(ImGuiTestEngine* engine) {
    RegisterClearConversationConfirm(engine);
    RegisterCopyMessageToClipboard(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
