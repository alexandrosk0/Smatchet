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
// and back, and ask before switching when the Views editor holds unapplied edits.
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

        // Unapplied edits in the Views editor are not thrown away by the key: it asks first (the same
        // "Discard changes?" a sidebar click raises), keeps the draft and stays on the active view.
        g_ui.cfg.BackendHasBeenReachable = true; // the first-launch gate on drawViewsDashboardWindow
        g_ui.showViewsDashboard = true;
        ImGuiWindow* editor = nullptr;
        const bool draftLive = YieldUntil(ctx, [&] {
            g_ui.requestViewsDashboardFocus = true;
            editor = FindLiveEditorPane();
            return editor != nullptr && g_ui.viewDraftId == start;
        });
        IM_CHECK_NO_RET(draftLive);
        if (draftLive) {
            g_ui.viewDraft.Name += "_unapplied";
            ctx->Yield(2);
            ImGui::ClearActiveID();
            ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_PageDown);
            // The dashboard turns the show flag into the open popup on the next draw, so the pending id,
            // which stays until the question is answered, is the lasting sign that it was asked.
            const bool asked = YieldUntil(ctx, [] { return !g_ui.viewsPendingActivateId.empty(); }, 60);
            if (!asked) {
                ctx->LogError("Ctrl+PageDown with unapplied editor edits did not ask before switching");
            }
            IM_CHECK_NO_RET(asked);
            IM_CHECK_NO_RET(CurrentViewId(ctx, app) == start);                            // nothing was switched
            IM_CHECK_NO_RET(g_ui.viewDraft.Name.find("_unapplied") != std::string::npos); // the draft is kept
            // Esc answers the question as Cancel does: it clears the pending switch and keeps the view.
            ctx->KeyPress(ImGuiKey_Escape);
            const bool answered = YieldUntil(ctx, [] { return g_ui.viewsPendingActivateId.empty(); }, 60);
            IM_CHECK_NO_RET(answered);
            IM_CHECK_NO_RET(CurrentViewId(ctx, app) == start);
            g_ui.viewDraftId = kReseedSentinel; // the next case starts from a clean draft
            ctx->Yield(2);
        }

        g_ui.requestViewsDashboardFocus = false;
    }
    for (const std::string& id : created) {
        nlohmann::json del;
        del["id"] = id;
        RunCommand(ctx, app, "view.delete", del);
    }
}

