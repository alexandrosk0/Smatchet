// views_columns_reorder.test.cpp — first bucket-E test, targets the
// SmatchetViewsDashboardUi.cpp Columns-tab drag-reorder bug.
//
// Hot lead recap (see docs/plans/shipped/imgui-test-engine-bucket-e-execution.md
// § Context):
//   - SmatchetViewsDashboardUi.cpp:683 submits an `ImGui::Selectable(...)`.
//   - :687-691 conditionally calls `ImGui::TextDisabled(...)` for a width
//     hint (only when ColumnWidths[key] > 0).
//   - :692 calls `HandleRowReorder(...)` which binds
//     `ImGui::BeginDragDropTarget()` to the LAST-SUBMITTED ImGui item.
//   - When TextDisabled fires, the drop-target rect is the tiny disabled-text
//     rect; when it doesn't, the rect is the row Selectable. Inconsistent
//     rect → inconsistent drop hit-test → user-visible flake.
//
// This test re-creates the exact call-site shape in a test-engine-owned
// window so we don't have to spin up the real Views → Jira window machinery
// (which depends on AppController + tracker backend + view storage). The
// helpers `DrawDragHandle` / `HandleRowReorder` from Source_Core/src are
// reused verbatim — we are testing the *call site*, not the helpers.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetViewsDashboardUi_detail.h"

#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"

#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

// Per-test seed + observation state. Lives in TU-local globals because the
// engine's GuiFunc / TestFunc lambdas have no shared user-data slot in this
// version — the cleaner alternative would be ImGuiTest::SetUserDataType().
struct ColumnsReorderState {
    std::vector<std::string> order;
    std::unordered_map<std::string, float> widths;
    int keyboardFocusRow = -1;
    bool reorderHappened = false;
};

ColumnsReorderState g_state_widthsOff;
ColumnsReorderState g_state_widthsOn;

void ResetState(ColumnsReorderState& s, bool seedWidths) {
    s.order = {"id", "summary", "status"};
    s.widths.clear();
    if (seedWidths) {
        // Force the conditional TextDisabled branch (line 687-691) by
        // priming column widths > 0 for every column. This exercises the
        // hot-lead path where the drop target binds to the disabled-text
        // rect instead of the row Selectable.
        s.widths["id"] = 64.0f;
        s.widths["summary"] = 320.0f;
        s.widths["status"] = 96.0f;
    }
    s.keyboardFocusRow = -1;
    s.reorderHappened = false;
}

// Faithful replica of the production Columns-tab loop body in
// SmatchetViewsDashboardUi.cpp. Any divergence here invalidates the test —
// keep this loop in lock-step with the production body.
void DrawColumnsTabBody(ColumnsReorderState& s) {
    for (int i = 0; i < static_cast<int>(s.order.size()); ++i) {
        const std::string key = s.order[static_cast<size_t>(i)];
        ImGui::PushID(i);
        ImGui::BeginGroup();
        SmatchetViewsDashboardUiDetail::DrawDragHandle("##h", i, "VIEWS_COLUMNS_ROW");
        char rowBuf[64];
        std::snprintf(rowBuf, sizeof(rowBuf), "%d.", i + 1);
        ImGui::TextUnformatted(rowBuf);
        ImGui::SameLine();
        const bool selected = (s.keyboardFocusRow == i);
        if (ImGui::Selectable(key.c_str(), selected, ImGuiSelectableFlags_AllowOverlap)) {
            s.keyboardFocusRow = i;
        }
        auto wit = s.widths.find(key);
        if (wit != s.widths.end() && wit->second > 0.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("  %.0fpx", wit->second);
        }
        ImGui::EndGroup();
        if (SmatchetViewsDashboardUiDetail::HandleRowReorder(i, s.order, &s.keyboardFocusRow, "VIEWS_COLUMNS_ROW")) {
            s.reorderHappened = true;
        }
        ImGui::PopID();
    }
}

void RegisterOneVariant(ImGuiTestEngine* engine, const char* name, ColumnsReorderState& state, bool seedWidths) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Views", name);
    t->UserData = &state;

    // GuiFunc — emits the test surface every frame the engine ticks the test.
    t->GuiFunc = [seedWidths](ImGuiTestContext* ctx) {
        auto* st = static_cast<ColumnsReorderState*>(ctx->Test->UserData);
        ImGui::SetNextWindowSize(ImVec2(420, 180), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::ColumnsReorder")) {
            DrawColumnsTabBody(*st);
        }
        ImGui::End();
        (void)seedWidths;
    };

    // TestFunc — drives the drag + asserts the resulting order.
    t->TestFunc = [seedWidths](ImGuiTestContext* ctx) {
        auto* st = static_cast<ColumnsReorderState*>(ctx->Test->UserData);
        ResetState(*st, seedWidths);

        ctx->SetRef("SmatchetTest::ColumnsReorder");
        // Let the GuiFunc rebuild the surface with the freshly-reset state.
        ctx->Yield();
        ctx->Yield();

        // Drag handle of row 2 onto handle of row 0. The drag-handle Item IDs
        // are nested: PushID(i) → PushID("##h") → InvisibleButton("##handle").
        // ItemDragAndDrop resolves the path "$$<rowIdx>/##h/##handle".
        ctx->ItemDragAndDrop("$$2/##h/##handle", "$$0/##h/##handle");

        IM_CHECK_EQ(static_cast<int>(st->order.size()), 3);
        IM_CHECK_STR_EQ(st->order[0].c_str(), "status");
        IM_CHECK(st->reorderHappened);
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsColumnsReorderTests(ImGuiTestEngine* engine) {
    // Two variants: ColumnWidths cleared (TextDisabled branch does NOT fire)
    // and ColumnWidths seeded (TextDisabled branch DOES fire). Exercising
    // both characterises the hot lead — if only the widths-on variant
    // flakes, the rect-inconsistency hypothesis is confirmed.
    RegisterOneVariant(engine, "ColumnsReorder_DragRow2_To_Top_NoWidths", g_state_widthsOff,
                       /*seedWidths*/ false);
    RegisterOneVariant(engine, "ColumnsReorder_DragRow2_To_Top_WithWidths", g_state_widthsOn,
                       /*seedWidths*/ true);
}

#endif // SMATCHET_BUILD_UI_TESTS
