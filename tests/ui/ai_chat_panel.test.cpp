// ai_chat_panel.test.cpp — bucket-E coverage for the AI chat panel's message-interaction scenarios
// (ai-chat-claude-desktop-parity; the 5 mandatory chat scenarios). All cases drive the LIVE
// "Smatchet Assistant" panel (opened via g_ui.assistantPanelOpen, same recipe as
// ai_assistant_panel_dock_swap.test.cpp) over a seeded in-memory g_ui.assistantHistory via the
// shared SeedAndOpenPanel helper. Covered so far:
//   - ClearConversation_ConfirmWipesCancelKeeps — the header trash button's ##ConfirmClearChat
//     modal: Cancel is a no-op, Clear wipes assistantHistory + assistantHistoryRowIds.
//   - CopyMessage_WritesContentToClipboard — the per-turn Copy button writes the message content to
//     the ImGui clipboard (turn seeded Pinned so its action row stays interactive without a hover).
//   - PinBookmark_ActionRowTogglesPinnedState — the per-turn Pin/Unpin action-row button flips
//     AiMessage::Pinned (the flag that drives the pinned-bookmark strip) both ways.
//   - KeyboardEnter_SubmitsThroughConsentGate — typing a prompt + bare Enter submits (EnterReturnsTrue);
//     the first-send outbound-consent gate intercepts it offline (assistantConsentRows / modal). The
//     panel is floated + enlarged (OpenAssistantPanelWithInput) so the bottom input row is reachable.
// Remaining (follow-up): history-persist (SQLite round-trip; reuses OpenAssistantPanelWithInput).

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

// Open the panel and make its bottom message input reachable. In the headless dockspace the panel
// docks into a short sidebar that clips the ##AiAssistantInput row off the fold (it never reaches
// the item table). Undock it to a floating window, then WindowResize it tall enough for header +
// history + input — WindowResize is a no-op on a DOCKED window, so the undock must come first.
// Returns true once the input item is reachable. Used by the send/keyboard scenarios.
bool OpenAssistantPanelWithInput(ImGuiTestContext* ctx) {
    if (!OpenAssistantPanel(ctx)) {
        return false;
    }
    ctx->UndockWindow("Smatchet Assistant");
    ctx->WindowResize("Smatchet Assistant", ImVec2(520.0f, 700.0f));
    ctx->Yield();
    ctx->SetRef("Smatchet Assistant");
    return YieldUntil(ctx, [&] { return ctx->ItemExists("**/##AiAssistantInput"); }, 60);
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

// Pin bookmark: the per-turn Pin/Unpin action-row button toggles AiMessage::Pinned — the flag that
// surfaces (and removes) a message in the pinned-bookmark strip above the history. Seed the turn
// Pinned so its action row is interactive without a hover; unpin then re-pin, asserting the state
// flips both ways. The button id is stable ("##AiPin0"); only the label prefix flips with state.
void RegisterPinBookmarkToggle(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiChat", "PinBookmark_ActionRowTogglesPinnedState");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        const bool origOpen = g_ui.assistantPanelOpen;
        const bool origWhisperSetup = g_ui.cfg.WhisperSetupCompleted;

        // Single Pinned turn: on-screen (never culled) with an interactive action row.
        SeedHistory(/*turnCount=*/1, /*pinFirst=*/true);
        const bool panelLive = OpenAssistantPanel(ctx);
        IM_CHECK_NO_RET(panelLive);
        if (panelLive && !g_ui.assistantHistory.empty()) {
            ctx->SetRef("Smatchet Assistant");
            const bool fa = SmatchetAreFaIconsLoaded();
            IM_CHECK_NO_RET(g_ui.assistantHistory[0].Pinned); // precondition: seeded Pinned

            // UNPIN: the turn starts Pinned, so the button shows the "unpin" affordance.
            const std::string unpinRef = std::string("**/") + (fa ? ICON_FA_THUMBTACK_SLASH : "Unpin") + "##AiPin0";
            ctx->ItemClick(unpinRef.c_str());
            const bool unpinned =
                YieldUntil(ctx, [] { return !g_ui.assistantHistory.empty() && !g_ui.assistantHistory[0].Pinned; });
            if (!unpinned) {
                ctx->LogError("Unpin: clicking the action-row pin button did not clear AiMessage::Pinned");
                IM_CHECK(false);
            }

            // RE-PIN: the button now shows the "pin" affordance; the just-clicked row stays active
            // (wasActive from the prior click) so its action row is still interactive.
            const std::string pinRef = std::string("**/") + (fa ? ICON_FA_THUMBTACK : "Pin") + "##AiPin0";
            ctx->ItemClick(pinRef.c_str());
            const bool repinned =
                YieldUntil(ctx, [] { return !g_ui.assistantHistory.empty() && g_ui.assistantHistory[0].Pinned; });
            if (!repinned) {
                ctx->LogError("Re-pin: clicking the action-row pin button did not set AiMessage::Pinned");
                IM_CHECK(false);
            }
        }

        // Teardown.
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantPanelOpen = origOpen;
        g_ui.cfg.WhisperSetupCompleted = origWhisperSetup;
        ctx->Yield();
    };
}

