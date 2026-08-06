// docked_tab_focus.test.cpp — bucket-E regression guard for
// SmatchetUI::selectDockedTab (Source/Core/src/Ui/SmatchetUI_Layout.cpp).
//
// Bug (fixed): User Info shares its dock node with Preferences. The focus
// paths raised it with ImGui::SetNextWindowFocus(), which cannot activate a
// DOCKED tab — upstream leaves FocusWindow()'s "select in dock node" lines
// commented out (ocornut/imgui#2304), and DockNodeUpdateTabBar only mirrors
// nav focus when g.NavWindow->RootWindow IS the node's own window, never true
// for a window docked into a host node. The tab bar kept whatever selection it
// made on the frame it was CREATED, so a window could come up behind its
// sibling and stay there — which is how the bucket-C user-info-* captures
// intermittently photographed the Preferences pane.
//
// Fix: selectDockedTab writes window->DockNode->TabBar->NextSelectedTabId
// directly and then calls ImGui::FocusWindow (the tab-bar chrome renders in the
// ImGuiCol_TabDimmed* palette unless a window inside the node holds nav focus).
//
// Drift warning — this test pins two upstream-fragile assumptions:
//   1. SetNextWindowFocus alone does NOT switch a docked tab. If upstream ever
//      restores imgui#2304's commented-out lines, variant 1's "control" half
//      starts passing on its own and selectDockedTab becomes redundant.
//   2. The window->DockNode->TabBar->NextSelectedTabId chain is imgui_internal
//      API with no stability guarantee.
// Either way the failure is loud here rather than silent in a masked
// bucket-C golden diff.
//
// Two variants:
//   1. DockedSibling_SelectDockedTabRaisesTab (PRIMARY) — docks two throwaway
//      windows into one private node, lets the tab bar settle on the sibling,
//      asserts SetNextWindowFocus leaves the selection alone, then asserts
//      selectDockedTab moves it. Skips (LogInfo, no failure) if the host never
//      realizes a tab bar — the reset_layout_docking Mesa-headless convention.
//   2. NotDocked_SelectDockedTabIsNoOp (DETERMINISTIC) — a floating window and
//      an unknown name must both take the early-return path without crashing
//      and without disturbing the caller's window. No host dependency.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "SmatchetUI.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow::DockNode / TabBar, DockBuilder*
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

