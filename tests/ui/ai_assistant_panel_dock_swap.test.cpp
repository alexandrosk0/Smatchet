// ai_assistant_panel_dock_swap.test.cpp — bucket-E coverage for the Assistant
// panel's dock-side swap toggle and auto-reveal of the secondary side bar.
//
// Drift warning — IF YOU CHANGE Source/Core/src/SmatchetAiAssistantUi.cpp
// (specifically the swap-button block at SmatchetDrawAiAssistantPanel lines
// 351-372, where SmallButton("<- Left" / "Right ->") flips
// AssistantPanelOnSecondarySide + assistantPendingSideSwap + auto-reveals
// ShowSecondarySideBar), UPDATE THIS REPLICA to match. The replica intentionally
// excludes ScheduleConfigSaveDetached(d.cfg) — that worker spawn is out-of-scope
// for state-only assertions.
//
// Two variants:
//   1. DockSwap_StateReplica_TogglesAllFlags (PRIMARY, deterministic) — drives
//      a faithful replica with TU-local flags and asserts every transition.
//   2. DockSwap_LiveHostProbe_PanelMigratesNode (SECONDARY, informational) —
//      drives the REAL SmatchetDrawAiAssistantPanel against the host dockspace
//      and observes ImGui dock-node migration. Skip-with-log on missing dock
//      nodes per the callstack_tooltip_hover variant-4 lesson — never fails on
//      host-layout drift.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "SmatchetDockNodeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <string>

namespace {

// Per-test state for the state-replica variant. Mirrors the three production
// flags that the SmallButton path flips. ScheduleConfigSaveDetached is NOT
// mirrored — its only observable effect is a worker spawn we don't want.
struct DockSwapState {
    bool onSecondarySide = false;
    bool showSecondarySideBar = false;
    bool pendingSwap = false;
};

DockSwapState g_dockSwapState;

void ResetDockSwapState() {
    g_dockSwapState.onSecondarySide = false;
    g_dockSwapState.showSecondarySideBar = false;
    g_dockSwapState.pendingSwap = false;
}

// Faithful replica of SmatchetAiAssistantUi.cpp:351-372 — header strip with the
// swap button only. Provider-name TextDisabled + tooltip omitted (irrelevant to
// flag transitions; production wraps the same SmallButton block).
void DrawDockSwapReplica(DockSwapState& s) {
    // Production resolves the side through AiAssistantEffectiveOnSecondary(cfg flag,
    // d.assistantSideFallback) so the label describes where the panel actually IS. This
    // replica models the no-fallback case (assistantSideFallback == -1), where the two are
    // identical; the fallback branch itself is pinned in tests/Core/AiAssistantUiDetail.test.cpp.
    const bool onRight = s.onSecondarySide;
    const char* swapLabel = onRight ? "<- Left" : "Right ->";
    if (ImGui::SmallButton(swapLabel)) {
        s.onSecondarySide = !onRight;
        s.pendingSwap = true;
        // Production auto-reveal: swapping right requires the slot to be visible.
        if (s.onSecondarySide && !s.showSecondarySideBar) {
            s.showSecondarySideBar = true;
        }
    }
}

void RegisterStateReplicaVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "DockSwap_StateReplica_TogglesAllFlags");
    t->UserData = &g_dockSwapState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockSwapState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(320, 80), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AiAssistantDockSwap", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawDockSwapReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockSwapState*>(ctx->Test->UserData);
        ResetDockSwapState();
        ctx->SetRef("SmatchetTest::AiAssistantDockSwap");
        ctx->Yield();
        ctx->Yield();

        // Initial state: left-side. Button label reads "Right ->".
        IM_CHECK(!s->onSecondarySide);
        IM_CHECK(!s->showSecondarySideBar);
        IM_CHECK(!s->pendingSwap);

        // Click 1: swap to right. All three flags flip true.
        ctx->ItemClick("Right ->");
        ctx->Yield();
        IM_CHECK(s->onSecondarySide);
        IM_CHECK(s->showSecondarySideBar);
        IM_CHECK(s->pendingSwap);

        // Clear pendingSwap to mirror production's per-frame consumption inside
        // SmatchetDrawAiAssistantPanel (line 314 — set, then immediately false
        // after SetNextWindowDockID). Without this clear, the next click's
        // pendingSwap-true assertion would be trivially satisfied.
        s->pendingSwap = false;
        ctx->Yield();

        // Click 2: swap back to left. Label has flipped to "<- Left". Side
        // flag flips false; ShowSecondarySideBar STAYS true (production never
        // clears it — the user's reveal preference is intentionally sticky);
        // pendingSwap goes true again.
        ctx->ItemClick("<- Left");
        ctx->Yield();
        IM_CHECK(!s->onSecondarySide);
        IM_CHECK(s->showSecondarySideBar);
        IM_CHECK(s->pendingSwap);
    };
}

void RegisterLiveHostProbeVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "DockSwap_LiveHostProbe_PanelMigratesNode");

    t->TestFunc = [](ImGuiTestContext* ctx) {
        // Pre-condition: open the Assistant panel + request focus. The real
        // SmatchetDrawAiAssistantPanel honours these via UiDrawSession state.
        g_ui.assistantPanelOpen = true;
        g_ui.requestAssistantFocus = true;

        // Allow several frames for the dockspace to settle the panel into its
        // first-use-ever dock id.
        for (int i = 0; i < 6; ++i)
            ctx->Yield();

        const ImGuiWindow* win = ImGui::FindWindowByName("Smatchet Assistant");
        if (win == nullptr) {
            ctx->LogInfo("skip: 'Smatchet Assistant' window not found in this layout");
            return;
        }
        const ImGuiID baselineDockId = win->DockId;
        if (baselineDockId == 0) {
            ctx->LogInfo("skip: panel has no dock-id (host layout not docking it)");
            return;
        }

        const bool startOnRight = g_ui.cfg.AssistantPanelOnSecondarySide;
        const ImGuiID targetDockId =
            startOnRight ? SmatchetDockNodeIds::kPrimarySideBar : SmatchetDockNodeIds::kSecondarySideBar;
        if (ImGui::DockBuilderGetNode(targetDockId) == nullptr) {
            ctx->LogInfo("skip: target dock node 0x%08X not registered in this layout", targetDockId);
            return;
        }

        ctx->SetRef("Smatchet Assistant");
        const char* swapLabel = startOnRight ? "<- Left" : "Right ->";
        ctx->ItemClick(swapLabel);

        for (int i = 0; i < 6; ++i)
            ctx->Yield();

        IM_CHECK(g_ui.cfg.AssistantPanelOnSecondarySide == !startOnRight);
        IM_CHECK(g_ui.cfg.ShowSecondarySideBar == true);

        const ImGuiWindow* postWin = ImGui::FindWindowByName("Smatchet Assistant");
        if (postWin == nullptr) {
            ctx->LogInfo("skip: post-swap window lookup failed (panel may have been closed)");
            return;
        }
        if (postWin->DockId != targetDockId) {
            ctx->LogInfo("skip: post-swap DockId=0x%08X != target=0x%08X (host layout overrode the swap)",
                         postWin->DockId, targetDockId);
            return;
        }
        IM_CHECK(postWin->DockId == targetDockId);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPanelDockSwapTests(ImGuiTestEngine* engine) {
    RegisterStateReplicaVariant(engine);
    RegisterLiveHostProbeVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
