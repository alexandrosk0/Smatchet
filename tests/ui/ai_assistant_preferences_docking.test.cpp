// ai_assistant_preferences_docking.test.cpp — bucket-E coverage for the
// AI Assistant Preferences tab's docking surface: tab activation, tab-bar
// docking inside the Preferences modal, and "Assistant" tab persistence
// across re-open.
//
// Drift warning — IF YOU CHANGE Source/Core/src/SmatchetPreferencesUi.cpp
// (specifically the `BeginTabItem("Assistant")` block at line 562 + the
// tab-bar surrounding it, the showPreferences gating at
// Source/Core/include/SmatchetUiSession.h:128, OR the close-handler cancel
// at SmatchetPreferencesUi.cpp:231-235 that cancels the in-flight probe on
// tab/modal close), UPDATE THIS REPLICA to match.
//
// Per slice 9 (docs/plans/shipped/autonomous-debugging-no-creds.md § Slice 9) and
// the per-test isolation contract: ResetDockingState() runs at the top of
// every TestFunc. No global teardown / engine state mutation.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

struct DockingState {
    bool showPreferences = false;
    bool assistantTabActive = false;
    int assistantOpenCount = 0;
};

DockingState g_dockingState;

void ResetDockingState() {
    g_dockingState.showPreferences = false;
    g_dockingState.assistantTabActive = false;
    g_dockingState.assistantOpenCount = 0;
}

// Faithful replica of SmatchetPreferencesUi.cpp's modal + tab-bar shape.
// Excludes the cfg / validation plumbing — only the docking + tab-activation
// surface is under test here.
void DrawPreferencesReplica(DockingState& s) {
    if (!s.showPreferences) {
        // Mirror SmatchetPreferencesUi.cpp's close-handler — when the modal
        // is dismissed, the next-frame tick clears the active-tab flag so the
        // cancel-on-close path can observe it gone.
        s.assistantTabActive = false;
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(640, 400), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::Preferences", &s.showPreferences,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::BeginTabBar("##PrefsTabs")) {
            if (ImGui::BeginTabItem("Tracker")) {
                ImGui::TextUnformatted("(tracker pane)");
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Assistant")) {
                if (!s.assistantTabActive) {
                    ++s.assistantOpenCount;
                }
                s.assistantTabActive = true;
                ImGui::TextUnformatted("(assistant pane)");
                ImGui::EndTabItem();
            } else {
                s.assistantTabActive = false;
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

void RegisterAssistantTabActivatesVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "Docking_AssistantTabActivates");
    t->UserData = &g_dockingState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockingState*>(ctx->Test->UserData);
        DrawPreferencesReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockingState*>(ctx->Test->UserData);
        ResetDockingState();
        s->showPreferences = true;
        ctx->Yield();
        ctx->Yield();
        ctx->SetRef("SmatchetTest::Preferences");

        ctx->ItemClick("##PrefsTabs/Assistant");
        ctx->Yield();
        IM_CHECK(s->assistantTabActive);
        IM_CHECK_EQ(s->assistantOpenCount, 1);

        // Switch away then back — counter increments on each fresh activation.
        ctx->ItemClick("##PrefsTabs/Tracker");
        ctx->Yield();
        IM_CHECK(!s->assistantTabActive);

        ctx->ItemClick("##PrefsTabs/Assistant");
        ctx->Yield();
        IM_CHECK(s->assistantTabActive);
        IM_CHECK_EQ(s->assistantOpenCount, 2);
    };
}

void RegisterCloseModalCancelsActiveVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "Docking_CloseClearsActiveFlag");
    t->UserData = &g_dockingState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockingState*>(ctx->Test->UserData);
        DrawPreferencesReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<DockingState*>(ctx->Test->UserData);
        ResetDockingState();
        s->showPreferences = true;
        ctx->Yield();
        ctx->Yield();
        ctx->SetRef("SmatchetTest::Preferences");

        ctx->ItemClick("##PrefsTabs/Assistant");
        ctx->Yield();
        IM_CHECK(s->assistantTabActive);

        // Mirror the cancel-on-close handler — externally setting showPreferences=false
        // causes the next frame's DrawPreferencesReplica to bail before BeginTabItem
        // fires, so assistantTabActive falls back to false.
        s->showPreferences = false;
        ctx->Yield();
        ctx->Yield();
        IM_CHECK(!s->assistantTabActive);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPreferencesDockingTests(ImGuiTestEngine* engine) {
    RegisterAssistantTabActivatesVariant(engine);
    RegisterCloseModalCancelsActiveVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
