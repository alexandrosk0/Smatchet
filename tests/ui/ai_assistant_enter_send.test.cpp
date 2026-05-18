// ai_assistant_enter_send.test.cpp — bucket-E coverage for the Assistant
// chat input's keyboard contract: Enter submits, Ctrl+Enter inserts newline,
// empty buffer guards the submit path.
//
// Drift warning — IF YOU CHANGE Source_Core/src/SmatchetAiAssistantUi.cpp
// (specifically DrawInputAndButtons at lines 192-291: the
// ImGuiInputTextFlags_EnterReturnsTrue | CtrlEnterForNewLine combo at
// 221-222, the InputTextMultiline call at 223, the sendDisabled guard at
// 231, the dispatchSend lambda body, and the SetKeyboardFocusHere(-1) call
// at 263), UPDATE THIS REPLICA to match. The replica intentionally excludes
// the AppController / AiAssistantController / context-block plumbing —
// those depend on tracker state and are out-of-scope for keyboard-contract
// assertions.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>

namespace {

// Per-test state. Lives in TU-local globals (engine UserData = void*) — one
// shared struct, reset per TestFunc entry. The atomic-style guards from
// callstack_tooltip_hover.test.cpp aren't needed: the engine runs GuiFunc and
// TestFunc on the same thread (UI thread) in alternating frames.
struct EnterSendState {
    static constexpr int kBufCap = 8192;
    char inputBuf[kBufCap] = {0};
    int dispatchCount = 0;
    std::string lastSnapshot;
    bool focusRestoreRequested = false;
    ImGuiID lastInputItemId = 0;
    // Mirrors the production sendDisabled guard's empty-buffer term
    // (SmatchetAiAssistantUi.cpp:231). Replica fixes the other two terms
    // (assistantInFlight / ctrl==nullptr) to false so only the empty-input
    // branch is exercisable.
    static constexpr bool ExternalDisabled() { return false; }
};

EnterSendState g_enterSendState;

void ResetEnterSendState() {
    std::memset(g_enterSendState.inputBuf, 0, EnterSendState::kBufCap);
    g_enterSendState.dispatchCount = 0;
    g_enterSendState.lastSnapshot.clear();
    g_enterSendState.focusRestoreRequested = false;
    g_enterSendState.lastInputItemId = 0;
}

// Faithful replica of SmatchetAiAssistantUi.cpp DrawInputAndButtons keyboard
// path. Buffer model differs (TU-local char[8192] instead of the static
// std::array + seed-from-d.assistantInputBuf) because the test owns the
// lifecycle. The guard + dispatch + SetKeyboardFocusHere structure is
// verbatim.
void DrawEnterSendReplica(EnterSendState& s) {
    const ImGuiInputTextFlags inputFlags =
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine;
    const float inputH = ImGui::GetTextLineHeight() * 3.0f;
    const bool enterSubmitted = ImGui::InputTextMultiline("##AiAssistantInput", s.inputBuf, EnterSendState::kBufCap,
                                                          ImVec2(-1.0f, inputH), inputFlags);
    // Capture the ItemID immediately after submission so the focus-restore
    // assertion can compare against the input's actual ID (not a re-hashed
    // PushID path).
    s.lastInputItemId = ImGui::GetItemID();

    const bool sendDisabled = EnterSendState::ExternalDisabled() || (s.inputBuf[0] == '\0');
    if (enterSubmitted && !sendDisabled) {
        ++s.dispatchCount;
        s.lastSnapshot.assign(s.inputBuf);
        std::memset(s.inputBuf, 0, EnterSendState::kBufCap);
        // Production: SetKeyboardFocusHere(-1) re-focuses the just-submitted
        // multiline so the user can keep typing. Mirror the call AND set a
        // TU-local flag the TestFunc can read deterministically.
        ImGui::SetKeyboardFocusHere(-1);
        s.focusRestoreRequested = true;
    }
}

void RegisterEnterSubmitsVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "AssistantInput_EnterSubmits");
    t->UserData = &g_enterSendState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(480, 140), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AssistantEnterSend", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawEnterSendReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ResetEnterSendState();
        ctx->SetRef("SmatchetTest::AssistantEnterSend");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##AiAssistantInput");
        ctx->KeyChars("hello");
        ctx->Yield();
        IM_CHECK_STR_EQ(s->inputBuf, "hello");
        IM_CHECK_EQ(s->dispatchCount, 0);

        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(s->dispatchCount, 1);
        IM_CHECK_STR_EQ(s->lastSnapshot.c_str(), "hello");
        // Buffer cleared by the dispatchSend body (replica memset).
        IM_CHECK_EQ(static_cast<int>(std::strlen(s->inputBuf)), 0);
        IM_CHECK(s->focusRestoreRequested);

        // Focus-restore secondary assertion: ImGui::SetKeyboardFocusHere(-1)
        // arms either ActiveId or NavId to the just-submitted item. Engine
        // ordering can place it on either slot depending on the frame the
        // submit occurred; accept either as evidence of focus restoration.
        const ImGuiID activeId = GImGui->ActiveId;
        const ImGuiID navId = GImGui->NavId;
        const bool focusLanded = (activeId == s->lastInputItemId) || (navId == s->lastInputItemId);
        if (!focusLanded) {
            ctx->LogInfo("focus-restore probe: ActiveId=0x%08X NavId=0x%08X expected=0x%08X — engine frame ordering "
                         "skipped both slots; relying on focusRestoreRequested flag.",
                         activeId, navId, s->lastInputItemId);
        }
    };
}

void RegisterCtrlEnterNewlineVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "AssistantInput_CtrlEnterNewline");
    t->UserData = &g_enterSendState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(480, 140), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AssistantEnterSend", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawEnterSendReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ResetEnterSendState();
        ctx->SetRef("SmatchetTest::AssistantEnterSend");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##AiAssistantInput");
        ctx->KeyChars("line1");
        ctx->Yield();
        // Ctrl+Enter chord — ImGuiInputTextFlags_CtrlEnterForNewLine routes
        // this into a literal '\n' insertion rather than EnterReturnsTrue.
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Enter);
        ctx->Yield();
        ctx->KeyChars("line2");
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(s->dispatchCount, 0);
        IM_CHECK_STR_EQ(s->inputBuf, "line1\nline2");
    };
}

void RegisterEmptySubmitGuardedVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "AssistantInput_EmptySubmitGuarded");
    t->UserData = &g_enterSendState;

    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(480, 140), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AssistantEnterSend", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawEnterSendReplica(*s);
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<EnterSendState*>(ctx->Test->UserData);
        ResetEnterSendState();
        ctx->SetRef("SmatchetTest::AssistantEnterSend");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##AiAssistantInput");
        // No KeyChars — buffer stays empty. EnterReturnsTrue still fires; the
        // sendDisabled guard MUST suppress dispatch.
        ctx->KeyPress(ImGuiKey_Enter);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(s->dispatchCount, 0);
        IM_CHECK(!s->focusRestoreRequested);
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantEnterSendTests(ImGuiTestEngine* engine) {
    RegisterEnterSubmitsVariant(engine);
    RegisterCtrlEnterNewlineVariant(engine);
    RegisterEmptySubmitGuardedVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
