// ai_assistant_input_paste_overflow.test.cpp — bucket-E ASan regression for
// Issue #1699: pasting > cap bytes into the Assistant chat input must NOT
// overflow the fixed process-static buffer.
//
// THE BUG (pre-fix): SmatchetAiAssistantUi.cpp draws the chat input with
//   ImGui::InputTextMultiline("##AiAssistantInput", s_inputCharBuf.data(),
//                             s_inputCharBuf.size(), ..., ImGuiInputTextFlags_CallbackResize,
//                             &InputBufferResizeCallback);
// The CallbackResize flag sets `is_resizable=true` inside ImGui, which DISABLES
// the insert-time capacity clamp (STB_TEXTEDIT_INSERTCHARS) and lets ImGui grow
// its internal edit state to hold the full over-cap paste. At apply time
// (InputTextEx, imgui_widgets.cpp) ImGui calls the resize callback with BufSize
// enlarged to needed+1, then reads that BufSize back as its working capacity,
// clamps the applied length to capacity minus one, and ImStrncpy's that many
// bytes into the buffer — copying the full over-cap paste into the fixed cap-byte
// buffer. The original callback recorded the dropped-byte count and returned 0
// WITHOUT resetting BufSize, so it stayed enlarged → static-buffer overflow.
//
// THE FIX (under test): InputBufferResizeCallback now sets `data->BufSize =
// kInputBufCap` (leaving Buf/BufTextLen), so apply_new_text_length clamps to
// kInputBufCap-1 and the copy fits exactly.
//
// THIS TEST replicates the exact ImGui interaction (a fixed cap-byte buffer +
// an InputTextMultiline with CallbackResize + a callback that clamps BufSize)
// and pastes a 2*cap-byte string via ImGui's internal clipboard. PASS CONDITION:
// built + run under AddressSanitizer, the process exits with ZERO reports AND
// the buffer content is clamped to cap-1 + NUL-terminated. Pre-fix (callback
// WITHOUT `data->BufSize = cap`) this body writes 2*cap bytes into a cap-byte
// buffer and ASan aborts with a global-buffer-overflow — so removing the clamp
// re-reds this test under the ASan bucket-E lane.
//
// Drift warning — this REPLICATES SmatchetAiAssistantUi.cpp's
// InputBufferResizeCallback (the production callback + s_inputCharBuf are
// anonymous-namespace and unreachable from a test TU). IF YOU CHANGE that
// callback's BufSize-clamp contract, update this replica to match.

#if defined(SMATCHET_BUILD_UI_TESTS) && defined(SMATCHET_WITH_AI)

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>

namespace {

// TU-local global (ASan instruments globals with redzones, so an over-cap write
// past index kCap-1 lands in a redzone and is caught) — mirrors the production
// buffer being a process-static std::array.
constexpr int kCap = 8 * 1024; // == SmatchetAiAssistantUi.cpp kInputBufCap
char g_pasteBuf[kCap] = {0};
bool g_resizeCallbackFired = false;

// Replica of SmatchetAiAssistantUi.cpp:InputBufferResizeCallback — the #1699 fix.
int PasteResizeCallbackReplica(ImGuiInputTextCallbackData* data) {
    if (data != nullptr && data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        g_resizeCallbackFired = true;
        // The fix: clamp the reported capacity back to the real fixed cap so
        // ImGui's post-callback ImStrncpy copies at most kCap bytes. We do NOT
        // resize Buf. (Removing this line reproduces the #1699 overflow: ASan
        // aborts on the apply-time copy of the full over-cap paste.)
        data->BufSize = kCap;
    }
    return 0; // did NOT resize Buf
}

void DrawPasteReplica() {
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_CallbackResize;
    const float h = ImGui::GetTextLineHeight() * 3.0f;
    ImGui::InputTextMultiline("##AiAssistantInput", g_pasteBuf, kCap, ImVec2(-1.0f, h), flags,
                              &PasteResizeCallbackReplica);
}

void RegisterPasteOverflowVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "AiAssistant", "AssistantInput_OverCapPasteDoesNotOverflow");

    t->GuiFunc = [](ImGuiTestContext* /*ctx*/) {
        ImGui::SetNextWindowSize(ImVec2(480, 140), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::AssistantPasteOverflow", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            DrawPasteReplica();
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        std::memset(g_pasteBuf, 0, kCap);
        g_resizeCallbackFired = false;

        // A paste TWICE the buffer capacity — the exact over-cap scenario #1699
        // is about (the truncation toast is meant to report it, not corrupt memory).
        static char huge[2 * kCap];
        std::memset(huge, 'A', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = '\0';
        ImGui::SetClipboardText(huge);

        ctx->SetRef("SmatchetTest::AssistantPasteOverflow");
        ctx->Yield();
        ctx->Yield();

        ctx->ItemClick("##AiAssistantInput");
        // Ctrl+V paste — routes clipboard text into a single InsertChars, the
        // over-cap grow that (pre-fix) overflowed at apply time.
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_V);
        ctx->Yield();
        ctx->Yield();

        // The resize callback must have fired (over-cap paste hit the grow path).
        IM_CHECK(g_resizeCallbackFired);
        // Content clamped to cap-1 + NUL-terminated — no overflow. (ASan is the
        // primary oracle for the memory-safety half; these assert the observable
        // clamp.)
        const std::size_t len = std::strlen(g_pasteBuf);
        IM_CHECK(len <= static_cast<std::size_t>(kCap - 1));
        IM_CHECK_EQ(g_pasteBuf[kCap - 1], '\0');
    };
}

} // namespace

extern "C" void SmatchetRegisterAiAssistantInputPasteOverflowTests(ImGuiTestEngine* engine) {
    RegisterPasteOverflowVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS && SMATCHET_WITH_AI
