// views_editor_header.test.cpp — bucket-E coverage for the Views editor header's action row
// (drawViewsEditorHeader, SmatchetViewsDashboardUi.cpp): Apply & Sync, then Discard, then Delete view
// on one row, with the destructive action last. The row uses plain SameLine flow, so the laid-out
// item rects give the left-to-right order. They are not clipped, so this does not prove a button
// fits the pane. Discard is the one button whose enabled state depends on the draft, so that gate
// is asserted as well.
//
// No button is clicked: Apply & Sync would start a backend sync, Delete view would open its confirm
// modal, and Discard would leave a toast over whichever test runs next.
//
// SEEDING SEAM: a fresh backend key (cfg.TrackerType flip) bootstraps a default workspace, so
// GetActiveView() is non-null and the editor pane renders (same seam as
// mobile_views_confirm_modal.test.cpp, here on the desktop dashboard).

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstring>
#include <string>

// g_ui — the shared UI-thread state bag the Views dashboard draws from.
extern UiDrawSession g_ui;

namespace {

const char* const kViewsWindow = "###SmatchetViewsDashboard";
const char* const kReseedSentinel = "smatchet_force_reseed_sentinel";
// SmatchetIconLeadingButton pins its id to the English label with "###", so the ref holds whether or
// not the icon font has loaded.
const char* const kApplyRef = "###Apply & Sync";
// Pixel slack for the same-row compare; ImGui rounds item positions.
const float kRowEpsilon = 1.0f;
const int kRowSettleAttempts = 120;

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// The editor pane is a child window, and items inside it hash from the child's own id, so the
// buttons resolve only with the child as the ref. Its name embeds the parent's full title, which
// carries the backend name, so it is matched on the stable "/ViewsEditor_" part.
ImGuiWindow* FindLiveEditorPane() {
    const ImGuiWindow* host = ImGui::FindWindowByName(kViewsWindow);
    if (host == nullptr || !host->Active) {
        return nullptr;
    }
    for (ImGuiWindow* child : host->DC.ChildWindows) {
        // SkipItems, not just Active: the dashboard ignores Begin's return, so a hidden tab's pane
        // still reports Active while submitting nothing.
        if (child->Active && !child->SkipItems && std::strstr(child->Name, "/ViewsEditor_") != nullptr) {
            return child;
        }
    }
    return nullptr;
}

// Hard-fails with the literal ref in the log, so a renamed label cannot degrade the test to a skip.
bool LocateItem(ImGuiTestContext* ctx, const char* label, ImGuiTestItemInfo* out) {
    *out = ctx->ItemInfo(label, ImGuiTestOpFlags_NoError);
    if (out->ID != 0) {
        return true;
    }
    ctx->LogError("Views editor header has no item \"%s\"", label);
    return false;
}

bool IsDisabled(const ImGuiTestItemInfo& item) { return (item.ItemFlags & ImGuiItemFlags_Disabled) != 0; }

struct HeaderRow {
    ImGuiTestItemInfo apply;
    ImGuiTestItemInfo discard;
    ImGuiTestItemInfo del;
};

bool SharesOneRow(const HeaderRow& row) {
    return ImFabs(row.apply.RectFull.Min.y - row.discard.RectFull.Min.y) <= kRowEpsilon &&
           ImFabs(row.discard.RectFull.Min.y - row.del.RectFull.Min.y) <= kRowEpsilon;
}

// The first ItemInfo for a ref yields a frame, and the content above the row can still be moving
// after the backend flip, so a pass can mix two layouts. A pass counts only if no frame elapsed
// during it and the three buttons report one row; a row that never forms is a failure, so this also
// asserts the same-row half of the layout.
bool ReadSettledRow(ImGuiTestContext* ctx, HeaderRow* row) {
    for (int i = 0; i < kRowSettleAttempts; ++i) {
        const int frameAtStart = ctx->FrameCount;
        if (!LocateItem(ctx, kApplyRef, &row->apply) || !LocateItem(ctx, "Discard", &row->discard) ||
            !LocateItem(ctx, "Delete view", &row->del)) {
            return false;
        }
        if (ctx->FrameCount == frameAtStart && SharesOneRow(*row)) {
            return true;
        }
        ctx->Yield();
    }
    ctx->LogError("Views editor header buttons never shared a row: y = %.1f, %.1f, %.1f", row->apply.RectFull.Min.y,
                  row->discard.RectFull.Min.y, row->del.RectFull.Min.y);
    return false;
}

// Left to right, with no overlap.
void CheckRowOrder(const HeaderRow& row) {
    IM_CHECK_NO_RET(row.apply.RectFull.Max.x <= row.discard.RectFull.Min.x);
    IM_CHECK_NO_RET(row.discard.RectFull.Max.x <= row.del.RectFull.Min.x);
}

// Restores the flags the test mutates. The draft is not copied back: viewDraftId stays on the
// sentinel, so the next draw reloads the draft and its text buffers from the active view, and a later
// test cannot inherit this test's dirty name. Queued side effects of the backend flip (the restarted
// sync, navHistory entries) are not undone, as with the other tests that use this seam.
struct ViewsDashboardStateGuard {
    UiMode uiMode = g_ui.cfg.UiMode;
    std::string tracker = g_ui.cfg.TrackerType;
    bool reachable = g_ui.cfg.BackendHasBeenReachable;
    bool showViews = g_ui.showViewsDashboard;
    ~ViewsDashboardStateGuard() {
        g_ui.requestViewsDashboardFocus = false;
        g_ui.viewDraftId = kReseedSentinel;
        g_ui.showViewsDashboard = showViews;
        g_ui.cfg.UiMode = uiMode;
        g_ui.cfg.TrackerType = tracker;
        g_ui.cfg.BackendHasBeenReachable = reachable;
    }
};

void CheckActionButtonOrder(ImGuiTestContext* ctx) {
    ViewsDashboardStateGuard guard;
    g_ui.cfg.UiMode = UiMode::Desktop;
    g_ui.cfg.TrackerType = "Plane";
    g_ui.cfg.BackendHasBeenReachable = true; // the first-launch gate on drawViewsDashboardWindow
    g_ui.showViewsDashboard = true;
    g_ui.viewDraftId = kReseedSentinel; // start from a clean draft, whatever an earlier test left

    // Re-arm focus each frame: the window docks as a tab, and an inactive tab submits no body.
    ImGuiWindow* pane = nullptr;
    const bool paneLive = YieldUntil(ctx, [&] {
        g_ui.requestViewsDashboardFocus = true;
        pane = FindLiveEditorPane();
        return pane != nullptr && g_ui.viewDraftId != kReseedSentinel;
    });
    IM_CHECK(paneLive);
    ctx->SetRef(pane);

    // A clean draft has nothing to discard; Apply & Sync stays available regardless.
    HeaderRow row;
    IM_CHECK(ReadSettledRow(ctx, &row));
    CheckRowOrder(row);
    IM_CHECK_NO_RET(IsDisabled(row.discard));
    IM_CHECK_NO_RET(!IsDisabled(row.apply));

    // An unsaved edit enables Discard, and the row keeps its order in that state too.
    g_ui.viewDraft.Name += "_dirty";
    ctx->Yield(2);
    IM_CHECK(ReadSettledRow(ctx, &row));
    CheckRowOrder(row);
    IM_CHECK_NO_RET(!IsDisabled(row.discard));
    IM_CHECK_NO_RET(!IsDisabled(row.apply));
}

void RegisterActionButtonOrder(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ViewsEditorHeader", "ActionButtons_ApplyThenDiscardThenDelete");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (SmatchetActiveUiTestAppController() == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        CheckActionButtonOrder(ctx);
        ctx->Yield(); // lands the backend flip-back inside this test, not the next one
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsEditorHeaderTests(ImGuiTestEngine* engine) { RegisterActionButtonOrder(engine); }

#endif // SMATCHET_BUILD_UI_TESTS