namespace {

// Private node id for this test only — deliberately not one of
// SmatchetDockNodeIds' canonical ids, so the live host layout is untouched.
// The node is created in TestFunc and removed again at the end.
ImGuiID TestNodeId() { return ImHashStr("SmatchetTest::DockedTabFocusNode"); }

const char* kTabA = "SmatchetTest DockedTabFocus A";
const char* kTabB = "SmatchetTest DockedTabFocus B";

// Neither name is a localization key, so WindowTitleFromSource (which
// selectDockedTab applies before FindWindowByName) returns them verbatim in
// every locale — the lookup inside the function resolves to these windows.
struct FocusState {
    bool visible = false;
    bool requestFocusA = false;
};

FocusState g_focusState;

void DrawTwoWindows(FocusState& s) {
    if (!s.visible) {
        return;
    }
    if (s.requestFocusA) {
        // The "control" half of variant 1: the pre-fix call shape.
        ImGui::SetNextWindowFocus();
    }
    if (ImGui::Begin(kTabA, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("(pane A)");
    }
    ImGui::End();

    if (ImGui::Begin(kTabB, nullptr, ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted("(pane B)");
    }
    ImGui::End();
}

ImGuiTabBar* TabBarOf(const char* name) {
    ImGuiWindow* w = ImGui::FindWindowByName(name);
    if (w == nullptr || w->DockNode == nullptr) {
        return nullptr;
    }
    return w->DockNode->TabBar;
}

// ---------------------------------------------------------------------------
// Variant 1 (PRIMARY): selectDockedTab raises a docked sibling; focus alone does not.
// ---------------------------------------------------------------------------
void RegisterDockedSiblingVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DockedTabFocus", "DockedSibling_SelectDockedTabRaisesTab");
    t->UserData = &g_focusState;

    t->GuiFunc = [](ImGuiTestContext* ctx) { DrawTwoWindows(*static_cast<FocusState*>(ctx->Test->UserData)); };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        FocusState* s = static_cast<FocusState*>(ctx->Test->UserData);
        s->visible = true;
        s->requestFocusA = false;

        if ((ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_DockingEnable) == 0) {
            ctx->LogInfo("skip: docking disabled in this host configuration");
            s->visible = false;
            return;
        }

        // Build a private node and dock both windows into it, B last so the tab
        // bar settles on B — the "sibling in front" state the bug produced.
        const ImGuiID nodeId = TestNodeId();
        ImGui::DockBuilderRemoveNode(nodeId);
        ImGui::DockBuilderAddNode(nodeId, ImGuiDockNodeFlags_None);
        ImGui::DockBuilderSetNodeSize(nodeId, ImVec2(400, 300));
        ImGui::DockBuilderDockWindow(kTabA, nodeId);
        ImGui::DockBuilderDockWindow(kTabB, nodeId);
        ImGui::DockBuilderFinish(nodeId);

        // Poll rather than a fixed frame count: a slow Mesa settle must not read
        // as the bug. The tab bar is created by DockNodeUpdateTabBar once the
        // node has a host window, which headless hosts may never provide.
        ImGuiTabBar* bar = nullptr;
        for (int i = 0; i < 60 && bar == nullptr; ++i) {
            ctx->Yield();
            bar = TabBarOf(kTabA);
        }
        if (bar == nullptr) {
            ctx->LogInfo("skip: dock node never realized a tab bar — headless host");
            ImGui::DockBuilderRemoveNode(nodeId);
            s->visible = false;
            return;
        }

        ImGuiWindow* wa = ImGui::FindWindowByName(kTabA);
        ImGuiWindow* wb = ImGui::FindWindowByName(kTabB);
        IM_CHECK(wa != nullptr);
        IM_CHECK(wb != nullptr);

        // Put B in front explicitly so the assertions below have a known start.
        bar->NextSelectedTabId = wb->TabId;
        ctx->Yield();
        ctx->Yield();
        IM_CHECK_EQ(bar->VisibleTabId, wb->TabId);

        // Control: SetNextWindowFocus on A does NOT switch the docked tab.
        // This is imgui#2304 — the precise reason selectDockedTab exists.
        s->requestFocusA = true;
        ctx->Yield();
        ctx->Yield();
        s->requestFocusA = false;
        bar = TabBarOf(kTabA);
        IM_CHECK(bar != nullptr);
        IM_CHECK_EQ(bar->VisibleTabId, wb->TabId);

        // The fix: selectDockedTab writes the selection directly.
        SmatchetUI::selectDockedTab(kTabA);
        bar = TabBarOf(kTabA);
        IM_CHECK(bar != nullptr);
        IM_CHECK_EQ(bar->NextSelectedTabId, wa->TabId);

        ctx->Yield();
        ctx->Yield();
        bar = TabBarOf(kTabA);
        IM_CHECK(bar != nullptr);
        IM_CHECK_EQ(bar->VisibleTabId, wa->TabId);

        // Leave no private node behind for the rest of the suite.
        ImGui::DockBuilderRemoveNode(nodeId);
        s->visible = false;
        ctx->Yield();
    };
}

// ---------------------------------------------------------------------------
// Variant 2 (DETERMINISTIC): undocked / unknown names take the early return.
// ---------------------------------------------------------------------------
void RegisterNotDockedVariant(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "DockedTabFocus", "NotDocked_SelectDockedTabIsNoOp");

    t->GuiFunc = [](ImGuiTestContext*) {
        ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_Appearing);
        if (ImGui::Begin("SmatchetTest::DockedTabFocusFloating", nullptr,
                         ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::TextUnformatted("floating probe");
        }
        ImGui::End();
    };

    t->TestFunc = [](ImGuiTestContext* ctx) {
        ctx->Yield();
        ctx->Yield();

        ImGuiWindow* floating = ImGui::FindWindowByName("SmatchetTest::DockedTabFocusFloating");
        IM_CHECK(floating != nullptr);
        IM_CHECK(floating->DockNode == nullptr); // NoDocking — never joins a node

        // Both of these must hit the `window == nullptr || DockNode == nullptr`
        // early return. A regression that dereferences before the null checks
        // crashes the suite here instead of corrupting a screenshot.
        SmatchetUI::selectDockedTab("SmatchetTest::DockedTabFocusFloating");
        SmatchetUI::selectDockedTab("SmatchetTest::NoSuchWindowExists");
        ctx->Yield();

        // The floating window is untouched — still present, still undocked.
        floating = ImGui::FindWindowByName("SmatchetTest::DockedTabFocusFloating");
        IM_CHECK(floating != nullptr);
        IM_CHECK(floating->DockNode == nullptr);
    };
}

} // namespace

extern "C" void SmatchetRegisterDockedTabFocusTests(ImGuiTestEngine* engine) {
    RegisterDockedSiblingVariant(engine);
    RegisterNotDockedVariant(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
