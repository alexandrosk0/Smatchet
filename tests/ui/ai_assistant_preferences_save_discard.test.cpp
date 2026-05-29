// ai_assistant_preferences_save_discard.test.cpp — bucket-E coverage for
// the AI Assistant Preferences tab's Save / Discard surface.
//
// Per the test.md P2 disposition recorded in
// tests/ui/ai_prefs_autosave_flow.test.cpp:14-17, an explicit "Save / Discard"
// button pair was never shipped on the Assistant tab — the shipped surface
// is autosave + the Test-connection probe. This TU covers the **effective**
// save-discard semantics: arming dirty → debounce drain (effective "Save")
// vs reset-before-drain (effective "Discard").
//
// Drift warning — IF YOU CHANGE Source/Core/include/SmatchetUiSession.h:601-614
// (prefsDirty + prefsSaveDueAt + MarkPrefsDirty inline helper), OR
// Source/Core/src/SmatchetUI.cpp:768-776 (debounce-and-save drain at end of
// frame), UPDATE THIS REPLICA to match.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <chrono>
#include <cstring>

namespace {

struct SaveDiscardState {
    char field[64] = {0};
    char persistedField[64] = {0};
    bool prefsDirty = false;
    std::chrono::steady_clock::time_point prefsSaveDueAt{};
    int saveCalls = 0;
    int discardCalls = 0;
};

SaveDiscardState g_saveDiscardState;

void ResetSaveDiscardState() {
    std::memset(g_saveDiscardState.field, 0, sizeof(g_saveDiscardState.field));
    std::memset(g_saveDiscardState.persistedField, 0, sizeof(g_saveDiscardState.persistedField));
    g_saveDiscardState.prefsDirty = false;
    g_saveDiscardState.prefsSaveDueAt = std::chrono::steady_clock::time_point{};
    g_saveDiscardState.saveCalls = 0;
    g_saveDiscardState.discardCalls = 0;
}

void ArmDirty(SaveDiscardState& s) {
    if (!s.prefsDirty) {
        s.prefsDirty = true;
        s.prefsSaveDueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
}

// "Save" = debounce drain commits field → persistedField.
void Drain(SaveDiscardState& s) {
    if (s.prefsDirty && std::chrono::steady_clock::now() >= s.prefsSaveDueAt) {
        std::memcpy(s.persistedField, s.field, sizeof(s.persistedField));
        ++s.saveCalls;
        s.prefsDirty = false;
    }
}

// "Discard" = reset field to persistedField before debounce drain fires.
void Discard(SaveDiscardState& s) {
    if (s.prefsDirty) {
        std::memcpy(s.field, s.persistedField, sizeof(s.field));
        s.prefsDirty = false;
        ++s.discardCalls;
    }
}

void DrawSaveDiscardReplica(SaveDiscardState& s) {
    ImGui::SetNextWindowSize(ImVec2(360, 120), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::SaveDiscard", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::InputText("##SDField", s.field, sizeof(s.field))) {
            ArmDirty(s);
        }
    }
    ImGui::End();
}

void RegisterSaveCommitsVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "SaveDiscard_DebounceDrainSaves");
    t->UserData = &g_saveDiscardState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<SaveDiscardState*>(ctx->Test->UserData);
        DrawSaveDiscardReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<SaveDiscardState*>(ctx->Test->UserData);
        ResetSaveDiscardState();
        ctx->SetRef("SmatchetTest::SaveDiscard");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##SDField");
        ctx->KeyChars("kept");
        ctx->Yield();
        IM_CHECK(s->prefsDirty);

        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        Drain(*s);

        IM_CHECK_EQ(s->saveCalls, 1);
        IM_CHECK(!s->prefsDirty);
        IM_CHECK_STR_EQ(s->persistedField, "kept");
    };
}

void RegisterDiscardResetsVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "SaveDiscard_DiscardResetsBeforeDrain");
    t->UserData = &g_saveDiscardState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<SaveDiscardState*>(ctx->Test->UserData);
        DrawSaveDiscardReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<SaveDiscardState*>(ctx->Test->UserData);
        ResetSaveDiscardState();
        std::memcpy(s->persistedField, "orig", 5);
        std::memcpy(s->field, "orig", 5);
        ctx->SetRef("SmatchetTest::SaveDiscard");
        ctx->Yield();
        ctx->Yield();

        // ImGui InputText select-all-on-first-click replaces "orig" with the
        // typed chars. We arm dirty directly to bypass the engine quirk
        // around InputText buffer-sync; the surface under test is the
        // Discard logic, not InputText itself.
        ArmDirty(*s);
        std::memcpy(s->field, "Xrig", 5);
        ctx->Yield();
        IM_CHECK(s->prefsDirty);

        Discard(*s);
        IM_CHECK_EQ(s->discardCalls, 1);
        IM_CHECK(!s->prefsDirty);
        IM_CHECK_STR_EQ(s->field, "orig");

        // After Discard, draining produces no save (already clean).
        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        Drain(*s);
        IM_CHECK_EQ(s->saveCalls, 0);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPreferencesSaveDiscardTests(ImGuiTestEngine* engine) {
    RegisterSaveCommitsVariant(engine);
    RegisterDiscardResetsVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
