#include <doctest/doctest.h>

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"
#include "SmatchetUiDensity.h"
#include "imgui.h"

#include <cmath>

// Covers the inverted Norton Commander palette: blue dominates panels/chrome, teal becomes the
// accent / selection, body text + borders + scrollbar grab are cyan #00FFFF, MenuBarBg is the
// NC 2.01 gray strip, and PopupBg uses teal to match the NC dropdown look. Regression-guards
// the colour assignments in ApplyNortonCommander against silent drift.

namespace {

struct ImGuiCtxFixture {
    ImGuiCtxFixture() : ctx_(ImGui::CreateContext()) {}
    ~ImGuiCtxFixture() {
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    ImGuiContext* ctx_;
};

bool ApproxEq(const ImVec4& c, float r, float g, float b, float a) {
    const float diff[4] = {std::fabs(c.x - r), std::fabs(c.y - g), std::fabs(c.z - b), std::fabs(c.w - a)};
    for (int i = 0; i < 4; ++i) {
        if (diff[i] > 1e-3f) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::ApplyStyle Norton — inverted palette dominance") {
    SmatchetTheme::ApplyStyle(ThemeId::NortonCommander);
    const ImVec4* colors = ImGui::GetStyle().Colors;

    // Body text + borders + scrollbar grab = cyan (#00FFFF).
    CHECK(ApproxEq(colors[ImGuiCol_Text], 0.00f, 1.00f, 1.00f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_Border], 0.00f, 1.00f, 1.00f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_Separator], 0.00f, 1.00f, 1.00f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_ScrollbarGrab], 0.00f, 1.00f, 1.00f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_TableBorderStrong], 0.00f, 1.00f, 1.00f, 1.00f));

    // Panel background = blue (#0000AA) — the inverted dominant.
    CHECK(ApproxEq(colors[ImGuiCol_WindowBg], 0.00f, 0.00f, 0.667f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_Tab], 0.00f, 0.00f, 0.667f, 1.00f));

    // Selection / header = teal (#008080) — the inverted accent.
    CHECK(ApproxEq(colors[ImGuiCol_Header], 0.00f, 0.50f, 0.50f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_TabActive], 0.00f, 0.50f, 0.50f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_ButtonActive], 0.00f, 0.50f, 0.50f, 1.00f));

    // NC 2.01 menu bar strip = gray (#AAAAAA).
    CHECK(ApproxEq(colors[ImGuiCol_MenuBarBg], 0.667f, 0.667f, 0.667f, 1.00f));

    // NC dropdown = teal accent (matches the screenshot's dropdown bg).
    CHECK(ApproxEq(colors[ImGuiCol_PopupBg], 0.00f, 0.50f, 0.50f, 1.00f));

    // Yellow retained as highlight accent on nav + drag-drop + scrollbar-active.
    CHECK(ApproxEq(colors[ImGuiCol_NavHighlight], 1.00f, 1.00f, 0.333f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_DragDropTarget], 1.00f, 1.00f, 0.333f, 1.00f));
    CHECK(ApproxEq(colors[ImGuiCol_ScrollbarGrabActive], 1.00f, 1.00f, 0.333f, 1.00f));
}
