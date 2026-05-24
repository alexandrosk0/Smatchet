// ai_assistant_preferences_verify_on_save.test.cpp — bucket-E coverage for
// the AI Assistant Preferences tab's "verify on save" surface: when the
// debounce drain commits a Save, the verify probe arms automatically and
// the result line populates with the outcome.
//
// Drift warning — IF YOU CHANGE Source_Core/include/AiPrefsTestConnection.h
// (TriggerProbe signature), Source_Core/src/AiPrefsTestConnection.cpp
// (the cancel-token short-circuit on completion), Source_Core/src/SmatchetUI.cpp
// (the autosave-debounce-then-verify chain at end-of-frame), OR
// Source_Core/include/SmatchetUiSession.h:244-257 (assistantPrefsTest* fields),
// UPDATE THIS REPLICA to match.
//
// Per slice-9 per-test isolation contract: ResetVerifyOnSaveState() runs at
// the top of every TestFunc. Cancel-on-close short-circuit is exercised in
// variant 2 to mirror the ai_prefs_autosave_flow TU's third variant — but
// here the replica drives the post-save chain directly (no AppController
// dependency).

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>

namespace {

struct VerifyOnSaveState {
    char field[64] = {0};
    bool prefsDirty = false;
    std::chrono::steady_clock::time_point prefsSaveDueAt{};
    int saveCalls = 0;
    bool verifyArmed = false;
    bool verifyInFlight = false;
    std::string verifyResult;
    int verifyResultType = 0;
    std::shared_ptr<std::atomic<bool>> verifyCancel;
};

VerifyOnSaveState g_verifyOnSaveState;

void ResetVerifyOnSaveState() {
    std::memset(g_verifyOnSaveState.field, 0, sizeof(g_verifyOnSaveState.field));
    g_verifyOnSaveState.prefsDirty = false;
    g_verifyOnSaveState.prefsSaveDueAt = std::chrono::steady_clock::time_point{};
    g_verifyOnSaveState.saveCalls = 0;
    g_verifyOnSaveState.verifyArmed = false;
    g_verifyOnSaveState.verifyInFlight = false;
    g_verifyOnSaveState.verifyResult.clear();
    g_verifyOnSaveState.verifyResultType = 0;
    g_verifyOnSaveState.verifyCancel.reset();
}

void ArmDirty(VerifyOnSaveState& s) {
    if (!s.prefsDirty) {
        s.prefsDirty = true;
        s.prefsSaveDueAt = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    }
}

// "Save" path: debounce drain commits + chains into the verify probe.
void DrainAndVerify(VerifyOnSaveState& s) {
    if (s.prefsDirty && std::chrono::steady_clock::now() >= s.prefsSaveDueAt) {
        ++s.saveCalls;
        s.prefsDirty = false;
        // Verify-on-save chain (mirrors the SmatchetUI.cpp end-of-frame logic).
        s.verifyArmed = true;
        s.verifyInFlight = true;
        s.verifyResult = "Verifying...";
        s.verifyResultType = 0;
        s.verifyCancel = std::make_shared<std::atomic<bool>>(false);
    }
}

void CompleteVerify(VerifyOnSaveState& s, bool ok) {
    if (s.verifyCancel && s.verifyCancel->load()) {
        // Cancel-on-close short-circuit: callback returns without mutating
        // the result fields (mirrors AiPrefsTestConnection's posted lambda).
        return;
    }
    s.verifyInFlight = false;
    if (ok) {
        s.verifyResult = "Verified.";
        s.verifyResultType = 1;
    } else {
        s.verifyResult = "HTTP 401";
        s.verifyResultType = 2;
    }
}

void DrawVerifyReplica(VerifyOnSaveState& s) {
    ImGui::SetNextWindowSize(ImVec2(360, 140), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::VerifyOnSave", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        if (ImGui::InputText("##VFField", s.field, sizeof(s.field))) {
            ArmDirty(s);
        }
        if (!s.verifyResult.empty()) {
            ImGui::TextUnformatted(s.verifyResult.c_str());
        }
    }
    ImGui::End();
}

void RegisterVerifyArmsAfterSaveVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "VerifyOnSave_ArmsAfterDrain");
    t->UserData = &g_verifyOnSaveState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<VerifyOnSaveState*>(ctx->Test->UserData);
        DrawVerifyReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<VerifyOnSaveState*>(ctx->Test->UserData);
        ResetVerifyOnSaveState();
        ctx->SetRef("SmatchetTest::VerifyOnSave");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##VFField");
        ctx->KeyChars("hello");
        ctx->Yield();
        IM_CHECK(s->prefsDirty);
        IM_CHECK(!s->verifyArmed);

        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        DrainAndVerify(*s);
        IM_CHECK_EQ(s->saveCalls, 1);
        IM_CHECK(s->verifyArmed);
        IM_CHECK(s->verifyInFlight);
        IM_CHECK_EQ(s->verifyResultType, 0);

        CompleteVerify(*s, /*ok=*/true);
        ctx->Yield();
        IM_CHECK(!s->verifyInFlight);
        IM_CHECK_EQ(s->verifyResultType, 1);
        IM_CHECK_STR_EQ(s->verifyResult.c_str(), "Verified.");
    };
}

void RegisterVerifyCancelOnCloseVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "VerifyOnSave_CancelShortCircuits");
    t->UserData = &g_verifyOnSaveState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<VerifyOnSaveState*>(ctx->Test->UserData);
        DrawVerifyReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<VerifyOnSaveState*>(ctx->Test->UserData);
        ResetVerifyOnSaveState();
        ctx->SetRef("SmatchetTest::VerifyOnSave");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##VFField");
        ctx->KeyChars("X");
        ctx->Yield();

        s->prefsSaveDueAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
        DrainAndVerify(*s);
        IM_CHECK(s->verifyInFlight);
        IM_CHECK(s->verifyCancel != nullptr);

        // Cancel BEFORE completion; CompleteVerify's cancel-guard short-circuits.
        s->verifyCancel->store(true, std::memory_order_release);
        CompleteVerify(*s, /*ok=*/true);
        ctx->Yield();
        // Result type stays at 0 (initial "Verifying...") — success branch
        // never landed because the cancel-guard returned early.
        IM_CHECK_EQ(s->verifyResultType, 0);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPreferencesVerifyOnSaveTests(ImGuiTestEngine* engine) {
    RegisterVerifyArmsAfterSaveVariant(engine);
    RegisterVerifyCancelOnCloseVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
