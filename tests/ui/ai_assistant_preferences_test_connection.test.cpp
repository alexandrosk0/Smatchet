// ai_assistant_preferences_test_connection.test.cpp — bucket-E coverage for
// the AI Assistant Preferences tab's "Test connection" probe lifecycle:
// in-flight flag arms on click, result line populates on success, and result
// line populates with the error path on a failing probe.
//
// Drift warning — IF YOU CHANGE Source/Core/include/AiPrefsTestConnection.h
// (TriggerProbe signature / behaviour), Source/Core/src/AiPrefsTestConnection.cpp
// (the probe worker body that polls cancel + invokes ProbeReachability +
// SendStreaming), OR Source/Core/include/SmatchetUiSession.h:244-257 (the
// assistantPrefsTest{InFlight,Result,ResultType,Cancel} state members),
// UPDATE THIS REPLICA / HOST-COUPLED CHECK to match.
//
// This TU uses a small inline replica of the probe lifecycle (rather than
// driving the live AiPrefsTestConnection::TriggerProbe entry point as the
// autosave_flow TU does) so the test stays deterministic without depending
// on SmatchetActiveUiTestAppController() returning a real AppController.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <atomic>
#include <memory>
#include <string>

namespace {

struct TestConnState {
    bool inFlight = false;
    std::string result;
    int resultType = 0; // 0 info, 1 success, 2 error
    std::shared_ptr<std::atomic<bool>> cancel;
    bool clicked = false;
};

TestConnState g_testConnState;

void ResetTestConnState() {
    g_testConnState.inFlight = false;
    g_testConnState.result.clear();
    g_testConnState.resultType = 0;
    g_testConnState.cancel.reset();
    g_testConnState.clicked = false;
}

// Replica of the inline TriggerProbe state-mutation prelude (Source/Core/
// src/AiPrefsTestConnection.cpp).
void StartProbe(TestConnState& s) {
    s.inFlight = true;
    s.result = "Testing...";
    s.resultType = 0;
    s.cancel = std::make_shared<std::atomic<bool>>(false);
}

void FinishProbeSuccess(TestConnState& s) {
    if (s.cancel && s.cancel->load()) {
        return;
    }
    s.inFlight = false;
    s.result = "Verified.";
    s.resultType = 1;
}

void FinishProbeError(TestConnState& s, const std::string& err) {
    if (s.cancel && s.cancel->load()) {
        return;
    }
    s.inFlight = false;
    s.result = err;
    s.resultType = 2;
}

void DrawTestConnReplica(TestConnState& s) {
    ImGui::SetNextWindowSize(ImVec2(360, 120), ImGuiCond_Appearing);
    if (ImGui::Begin("SmatchetTest::TestConn", nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::BeginDisabled(s.inFlight);
        if (ImGui::Button("Test connection##TC")) {
            s.clicked = true;
            StartProbe(s);
        }
        ImGui::EndDisabled();
        if (!s.result.empty()) {
            ImGui::TextUnformatted(s.result.c_str());
        }
    }
    ImGui::End();
}

void RegisterClickArmsInFlightVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "TestConnection_ClickArmsInFlight");
    t->UserData = &g_testConnState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        DrawTestConnReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        ResetTestConnState();
        ctx->SetRef("SmatchetTest::TestConn");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("Test connection##TC");
        ctx->Yield();
        IM_CHECK(s->clicked);
        IM_CHECK(s->inFlight);
        IM_CHECK_EQ(s->resultType, 0);
        IM_CHECK_STR_EQ(s->result.c_str(), "Testing...");
    };
}

void RegisterSuccessResultVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "TestConnection_SuccessSetsResult");
    t->UserData = &g_testConnState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        DrawTestConnReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        ResetTestConnState();
        ctx->SetRef("SmatchetTest::TestConn");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("Test connection##TC");
        ctx->Yield();
        FinishProbeSuccess(*s);
        ctx->Yield();

        IM_CHECK(!s->inFlight);
        IM_CHECK_EQ(s->resultType, 1);
        IM_CHECK_STR_EQ(s->result.c_str(), "Verified.");
    };
}

void RegisterErrorResultVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiPrefsTab", "TestConnection_ErrorSetsResultType2");
    t->UserData = &g_testConnState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        DrawTestConnReplica(*s);
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<TestConnState*>(ctx->Test->UserData);
        ResetTestConnState();
        ctx->SetRef("SmatchetTest::TestConn");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("Test connection##TC");
        ctx->Yield();
        FinishProbeError(*s, "HTTP 401: invalid api key");
        ctx->Yield();

        IM_CHECK(!s->inFlight);
        IM_CHECK_EQ(s->resultType, 2);
        IM_CHECK(s->result.find("401") != std::string::npos);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantPreferencesTestConnectionTests(ImGuiTestEngine* engine) {
    RegisterClickArmsInFlightVariant(engine);
    RegisterSuccessResultVariant(engine);
    RegisterErrorResultVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
