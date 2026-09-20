// drag_checkbox_paint.test.cpp — bucket-E coverage for the drag-to-paint checkbox gesture
// (SmatchetDragCheckbox.h): press a checkbox and drag across its neighbours to set the whole
// run to the value the press produced. The pure decision core is unit-tested in
// tests/Core/SmatchetDragCheckboxPure.test.cpp; what only a live ImGui context can prove is
// the wiring around it — that the press is seen through ImGui's release-toggle timing, that
// the swept pointer paints the rows it crosses, that a disabled row mid-run stays as it is,
// and that a plain click still toggles exactly once.
//
// The rows here are the real widget (not a replica) drawn into the test's own window, so the
// gesture is exercised exactly as Views > Fields and the multi-select / Labels cell editors
// call it.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetDragCheckbox.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

const char* const kWindowRef = "SmatchetTest::DragCheckboxPaint";

// Per-test state. TU-local — the engine runs GuiFunc and TestFunc on the same (UI) thread in
// alternating frames, so no synchronisation is needed (same model as ai_assistant_enter_send).
struct DragPaintState {
    // Unnamed enum, not `static const int`: the IM_CHECK_* macros take their operands by
    // reference, which would ODR-use a constant this header-less TU never defines out of line.
    enum { kRows = 6, kDisabledRow = 3 }; // row 3 is locked mid-list: a run must step over it
    bool values[kRows];
    int changes[kRows]; // times the widget reported a change for that row
    ImVec2 centers[kRows];
    bool geometryReady;
};

DragPaintState g_dragPaintState;

void ResetDragPaintState() {
    for (int i = 0; i < DragPaintState::kRows; ++i) {
        g_dragPaintState.values[i] = false;
        g_dragPaintState.changes[i] = 0;
        g_dragPaintState.centers[i] = ImVec2(0.0f, 0.0f);
    }
    g_dragPaintState.geometryReady = false;
}

