// grid_wheel_route_ownership.test.cpp -- bucket-E regression coverage for
// active-project grid edge-wheel ownership.
//
// The production route helper is TU-local in SmatchetActiveProjectGridUi.cpp.
// A SMATCHET_BUILD_UI_TESTS-only bridge forwards the current table into the real
// helper so these tests exercise the same cursor / wheeling / tooltip ownership
// guard as the active project grid without requiring tracker data.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "GridPane.h"
#include "SmatchetUiSession.h"
#include "Ui/SmatchetTooltipWheelRouter.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cmath>
#include <cstring>

extern "C" void SmatchetUiTestRouteActiveProjectGridWheelForCurrentTable(UiDrawSession* d, GridPane* pane);

namespace {

struct WheelRouteState {
    UiDrawSession session;
    GridPane pane;
    ImVec2 tableCenter{0.0f, 0.0f};
    ImVec2 otherOverlapPoint{0.0f, 0.0f};
    ImVec2 tooltipCenter{0.0f, 0.0f};
    float scrollX = 0.0f;
    float scrollMaxX = 0.0f;
    float scrollY = 0.0f;
    float scrollMaxY = 0.0f;
    float tooltipScrollY = 0.0f;
    float tooltipScrollMaxY = 0.0f;
    bool tableReady = false;
    bool tooltipReady = false;
    bool showScrollableTooltip = false;
    bool resetScroll = false;
    char hoveredWindowName[128] = {};
};

WheelRouteState g_state;

const ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

void ResetState(WheelRouteState& s) {
    s.session.cfg.GridEndWheelSwallowsBeforeHorizontal = 0;
    s.pane.gridBottomHorizontalWheelSwallowsRemaining = 0;
    s.pane.gridTopHorizontalWheelSwallowsRemaining = 0;
    s.scrollX = 0.0f;
    s.scrollMaxX = 0.0f;
    s.scrollY = 0.0f;
    s.scrollMaxY = 0.0f;
    s.otherOverlapPoint = ImVec2(0.0f, 0.0f);
    s.tooltipScrollY = 0.0f;
    s.tooltipScrollMaxY = 0.0f;
    s.tableReady = false;
    s.tooltipReady = false;
    s.showScrollableTooltip = false;
    s.resetScroll = true;
    s.hoveredWindowName[0] = '\0';
}

bool NearlyZero(float v) { return std::fabs(v) < 0.5f; }

void CaptureHoveredWindow(WheelRouteState& s) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    const char* name = (g.HoveredWindow != nullptr) ? g.HoveredWindow->Name : "";
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    std::strncpy(s.hoveredWindowName, name, sizeof(s.hoveredWindowName) - 1);
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    s.hoveredWindowName[sizeof(s.hoveredWindowName) - 1] = '\0';
}

void DrawOtherWindow(WheelRouteState& s) {
    ImGui::SetNextWindowPos(ImVec2(245, 120), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(260, 180), ImGuiCond_Always);
    if (ImGui::Begin("SmatchetTest::WheelRouteOther", nullptr, kWindowFlags)) {
        ImGui::Button("Cursor owner##WheelRouteOtherAnchor");
        ImGuiWindow* window = ImGui::GetCurrentWindow();
        if (window != nullptr) {
            const ImRect rect = window->Rect();
            s.otherOverlapPoint = ImVec2(rect.Min.x + 40.0f, rect.Min.y + 70.0f);
        }
        CaptureHoveredWindow(s);
    }
    ImGui::End();
}

void DrawScrollableTooltip(WheelRouteState& s, const ImRect& tableRect) {
    s.tooltipReady = false;
    ImGui::SetNextWindowPos(ImVec2(tableRect.Min.x + 24.0f, tableRect.Min.y + 24.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(190.0f, 100.0f), ImGuiCond_Always);
    if (ImGui::BeginTooltip()) {
        if (ImGui::BeginChild("ScrollableTooltipOwner", ImVec2(170.0f, 72.0f), true,
                              ImGuiWindowFlags_AlwaysVerticalScrollbar)) {
            for (int i = 0; i < 24; ++i) {
                ImGui::Text("tooltip row %02d", i);
            }
            ImGuiWindow* child = ImGui::GetCurrentWindow();
            if (child != nullptr) {
                const ImRect childRect = child->Rect();
                s.tooltipCenter = ImVec2((childRect.Min.x + childRect.Max.x) * 0.5f,
                                         (childRect.Min.y + childRect.Max.y) * 0.5f);
                s.tooltipScrollY = child->Scroll.y;
                s.tooltipScrollMaxY = child->ScrollMax.y;
                s.tooltipReady = s.tooltipScrollMaxY > 0.0f;
            }
        }
        ImGui::EndChild();
        ImGui::EndTooltip();
    }
}

void DrawGridWindow(WheelRouteState& s) {
    ImGui::SetNextWindowPos(ImVec2(60, 80), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(330, 220), ImGuiCond_Always);
    (void)smatchet::ui::RouteWheelToScrollableTooltip();
    if (ImGui::Begin("SmatchetTest::WheelRouteGrid", nullptr, kWindowFlags)) {
        const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollX |
                                      ImGuiTableFlags_ScrollY;
        if (ImGui::BeginTable("TicketGrid", 8, flags, ImVec2(260.0f, 130.0f))) {
            for (int c = 0; c < 8; ++c) {
                ImGui::TableSetupColumn("WideColumn", ImGuiTableColumnFlags_WidthFixed, 150.0f);
            }
            ImGui::TableHeadersRow();
            const int rowCount = s.showScrollableTooltip ? 24 : 4;
            for (int r = 0; r < rowCount; ++r) {
                ImGui::TableNextRow();
                for (int c = 0; c < 8; ++c) {
                    ImGui::TableSetColumnIndex(c);
                    ImGui::Text("R%d C%d content", r, c);
                }
            }

            ImGuiTable* table = ImGui::GetCurrentTable();
            if (table != nullptr && table->OuterWindow != nullptr && table->InnerWindow != nullptr) {
                const ImRect tableRect = table->OuterWindow->Rect();
                s.tableCenter = ImVec2((tableRect.Min.x + tableRect.Max.x) * 0.5f,
                                       (tableRect.Min.y + tableRect.Max.y) * 0.5f);
                if (s.resetScroll) {
                    ImGui::SetScrollX(table->InnerWindow, 0.0f);
                    ImGui::SetScrollY(table->InnerWindow, 0.0f);
                    s.resetScroll = false;
                }
            }

            SmatchetUiTestRouteActiveProjectGridWheelForCurrentTable(&s.session, &s.pane);

            table = ImGui::GetCurrentTable();
            if (table != nullptr && table->InnerWindow != nullptr) {
                s.scrollX = table->InnerWindow->Scroll.x;
                s.scrollMaxX = table->InnerWindow->ScrollMax.x;
                s.scrollY = table->InnerWindow->Scroll.y;
                s.scrollMaxY = table->InnerWindow->ScrollMax.y;
                s.tableReady = s.scrollMaxX > 10.0f;
            }
            if (s.showScrollableTooltip && table != nullptr && table->OuterWindow != nullptr) {
                DrawScrollableTooltip(s, table->OuterWindow->Rect());
            }
            ImGui::EndTable();
        }
        CaptureHoveredWindow(s);
    }
    ImGui::End();
}

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 80) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

