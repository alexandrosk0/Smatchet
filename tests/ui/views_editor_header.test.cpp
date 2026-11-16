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
#include "Commands/Command.h"
#include "Commands/CommandRegistry.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiModeIds.h"
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <chrono>
#include <cstring>
#include <future>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

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
// asserts the same-row half of the layout. Focus is re-armed on every attempt: another docked window
// can take the tab at any time, and a pane that stops submitting must be waited on, not failed on.
bool ReadSettledRow(ImGuiTestContext* ctx, HeaderRow* row) {
    int paneMissing = 0;
    for (int i = 0; i < kRowSettleAttempts; ++i) {
        g_ui.requestViewsDashboardFocus = true;
        if (FindLiveEditorPane() == nullptr) {
            ++paneMissing;
            ctx->Yield();
            continue;
        }
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
    if (paneMissing == kRowSettleAttempts) {
        ctx->LogError("Views editor pane never became live (%d/%d attempts)", paneMissing, kRowSettleAttempts);
        return false;
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

// ---- View-switch keys (INPUT_UX_AUDIT.md #65) -------------------------------------------------------

// The view.* handlers hop to the UI thread and wait for it, so they run on a worker while the test keeps
// stepping frames.
smatchet::cmd::CommandResult RunCommand(ImGuiTestContext* ctx, AppController* app, const std::string& name,
                                        const nlohmann::json& args) {
    std::future<smatchet::cmd::CommandResult> pending = std::async(std::launch::async, [app, name, args] {
        smatchet::cmd::CommandContext cmdCtx;
        cmdCtx.ConfirmedDestructive = true; // view.delete, for the clean-up
        return app->Commands().Dispatch(name, args, cmdCtx);
    });
    while (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        ctx->Yield();
    }
    return pending.get();
}

std::string CurrentViewId(ImGuiTestContext* ctx, AppController* app) {
    const smatchet::cmd::CommandResult r = RunCommand(ctx, app, "view.current", nlohmann::json::object());
    return r.Ok && r.Data ? r.Data->value("id", std::string()) : std::string();
}

// The saved views in list order (the order view.next steps through).
std::vector<std::string> ViewIdsInOrder(ImGuiTestContext* ctx, AppController* app) {
    nlohmann::json args;
    args["limit"] = 500;
    const smatchet::cmd::CommandResult r = RunCommand(ctx, app, "view.list", args);
    std::vector<std::string> ids;
    if (r.Ok && r.Data && r.Data->contains("items")) {
        for (const auto& item : (*r.Data)["items"]) {
            ids.push_back(item.value("id", std::string()));
        }
    }
    return ids;
}

// The id `step` places from `id` in `ids`, wrapping.
std::string Neighbour(const std::vector<std::string>& ids, const std::string& id, int step) {
    const int n = static_cast<int>(ids.size());
    for (int i = 0; i < n; ++i) {
        if (ids[static_cast<std::size_t>(i)] == id) {
            return ids[static_cast<std::size_t>(((i + step) % n + n) % n)];
        }
    }
    return std::string();
}

bool WaitForCurrentView(ImGuiTestContext* ctx, AppController* app, const std::string& id) {
    for (int i = 0; i < 60; ++i) {
        if (CurrentViewId(ctx, app) == id) {
            return true;
        }
        ctx->Yield();
    }
    return false;
}

// Ctrl+PageDown / Ctrl+PageUp (view.next / view.previous) move the active view one step through the list
// and back, and Enter in the dashboard's views search opens the first view it lists.
void CheckViewSwitchKeys(ImGuiTestContext* ctx, AppController* app) {
    // Two views of its own, so there is always a next and a previous.
    std::vector<std::string> created;
    for (const char* name : {"zz input keys view A", "zz input keys view B"}) {
        nlohmann::json args;
        args["name"] = name;
        const smatchet::cmd::CommandResult r = RunCommand(ctx, app, "view.create", args);
        IM_CHECK_NO_RET(r.Ok);
        if (r.Ok && r.Data) {
            created.push_back(r.Data->value("id", std::string()));
        }
    }
    IM_CHECK_NO_RET(created.size() == 2);
    if (created.size() == 2) {
        ctx->Yield(3); // the deferred create lands
        const std::vector<std::string> ids = ViewIdsInOrder(ctx, app);
        IM_CHECK_NO_RET(ids.size() >= 2);
        const std::string start = CurrentViewId(ctx, app);
        IM_CHECK_NO_RET(!start.empty());
        ctx->SetInputMode(ImGuiInputSource_Keyboard);
        ImGui::ClearActiveID(); // a global chord: no text field holds the caret

        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_PageDown);
        const std::string next = Neighbour(ids, start, 1);
        const bool movedNext = WaitForCurrentView(ctx, app, next);
        if (!movedNext) {
            ctx->LogError("Ctrl+PageDown did not switch from '%s' to the next view '%s'", start.c_str(), next.c_str());
        }
        IM_CHECK_NO_RET(movedNext);
        ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_PageUp);
        const bool movedBack = WaitForCurrentView(ctx, app, start);
        if (!movedBack) {
            ctx->LogError("Ctrl+PageUp did not switch back to '%s'", start.c_str());
        }
        IM_CHECK_NO_RET(movedBack);

        // Enter in the views search box opens the first view it lists: with view A active, search B.
        nlohmann::json activateA;
        activateA["id"] = created[0];
        IM_CHECK_NO_RET(RunCommand(ctx, app, "view.activate", activateA).Ok);
        g_ui.cfg.BackendHasBeenReachable = true; // the first-launch gate on drawViewsDashboardWindow
        g_ui.showViewsDashboard = true;
        std::memset(g_ui.viewsSidebarSearchBuf, 0, sizeof(g_ui.viewsSidebarSearchBuf));
#if defined(_MSC_VER)
        strncpy_s(g_ui.viewsSidebarSearchBuf, sizeof(g_ui.viewsSidebarSearchBuf), "input keys view B", _TRUNCATE);
#else
        std::strncpy(g_ui.viewsSidebarSearchBuf, "input keys view B", sizeof(g_ui.viewsSidebarSearchBuf) - 1);
#endif
        ImGuiWindow* sidebar = nullptr;
        const bool sidebarLive = YieldUntil(ctx, [&] {
            g_ui.requestViewsDashboardFocus = true;
            const ImGuiWindow* host = ImGui::FindWindowByName(kViewsWindow);
            if (host == nullptr || !host->Active) {
                return false;
            }
            for (ImGuiWindow* child : host->DC.ChildWindows) {
                if (child->Active && !child->SkipItems && std::strstr(child->Name, "/ViewsSidebar_") != nullptr) {
                    sidebar = child;
                    return true;
                }
            }
            return false;
        });
        IM_CHECK_NO_RET(sidebarLive);
        if (sidebarLive) {
            ctx->SetRef(sidebar);
            ctx->ItemNavActivate("##ViewsSidebarSearch"); // the caret in the search box, without a mouse path
            ctx->KeyPress(ImGuiKey_Enter);
            const bool opened = WaitForCurrentView(ctx, app, created[1]);
            if (!opened) {
                ctx->LogError("Enter in the views search did not open the view it lists");
            }
            IM_CHECK_NO_RET(opened);
        }
        g_ui.viewsSidebarSearchBuf[0] = '\0';
        g_ui.requestViewsDashboardFocus = false;
    }
    for (const std::string& id : created) {
        nlohmann::json del;
        del["id"] = id;
        RunCommand(ctx, app, "view.delete", del);
    }
}

void RegisterViewSwitchKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ViewsEditorHeader", "ViewKeys_CtrlPageDownUpAndSidebarEnter");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        ViewsDashboardStateGuard guard;
        g_ui.cfg.UiMode = UiMode::Desktop;
        CheckViewSwitchKeys(ctx, app);
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsEditorHeaderTests(ImGuiTestEngine* engine) {
    RegisterActionButtonOrder(engine);
    RegisterViewSwitchKeys(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