void DrawDragPaintRows(DragPaintState& s) {
    for (int i = 0; i < DragPaintState::kRows; ++i) {
        char id[32];
        ImFormatString(id, IM_ARRAYSIZE(id), "##row%d", i);
        const bool disabled = (i == DragPaintState::kDisabledRow);
        if (disabled) {
            ImGui::BeginDisabled();
        }
        if (SmatchetDragCheckbox(id, &s.values[i])) {
            ++s.changes[i];
        }
        // Captured here rather than through ctx->ItemInfo: the test engine does not register
        // disabled items, so the locked row has no queryable record to aim the drag at.
        s.centers[i] = ImVec2((ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x) * 0.5f,
                              (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f);
        if (disabled) {
            ImGui::EndDisabled();
        }
        ImGui::SameLine();
        ImGui::Text("row %d%s", i, disabled ? " (locked)" : "");
    }
    s.geometryReady = true;
}

void DragPaintGuiFunc(ImGuiTestContext* ctx) {
    DragPaintState* s = static_cast<DragPaintState*>(ctx->Test->UserData);
    ImGui::SetNextWindowSize(ImVec2(320.0f, 260.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(60.0f, 60.0f), ImGuiCond_Always);
    if (ImGui::Begin(kWindowRef, nullptr,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize)) {
        DrawDragPaintRows(*s);
    }
    ImGui::End();
}

// Bring the rows up and take the pointer to the first row, leaving the window focused and the
// per-row geometry captured.
void ArmRows(ImGuiTestContext* ctx) {
    ResetDragPaintState();
    ctx->SetRef(kWindowRef);
    ctx->WindowFocus(kWindowRef);
    ctx->Yield();
    ctx->Yield();
    ctx->MouseMove("##row0");
}

void RegisterDragPaintsCrossedRows(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DragCheckbox", "DragPaintsCrossedRowsAndSkipsDisabled");
    t->UserData = &g_dragPaintState;
    t->GuiFunc = DragPaintGuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        DragPaintState* s = static_cast<DragPaintState*>(ctx->Test->UserData);
        ArmRows(ctx);
        IM_CHECK(s->geometryReady);

        // Press row 0: the gesture toggles on the PRESS (ImGui's own Checkbox toggles on
        // release, which would never reach a row the drag has left).
        ctx->MouseDown(0);
        ctx->Yield();
        IM_CHECK_EQ(s->values[0], true);
        IM_CHECK_EQ(s->changes[0], 1);

        // Drag down through rows 1..4. Row 3 is disabled and must come out untouched.
        for (int row = 1; row <= 4; ++row) {
            ctx->MouseMoveToPos(s->centers[row]);
            ctx->Yield();
        }
        ctx->MouseUp(0);
        ctx->Yield();
        ctx->Yield();

        IM_CHECK_EQ(s->values[0], true);
        IM_CHECK_EQ(s->values[1], true);
        IM_CHECK_EQ(s->values[2], true);
        IM_CHECK_EQ(s->values[DragPaintState::kDisabledRow], false); // locked row stepped over
        IM_CHECK_EQ(s->values[4], true);
        IM_CHECK_EQ(s->values[5], false); // past where the drag ended
        // Every painted row reported exactly one change — call sites queue tracker edits and
        // set dirty flags off that return value.
        IM_CHECK_EQ(s->changes[1], 1);
        IM_CHECK_EQ(s->changes[2], 1);
        IM_CHECK_EQ(s->changes[4], 1);
        IM_CHECK_EQ(s->changes[DragPaintState::kDisabledRow], 0);
        IM_CHECK_EQ(s->changes[5], 0);
    };
}

void RegisterDragClearsCrossedRows(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DragCheckbox", "DragFromCheckedRowClearsTheRun");
    t->UserData = &g_dragPaintState;
    t->GuiFunc = DragPaintGuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        DragPaintState* s = static_cast<DragPaintState*>(ctx->Test->UserData);
        ArmRows(ctx);
        // Start from an all-checked list: the press must paint "unchecked" across the run.
        for (int i = 0; i < DragPaintState::kRows; ++i) {
            s->values[i] = true;
        }
        ctx->Yield();

        ctx->MouseDown(0);
        ctx->Yield();
        for (int row = 1; row <= 2; ++row) {
            ctx->MouseMoveToPos(s->centers[row]);
            ctx->Yield();
        }
        ctx->MouseUp(0);
        ctx->Yield();

        IM_CHECK_EQ(s->values[0], false);
        IM_CHECK_EQ(s->values[1], false);
        IM_CHECK_EQ(s->values[2], false);
        IM_CHECK_EQ(s->values[3], true); // never reached
        IM_CHECK_EQ(s->values[5], true);
    };
}

void RegisterPlainClickTogglesOnce(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DragCheckbox", "PlainClickStillTogglesExactlyOnce");
    t->UserData = &g_dragPaintState;
    t->GuiFunc = DragPaintGuiFunc;
    t->TestFunc = [](ImGuiTestContext* ctx) {
        DragPaintState* s = static_cast<DragPaintState*>(ctx->Test->UserData);
        ArmRows(ctx);

        // Press and release on the same row. The press applies the toggle and ImGui's own
        // release-toggle lands on the same row — the widget has to swallow the second one.
        ctx->MouseClick(0);
        ctx->Yield();
        IM_CHECK_EQ(s->values[0], true);
        IM_CHECK_EQ(s->changes[0], 1);

        // And a second click takes it back, once.
        ctx->MouseClick(0);
        ctx->Yield();
        IM_CHECK_EQ(s->values[0], false);
        IM_CHECK_EQ(s->changes[0], 2);

        // A run never starts without a press: hovering the other rows changed nothing.
        for (int i = 1; i < DragPaintState::kRows; ++i) {
            IM_CHECK_EQ(s->values[i], false);
        }
    };
}

} // namespace

extern "C" void SmatchetRegisterDragCheckboxPaintTests(ImGuiTestEngine* engine) {
    RegisterDragPaintsCrossedRows(engine);
    RegisterDragClearsCrossedRows(engine);
    RegisterPlainClickTogglesOnce(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