void RegisterViewSwitchKeys(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ViewsEditorHeader", "ViewKeys_CtrlPageDownUpAndDirtyDraftAsks");
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

// ---- Views sidebar search and the title rename (INPUT_UX_AUDIT.md #5, #65; one-matcher plan) ------------

// The sidebar's list child, live this frame. Items in it hash from the sidebar child's own id.
ImGuiWindow* FindLiveSidebarChild(const char* namePart) {
    const ImGuiWindow* host = ImGui::FindWindowByName(kViewsWindow);
    if (host == nullptr || !host->Active) {
        return nullptr;
    }
    std::vector<const ImGuiWindow*> stack(1, host);
    while (!stack.empty()) {
        const ImGuiWindow* w = stack.back();
        stack.pop_back();
        for (ImGuiWindow* child : w->DC.ChildWindows) {
            if (child->Active && !child->SkipItems && std::strstr(child->Name, namePart) != nullptr) {
                return child;
            }
            stack.push_back(child);
        }
    }
    return nullptr;
}

void SetSidebarSearch(const char* text) {
    std::memset(g_ui.viewsSidebarSearchBuf, 0, sizeof(g_ui.viewsSidebarSearchBuf));
#if defined(_MSC_VER)
    strncpy_s(g_ui.viewsSidebarSearchBuf, sizeof(g_ui.viewsSidebarSearchBuf), text, _TRUNCATE);
#else
    std::strncpy(g_ui.viewsSidebarSearchBuf, text, sizeof(g_ui.viewsSidebarSearchBuf) - 1);
#endif
}

// Vertices the last frame drew in `w` in the theme's match-mark colour: SmatchetMatchHighlight's marks.
int MatchMarkVertices(const ImGuiWindow* w) {
    if (w == nullptr || w->DrawList == nullptr) {
        return 0;
    }
    const ImU32 mark = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
    int n = 0;
    for (const ImDrawVert& v : w->DrawList->VtxBuffer) {
        n += v.col == mark ? 1 : 0;
    }
    return n;
}

// Opens the dashboard on `viewId` with a clean draft and returns its live editor pane (nullptr on timeout).
ImGuiWindow* OpenDashboardOn(ImGuiTestContext* ctx, AppController* app, const std::string& viewId) {
    nlohmann::json activate;
    activate["id"] = viewId;
    if (!RunCommand(ctx, app, "view.activate", activate).Ok) {
        ctx->LogError("view.activate '%s' failed", viewId.c_str());
        return nullptr;
    }
    g_ui.cfg.BackendHasBeenReachable = true; // the first-launch gate on drawViewsDashboardWindow
    g_ui.showViewsDashboard = true;
    g_ui.viewsTitleEditing = false;
    g_ui.viewDraftId = kReseedSentinel;
    ImGuiWindow* editor = nullptr;
    const bool live = YieldUntil(ctx, [&] {
        g_ui.requestViewsDashboardFocus = true;
        editor = FindLiveEditorPane();
        return editor != nullptr && g_ui.viewDraftId == viewId;
    });
    if (!live) {
        ctx->LogError("Views dashboard never showed view '%s'", viewId.c_str());
        return nullptr;
    }
    return editor;
}

// The sidebar child, live, with the dashboard kept in front (nullptr on timeout).
ImGuiWindow* WaitForSidebar(ImGuiTestContext* ctx) {
    ImGuiWindow* sidebar = nullptr;
    YieldUntil(ctx, [&] {
        g_ui.requestViewsDashboardFocus = true;
        sidebar = FindLiveSidebarChild("/ViewsSidebar_");
        return sidebar != nullptr;
    });
    return sidebar;
}

// Waits until the views list is live and its match marks satisfy `wantMarks`; reports whether it got there.
template <typename Want> bool WaitForListMarks(ImGuiTestContext* ctx, Want wantMarks) {
    return YieldUntil(
        ctx,
        [&] {
            g_ui.requestViewsDashboardFocus = true;
            const ImGuiWindow* list = FindLiveSidebarChild("/ViewsSidebarList_");
            return list != nullptr && wantMarks(MatchMarkVertices(list));
        },
        60);
}

// Types `query` into the search (with the caret in it), presses `chord`, and returns the current view after.
std::string PressInSidebarSearch(ImGuiTestContext* ctx, AppController* app, ImGuiWindow* sidebar, const char* query,
                                 ImGuiKeyChord chord) {
    SetSidebarSearch(query);
    ctx->Yield(2);
    ctx->SetRef(sidebar);
    ctx->ItemNavActivate("##ViewsSidebarSearch"); // the caret in the search box, without a mouse path
    ctx->KeyPress(chord);
    ctx->Yield(5);
    ImGui::ClearActiveID();
    return CurrentViewId(ctx, app);
}

// 1. A bare Enter in the views search opens the first view it lists: with "alpha" active, search "kilo", a word
// only the other view holds, so the list has one row whatever the scorer's tie-breaks. The listed row marks the
// letters that matched. Ctrl+Enter (the dashboard's Apply & Sync key) and Enter on a blank search open nothing.
void CheckSidebarEnterAndMarks(ImGuiTestContext* ctx, AppController* app, const std::vector<std::string>& created) {
    if (OpenDashboardOn(ctx, app, created[0]) == nullptr) {
        return;
    }
    ImGuiWindow* sidebar = WaitForSidebar(ctx);
    IM_CHECK_NO_RET(sidebar != nullptr);
    if (sidebar == nullptr) {
        return;
    }
    SetSidebarSearch("kilo");
    const bool marked = WaitForListMarks(ctx, [](int n) { return n > 0; });
    if (!marked) {
        ctx->LogError("The views list drew no match mark for the search 'kilo'");
    }
    IM_CHECK_NO_RET(marked);

    const std::string afterCtrlEnter = PressInSidebarSearch(ctx, app, sidebar, "kilo", ImGuiMod_Ctrl | ImGuiKey_Enter);
    IM_CHECK_STR_EQ_NO_RET(afterCtrlEnter.c_str(), created[0].c_str());
    const std::string afterBlankEnter = PressInSidebarSearch(ctx, app, sidebar, "   ", ImGuiKey_Enter);
    IM_CHECK_STR_EQ_NO_RET(afterBlankEnter.c_str(), created[0].c_str());

    PressInSidebarSearch(ctx, app, sidebar, "kilo", ImGuiKey_Enter);
    const bool opened = WaitForCurrentView(ctx, app, created[1]);
    if (!opened) {
        ctx->LogError("Enter in the views search did not open the view it lists");
    }
    IM_CHECK_NO_RET(opened);

    SetSidebarSearch("");
    IM_CHECK_NO_RET(WaitForListMarks(ctx, [](int n) { return n == 0; }));
}

// Types `text` into the title field once Rename is on and its caret has settled, then presses `key`; reports
// whether the edit ended.
bool TypeIntoTitle(ImGuiTestContext* ctx, const char* text, ImGuiKey key) {
    if (!YieldUntil(ctx, [] { return g_ui.viewsTitleEditing; }, 30)) {
        ctx->LogError("Rename did not open the title field");
        return false;
    }
    ctx->Yield(2); // the field takes the caret on the frame it first draws
    ctx->KeyChars(text);
    ctx->KeyPress(key);
    return YieldUntil(ctx, [] { return !g_ui.viewsTitleEditing; }, 30);
}

// 2. Rename puts the caret in the title with the name selected: typing replaces it, Enter keeps it, Esc puts
// the name back.
void CheckTitleRename(ImGuiTestContext* ctx, AppController* app, const std::vector<std::string>& created) {
    ImGuiWindow* editor = OpenDashboardOn(ctx, app, created[0]);
    if (editor == nullptr) {
        return;
    }
    ctx->SetRef(editor);
    ctx->ItemClick("Rename");
    IM_CHECK_NO_RET(TypeIntoTitle(ctx, "renamed by keys", ImGuiKey_Enter));
    if (g_ui.viewDraft.Name != "renamed by keys") {
        ctx->LogError("Rename did not select the name for typing: draft name is '%s'", g_ui.viewDraft.Name.c_str());
    }
    IM_CHECK_NO_RET(g_ui.viewDraft.Name == "renamed by keys");

    const std::string before = g_ui.viewDraft.Name;
    ctx->ItemClick("Rename");
    IM_CHECK_NO_RET(TypeIntoTitle(ctx, "x", ImGuiKey_Escape));
    IM_CHECK_STR_EQ_NO_RET(g_ui.viewDraft.Name.c_str(), before.c_str());
    g_ui.viewDraftId = kReseedSentinel; // drop the unapplied rename
    ctx->Yield(2);
}

// 3. Rename... in a row's right-click menu takes the caret too, with no window focused beforehand.
void CheckMenuRename(ImGuiTestContext* ctx, AppController* app, const std::vector<std::string>& created) {
    if (OpenDashboardOn(ctx, app, created[0]) == nullptr) {
        return;
    }
    ImGuiWindow* list = nullptr;
    YieldUntil(ctx, [&] {
        g_ui.requestViewsDashboardFocus = true;
        list = FindLiveSidebarChild("/ViewsSidebarList_");
        return list != nullptr;
    });
    IM_CHECK_NO_RET(list != nullptr);
    if (list == nullptr) {
        return;
    }
    g_ui.requestViewsDashboardFocus = false;
    ImGui::FocusWindow(nullptr);
    ctx->Yield(2);
    ctx->SetRef(list);
    // NoFocusWindow: a right-click does not focus the window it lands in, and the engine must not either.
    ctx->ItemClick((created[0] + "/##row").c_str(), ImGuiMouseButton_Right, ImGuiTestOpFlags_NoFocusWindow);
    ctx->ItemClick("//$FOCUSED/Rename...");
    IM_CHECK_NO_RET(TypeIntoTitle(ctx, "renamed from the menu", ImGuiKey_Enter));
    if (g_ui.viewDraft.Name != "renamed from the menu") {
        ctx->LogError("Rename... from the row menu left the caret elsewhere: draft name is '%s'",
                      g_ui.viewDraft.Name.c_str());
    }
    IM_CHECK_NO_RET(g_ui.viewDraft.Name == "renamed from the menu");
    g_ui.viewDraftId = kReseedSentinel; // drop the unapplied rename
    ctx->Yield(2);
}

void CheckSidebarSearchAndRename(ImGuiTestContext* ctx, AppController* app) {
    std::vector<std::string> created;
    for (const char* name : {"zz sidebar keys alpha", "zz sidebar keys kilo"}) {
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
        ctx->SetInputMode(ImGuiInputSource_Keyboard);
        CheckSidebarEnterAndMarks(ctx, app, created);
        SetSidebarSearch("");
        CheckTitleRename(ctx, app, created);
        ctx->SetInputMode(ImGuiInputSource_Mouse);
        CheckMenuRename(ctx, app, created);
        g_ui.requestViewsDashboardFocus = false;
    }
    for (const std::string& id : created) {
        nlohmann::json del;
        del["id"] = id;
        RunCommand(ctx, app, "view.delete", del);
    }
}

void RegisterSidebarSearchAndRename(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "ViewsEditorHeader", "Sidebar_EnterOpensFirstMarksMatchRenameSelects");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = SmatchetActiveUiTestAppController();
        if (app == nullptr) {
            ctx->LogInfo("SKIP: app not booted");
            return;
        }
        ViewsDashboardStateGuard guard;
        g_ui.cfg.UiMode = UiMode::Desktop;
        CheckSidebarSearchAndRename(ctx, app);
        g_ui.viewsTitleEditing = false;
        ctx->Yield();
    };
}

} // namespace

extern "C" void SmatchetRegisterViewsEditorHeaderTests(ImGuiTestEngine* engine) {
    RegisterActionButtonOrder(engine);
    RegisterViewSwitchKeys(engine);
    RegisterSidebarSearchAndRename(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