// Keyboard nav: the multiline input is wired ImGuiInputTextFlags_EnterReturnsTrue, so a bare Enter
// submits. Typing text + Enter runs the send path (DispatchAiSend); on a fresh profile the first
// send is intercepted by the outbound-consent gate, which populates assistantConsentRows + opens the
// ##AiOutboundConsent modal and returns WITHOUT any network — a deterministic offline observable
// that "Enter submitted". The panel is floated + enlarged first (OpenAssistantPanelWithInput) so the
// bottom input row isn't clipped off the docked sidebar.
void RegisterKeyboardEnterSubmits(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiChat", "KeyboardEnter_SubmitsThroughConsentGate");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        const AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        if (!app->HasAiAssistantController()) {
            ctx->LogInfo("SKIP: no AI assistant controller (SMATCHET_WITH_AI off) — send path inert");
            return;
        }
        const bool origOpen = g_ui.assistantPanelOpen;
        const bool origWhisperSetup = g_ui.cfg.WhisperSetupCompleted;
        const bool origConsentShown = g_ui.cfg.AssistantOutboundConsentShown;

        // Force the first-send consent gate so the Enter dispatch is intercepted offline (no POST).
        g_ui.cfg.AssistantOutboundConsentShown = false;
        g_ui.assistantConsentRows.clear();
        g_ui.assistantInFlight = false;
        SeedHistory(/*turnCount=*/1, /*pinFirst=*/false);
        const bool inputReachable = OpenAssistantPanelWithInput(ctx);
        IM_CHECK_NO_RET(inputReachable);
        if (inputReachable) {
            // Focus the multiline input + type a prompt; the buffer mirrors into assistantInputBuf.
            ctx->ItemInput("**/##AiAssistantInput");
            ctx->KeyChars("smatchet keyboard send");
            const bool typed = YieldUntil(ctx, [] { return !g_ui.assistantInputBuf.empty(); }, 90);
            IM_CHECK_NO_RET(typed);
            if (typed) {
                // Bare Enter submits; with a controller + non-empty input, send is NOT disabled, so
                // DispatchAiSend runs and the first-send consent gate fires (offline).
                ctx->KeyPress(ImGuiKey_Enter);
                const bool submitted = YieldUntil(
                    ctx, [] { return !g_ui.assistantConsentRows.empty() || WindowIsLive("##AiOutboundConsent"); }, 90);
                if (!submitted) {
                    ctx->LogError("Bare Enter did not submit — neither the outbound-consent rows nor "
                                  "the ##AiOutboundConsent modal appeared; the keyboard send path is broken");
                    IM_CHECK(false);
                }
            }
        }

        // Teardown: dismiss the consent modal via Escape (never hangs on a missing button) + restore
        // the mutated flags. This is the last AiChat case, so a leftover input buffer can't reach a
        // sibling. assistantInputBuf is re-mirrored from the file-static widget buffer each frame.
        if (WindowIsLive("##AiOutboundConsent")) {
            ctx->KeyPress(ImGuiKey_Escape);
            YieldUntil(ctx, [] { return !WindowIsLive("##AiOutboundConsent"); }, 60);
        }
        g_ui.assistantConsentRows.clear();
        g_ui.assistantHistory.clear();
        g_ui.assistantHistoryRowIds.clear();
        g_ui.assistantPanelOpen = origOpen;
        g_ui.cfg.WhisperSetupCompleted = origWhisperSetup;
        g_ui.cfg.AssistantOutboundConsentShown = origConsentShown;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterAiChatPanelTests(ImGuiTestEngine* engine) {
    RegisterClearConversationConfirm(engine);
    RegisterCopyMessageToClipboard(engine);
    RegisterPinBookmarkToggle(engine);
    RegisterKeyboardEnterSubmits(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