void RegisterTests(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "Grid", "WheelRouteOwnership_CursorAndTooltip");
    t->UserData = &g_state;
    t->GuiFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WheelRouteState*>(ctx->Test->UserData);
        DrawGridWindow(*s);
        DrawOtherWindow(*s);
    };
    t->TestFunc = [](ImGuiTestContext* ctx) {
        auto* s = static_cast<WheelRouteState*>(ctx->Test->UserData);

        ResetState(*s);
        ctx->SetRef("SmatchetTest::WheelRouteGrid");
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return s->tableReady; }));

        ctx->WindowFocus("//SmatchetTest::WheelRouteOther");
        ctx->MouseMoveToPos(s->tableCenter);
        ctx->MouseWheelY(-1.0f);
        ctx->Yield();
        ctx->Yield();
        if (s->scrollX <= 0.0f) {
            ctx->LogError("grid-under-cursor wheel did not route horizontally: scrollX=%.2f scrollMaxX=%.2f",
                          s->scrollX, s->scrollMaxX);
            IM_CHECK(false);
        }

        ResetState(*s);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] {
            return s->tableReady && NearlyZero(s->scrollX) && s->otherOverlapPoint.x > 0.0f;
        }));
        ctx->MouseMoveToPos(s->otherOverlapPoint);
        ctx->Yield();
        ctx->MouseWheelY(-1.0f);
        ctx->Yield();
        ctx->Yield();
        if (std::strstr(s->hoveredWindowName, "WheelRouteOther") == nullptr) {
            ctx->LogError("expected other window to own hover, hoveredWindowName='%s'", s->hoveredWindowName);
            IM_CHECK(false);
        }
        if (!NearlyZero(s->scrollX)) {
            ctx->LogError("grid routed wheel while another window owned cursor hover: scrollX=%.2f hover='%s'",
                          s->scrollX, s->hoveredWindowName);
            IM_CHECK(false);
        }

        ResetState(*s);
        s->showScrollableTooltip = true;
        ctx->WindowFocus("//SmatchetTest::WheelRouteGrid");
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return s->tableReady && s->tooltipReady; }));
        ctx->MouseMoveToPos(s->tooltipCenter);
        ctx->Yield();
        ctx->MouseWheelY(-1.0f);
        const bool tooltipScrolled = YieldUntil(ctx, [&] { return s->tooltipScrollY > 0.0f; }, 12);
        if (!tooltipScrolled) {
            ctx->LogError("wheel over scrollable tooltip did not scroll tooltip: tooltipScrollY=%.2f tooltipMaxY=%.2f",
                          s->tooltipScrollY, s->tooltipScrollMaxY);
            IM_CHECK(false);
        }
        if (!NearlyZero(s->scrollX)) {
            ctx->LogError("grid routed wheel while scrollable tooltip owned hover/wheel: scrollX=%.2f tooltipMaxY=%.2f",
                          s->scrollX, s->tooltipScrollMaxY);
            IM_CHECK(false);
        }
        if (!NearlyZero(s->scrollY)) {
            ctx->LogError("grid scrolled vertically while scrollable tooltip owned wheel: scrollY=%.2f scrollMaxY=%.2f",
                          s->scrollY, s->scrollMaxY);
            IM_CHECK(false);
        }
    };
}

} // namespace

extern "C" void SmatchetRegisterGridWheelRouteOwnershipTests(ImGuiTestEngine* engine) { RegisterTests(engine); }

#endif // SMATCHET_BUILD_UI_TESTS
