// window_expand.test.cpp — bucket-E coverage of the per-window Expand / Minimize
// control (Source/Core/src/Ui/SmatchetWindowExpand.cpp). Bucket-A
// (tests/Core/WindowExpand.test.cpp) already pins the pure state machine; what
// only a live ImGui frame loop can prove is the GEOMETRY half:
//
//   * expanding really pins the window over the viewport WORK AREA — the rect
//     below the main menu bar and above the status bar — undocked and drawn on
//     top of the dock host it covers.
//   * minimizing really puts it back in the dock node / floating rect it left.
//   * expanding a SECOND window hands the fullscreen slot over and drops the
//     first back where it was.
//   * closing a window while it is expanded self-heals, and reopening it lands
//     in its old slot rather than fullscreen.
//   * the main grid pane expands too, and grows the title bar it normally
//     suppresses so the minimize half has somewhere to live.
//
// This is the coverage that keeps the feature out of the visual-validation
// user-pause (AGENTS.md § Autonomous ship-loop default, exception 5).
//
// Driving the toggle: most cases drive SmatchetWindowExpand::ToggleWindow(),
// the same entry point both click handlers funnel into. Three cases click the
// real widget instead. Neither face is reachable by name-path: both are manual
// buttons placed at an absolute screen pos, and the docked one is submitted
// into the dock node's HOST window. Both are addressed by id instead —
// ImHashStr(label, 0, seed), seeded by the window id when floating and by the
// node id (PushOverrideID) when docked.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "SmatchetUiSession.h"
#include "SmatchetWindowExpand.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName, ImFabs, g.Windows display order
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

// g_ui — the shared bag of UI-thread visibility flags, flipped by
// ViewToggleCommands.cpp for the same windows the View menu opens.
extern UiDrawSession g_ui;

namespace {

const char* const kLogWindow = "Log";
const char* const kAuditWindow = "Backend Audit";
// The bootstrap grid pane keeps the legacy window name (SmatchetActiveProjectGridUi.cpp).
const char* const kMainPane = "Smatchet - Active Project";
// Must match SmatchetWindowExpand.cpp's kToggleLabel.
const char* const kToggleLabel = "##SmatchetWindowExpandToggle";

// The control's ImGuiID. "##" keeps the id RELATIVE, so it is the seed that
// separates one instance from the next: the window id on the floating path, the
// dock node id (PushOverrideID) on the docked one.
ImGuiID ToggleIdSeededBy(ImGuiID seed) { return ImHashStr(kToggleLabel, 0, seed); }

// Pixel slack for the fullscreen / restore rect compares. ImGui rounds window
// rects, and a re-docked window's size is the node's, not the replayed float.
const float kRectEpsilon = 1.5f;

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// Placement snapshot mirroring SmatchetWindowExpand's own CapturePlacement:
// DockNode (not DockId) is the live-docked test, because DockId lingers on a
// window the user floated.
struct Placement {
    bool live;
    unsigned int dockId;
    ImVec2 pos;
    ImVec2 size;

    Placement() : live(false), dockId(0u), pos(0.0f, 0.0f), size(0.0f, 0.0f) {}
};

Placement Capture(const char* title) {
    Placement out;
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    if (win == nullptr || !win->Active) {
        return out;
    }
    out.live = true;
    out.dockId = (win->DockNode != nullptr) ? static_cast<unsigned int>(win->DockNode->ID) : 0u;
    out.pos = win->Pos;
    out.size = win->SizeFull;
    return out;
}

// The expanded contract: undocked and flush with the viewport WORK area (which
// BeginViewportSideBar already shrank past the menu bar and the status bar).
bool CoversWorkArea(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    if (win == nullptr || !win->Active || win->DockNode != nullptr) {
        return false;
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    return ImFabs(win->Pos.x - vp->WorkPos.x) <= kRectEpsilon && ImFabs(win->Pos.y - vp->WorkPos.y) <= kRectEpsilon &&
           ImFabs(win->SizeFull.x - vp->WorkSize.x) <= kRectEpsilon &&
           ImFabs(win->SizeFull.y - vp->WorkSize.y) <= kRectEpsilon;
}

bool MatchesWorkArea(const ImVec2& pos, const ImVec2& size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    return ImFabs(pos.x - vp->WorkPos.x) <= kRectEpsilon && ImFabs(pos.y - vp->WorkPos.y) <= kRectEpsilon &&
           ImFabs(size.x - vp->WorkSize.x) <= kRectEpsilon && ImFabs(size.y - vp->WorkSize.y) <= kRectEpsilon;
}

// The footprint the USER sees, dock-state agnostic. CoversWorkArea requires
// UNDOCKED, so a botched restore that docks into a node ImGui invents from a
// dead id already clears it — while the window still covers the whole screen,
// which is exactly the "minimize does nothing" report. A docked window's outer
// rect is its NODE's (its own is inset by the tab bar), so compare that.
bool FootprintCoversWorkArea(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    if (win == nullptr || !win->Active) {
        return false;
    }
    if (MatchesWorkArea(win->Pos, win->SizeFull)) {
        return true;
    }
    // Its node too — a docked window is inset by the tab bar, so the node is the
    // rect the user sees. (An invented node can also sit un-laid-out at 0x0 while
    // the window itself still paints over everything, hence checking both.)
    return win->DockNode != nullptr && MatchesWorkArea(win->DockNode->Pos, win->DockNode->Size);
}

bool PlacementRestored(const char* title, const Placement& before) {
    const Placement now = Capture(title);
    if (!now.live || now.dockId != before.dockId) {
        return false;
    }
    if (before.dockId != 0u) {
        // Re-docked into the same node — the node owns pos/size from here.
        return true;
    }
    return ImFabs(now.pos.x - before.pos.x) <= kRectEpsilon && ImFabs(now.pos.y - before.pos.y) <= kRectEpsilon &&
           ImFabs(now.size.x - before.size.x) <= kRectEpsilon && ImFabs(now.size.y - before.size.y) <= kRectEpsilon;
}

// g.Windows is sorted back-to-front each frame, so a higher index draws on top.
int DisplayIndex(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    if (win == nullptr) {
        return -1;
    }
    ImGuiContext& g = *GImGui;
    for (int i = 0; i < g.Windows.Size; ++i) {
        if (g.Windows[i] == win) {
            return i;
        }
    }
    return -1;
}

// Opens a docked top-level window the way the View menu does: flip the
// visibility flag and re-arm the focus request every frame until the tab
// activates (an inactive dock tab's Begin() returns false, so the window never
// becomes live). Returns false if it never came up.
bool OpenWindow(ImGuiTestContext* ctx, bool UiDrawSession::* openFlag, bool UiDrawSession::* focusFlag,
                const char* title) {
    g_ui.*openFlag = true;
    return YieldUntil(ctx, [&] {
        g_ui.*focusFlag = true;
        return WindowIsLive(title);
    });
}

// Restores the visibility flags this suite flips, and — if a check failed
// mid-test and left a window pinned fullscreen — minimizes whatever is still
// expanded so the next test (and the dev's own session) starts clean.
struct WindowExpandSessionGuard {
    bool showLog;
    bool showAudit;

    WindowExpandSessionGuard() : showLog(g_ui.showLogWindow), showAudit(g_ui.showAuditTrail) {}

    ~WindowExpandSessionGuard() {
        const unsigned int expanded = g_ui.windowExpand.ExpandedId;
        if (expanded != 0u) {
            // Same id as the current expansion ⇒ ApplyToggle takes its minimize
            // branch, which arms the restore replay; the placement argument is
            // unused on that path.
            const SmatchetWindowExpand::WindowExpandSaved unused;
            SmatchetWindowExpand::detail::ApplyToggle(g_ui.windowExpand, expanded, unused);
        }
        g_ui.showLogWindow = showLog;
        g_ui.showAuditTrail = showAudit;
    }
};

const AppController* BootedAppOrSkip(ImGuiTestContext* ctx) {
    const AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo(
            "SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted under UiTestScenario");
    }
    return app;
}

// --- expand → fullscreen → minimize → back where it was -------------------
void RegisterExpandThenMinimize(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "ExpandCoversWorkAreaThenMinimizeRestores");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool opened = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(opened);
        if (!opened) {
            return;
        }
        const Placement before = Capture(kLogWindow);
        IM_CHECK_NO_RET(before.live);
        IM_CHECK_NO_RET(!CoversWorkArea(kLogWindow));

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        const bool expanded = YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); });
        IM_CHECK_NO_RET(expanded);
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId != 0u);

        // Fullscreen means ON TOP of the dock host it covers, not merely the
        // same size as it. Only asserted when the grid pane is up in this boot.
        const int logIndex = DisplayIndex(kLogWindow);
        const int mainIndex = DisplayIndex(kMainPane);
        if (logIndex >= 0 && mainIndex >= 0) {
            IM_CHECK_NO_RET(logIndex > mainIndex);
        }

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        const bool restored = YieldUntil(ctx, [&] { return PlacementRestored(kLogWindow, before); });
        IM_CHECK_NO_RET(restored);
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);
        // Consume-once: the replay must not keep re-pinning the window.
        IM_CHECK_NO_RET(g_ui.windowExpand.RestorePending.empty());
        IM_CHECK_NO_RET(g_ui.windowExpand.Saved.empty());
    };
}

// --- expanding a second window hands the slot over ------------------------
void RegisterSecondExpandMinimizesFirst(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "SecondExpandMinimizesTheFirst");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool logUp = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        const bool auditUp =
            OpenWindow(ctx, &UiDrawSession::showAuditTrail, &UiDrawSession::requestAuditTrailFocus, kAuditWindow);
        IM_CHECK_NO_RET(logUp);
        IM_CHECK_NO_RET(auditUp);
        if (!logUp || !auditUp) {
            return;
        }
        // Re-arm Log's tab: opening Audit may have taken the shared dock slot.
        const bool logBack =
            OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(logBack);
        const Placement logBefore = Capture(kLogWindow);

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); }));

        // Audit is docked behind the fullscreen Log; re-arm its focus so its tab
        // is the selected one before expanding it.
        const bool auditBack =
            OpenWindow(ctx, &UiDrawSession::showAuditTrail, &UiDrawSession::requestAuditTrailFocus, kAuditWindow);
        IM_CHECK_NO_RET(auditBack);
        SmatchetWindowExpand::ToggleWindow(g_ui, kAuditWindow);
        const bool swapped =
            YieldUntil(ctx, [&] { return CoversWorkArea(kAuditWindow) && PlacementRestored(kLogWindow, logBefore); });
        IM_CHECK_NO_RET(swapped);
        IM_CHECK_NO_RET(!CoversWorkArea(kLogWindow));

        SmatchetWindowExpand::ToggleWindow(g_ui, kAuditWindow);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !CoversWorkArea(kAuditWindow); }));
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);
    };
}

// --- closed while expanded → self-heal, reopen lands in the old slot -------
void RegisterClosedWhileExpandedSelfHeals(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "ClosedWhileExpandedSelfHeals");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool opened = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(opened);
        if (!opened) {
            return;
        }
        const Placement before = Capture(kLogWindow);

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); }));

        // Close it while expanded (the tab X / View menu path). The window stops
        // submitting, so nothing may stay pinned to it.
        g_ui.showLogWindow = false;
        const bool healed = YieldUntil(ctx, [&] { return g_ui.windowExpand.ExpandedId == 0u; }, 30);
        IM_CHECK_NO_RET(healed);

        // Reopening must land in the pre-expand slot, not fullscreen.
        const bool reopened =
            OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(reopened);
        const bool back = YieldUntil(ctx, [&] { return PlacementRestored(kLogWindow, before); });
        IM_CHECK_NO_RET(back);
        IM_CHECK_NO_RET(!CoversWorkArea(kLogWindow));
    };
}

// --- the CONTROL itself round-trips (not just the state machine) ----------
// The other cases drive SmatchetWindowExpand::ToggleWindow directly, which
// proves the transitions but not the widget. This one clicks the real title-bar
// button on the expanded (undocked) window — the path that has to work for the
// minimize half of the feature to exist at all.
void RegisterTitleBarToggleClickMinimizes(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "TitleBarToggleClickMinimizes");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool opened = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(opened);
        if (!opened) {
            return;
        }
        const Placement before = Capture(kLogWindow);

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); }));

        // Expanded ⇒ undocked ⇒ real title bar ⇒ the manual button is submitted
        // into the window itself, so its id is seeded by the window id.
        const ImGuiWindow* logWin = ImGui::FindWindowByName(kLogWindow);
        IM_CHECK_NO_RET(logWin != nullptr);
        if (logWin == nullptr) {
            return;
        }
        ctx->ItemClick(ImGuiTestRef(ToggleIdSeededBy(logWin->ID)));
        const bool minimized = YieldUntil(ctx, [&] { return !CoversWorkArea(kLogWindow); });
        IM_CHECK_NO_RET(minimized);
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return PlacementRestored(kLogWindow, before); }));
    };
}

// --- the DOCKED control round-trips (expand from the tab bar, then minimize) -
// The docked half of the widget: a manual button right-glued to the tab bar's
// far edge, submitted into the node's HOST window, so no ref built from the
// docked window's name reaches it — it is addressed by node-seeded id.
void RegisterTabBarToggleClickRoundTrips(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "TabBarToggleClickExpandsThenMinimizes");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool opened = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(opened);
        if (!opened) {
            return;
        }
        const ImGuiWindow* win = ImGui::FindWindowByName(kLogWindow);
        if (win == nullptr || win->DockNode == nullptr) {
            ctx->LogInfo("SKIP: '%s' is floating in this boot — no tab bar to click", kLogWindow);
            return;
        }
        const Placement before = Capture(kLogWindow);
        IM_CHECK_NO_RET(before.dockId != 0u);

        const ImGuiID toggleId = ToggleIdSeededBy(win->DockNode->ID);
        const ImGuiTestItemInfo toggleInfo = ctx->ItemInfo(ImGuiTestRef(toggleId));
        IM_CHECK_NO_RET(toggleInfo.ID == toggleId);
        if (toggleInfo.ID != toggleId) {
            return;
        }
        // Glued to the RIGHT edge of the tab bar, i.e. immediately left of the
        // node's close X (DockNodeCalcTabBarLayout already shrank BarRect past it).
        // A trailing TabItemButton packs against the last TAB instead, which is the
        // left-aligned placement this replaced.
        IM_CHECK_NO_RET(ImFabs(toggleInfo.RectFull.Max.x - win->DockNode->TabBar->BarRect.Max.x) <= kRectEpsilon);

        ctx->ItemClick(ImGuiTestRef(toggleId));
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); }));

        // Expanded ⇒ undocked ⇒ real title bar: the minimize half is the manual
        // button in the window itself, so the seed flips to the window id.
        ctx->ItemClick(ImGuiTestRef(ToggleIdSeededBy(win->ID)));
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !FootprintCoversWorkArea(kLogWindow); }));
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return PlacementRestored(kLogWindow, before); }));
    };
}

// --- minimizing when the node you left no longer exists --------------------
// Expanding UNDOCKS the window, and undocking the last tab out of a node
// destroys that node. Replaying the dead id then docks nowhere and leaves the
// window sitting at its fullscreen rect — the "minimize does nothing" report.
// Staged directly (a real sole-tab node is not deterministic across boots) by
// pointing the saved slot at an id no node owns.
// An id no real dock node owns — the collapsed node's stand-in.
const ImGuiID kStaleNodeId = 0x5A5A5A5Au;

void RegisterStaleDockNodeRestoreFallsBack(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "StaleDockNodeRestoreFallsBackToFloating");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        WindowExpandSessionGuard guard;

        const bool opened = OpenWindow(ctx, &UiDrawSession::showLogWindow, &UiDrawSession::requestLogFocus, kLogWindow);
        IM_CHECK_NO_RET(opened);
        if (!opened) {
            return;
        }
        const ImGuiWindow* win = ImGui::FindWindowByName(kLogWindow);
        IM_CHECK_NO_RET(win != nullptr);
        if (win == nullptr) {
            return;
        }
        const unsigned int id = static_cast<unsigned int>(win->ID);

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kLogWindow); }));

        // Kill the saved node: an id no ImGuiDockNode owns, standing in for the
        // node that the undock collapsed.
        std::unordered_map<unsigned int, SmatchetWindowExpand::WindowExpandSaved>::iterator saved =
            g_ui.windowExpand.Saved.find(id);
        IM_CHECK_NO_RET(saved != g_ui.windowExpand.Saved.end());
        if (saved == g_ui.windowExpand.Saved.end()) {
            return;
        }
        saved->second.DockId = static_cast<unsigned int>(kStaleNodeId);
        IM_CHECK_NO_RET(ImGui::DockBuilderGetNode(kStaleNodeId) == nullptr);

        SmatchetWindowExpand::ToggleWindow(g_ui, kLogWindow);
        // Contract: it stops covering the screen. Where it lands (floating rect, or
        // re-docked) is the layout's call. Footprint rather than CoversWorkArea: a
        // window docked into a node ImGui invents from the dead id clears
        // CoversWorkArea (which wants undocked) while still filling the viewport.
        // This guards the branch, not a specific rescue path — SmatchetUI's
        // repairTopLevelWindow also recovers the window, just tens of frames later.
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !FootprintCoversWorkArea(kLogWindow); }));
        IM_CHECK_NO_RET(!CoversWorkArea(kLogWindow));
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);

        // Cleanup: on a REGRESSED build the window ends up docked inside the node
        // ImGui invents from the staged id, and that node is then written to
        // imgui.ini — poisoning the next boot (which is a real dev's layout, since
        // the bucket-E spawn shares the user data dir). Drop it either way.
        ImGui::DockBuilderRemoveNode(kStaleNodeId);
        ctx->Yield();
        IM_CHECK_NO_RET(ImGui::DockBuilderGetNode(kStaleNodeId) == nullptr);
    };
}

// --- the main grid pane expands too, and grows a title bar while it does ----
// It is the ONE window that normally runs with ImGuiWindowFlags_NoTitleBar. Undocked
// by the expansion, that would leave the minimize half nowhere to draw and wedge the
// pane fullscreen — so the flag has to lift for the expanded frames.
void RegisterMainPaneExpandsWithTitleBar(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "WindowExpand", "MainGridPaneExpandsAndMinimizes");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        if (BootedAppOrSkip(ctx) == nullptr) {
            return;
        }
        if (!WindowIsLive(kMainPane)) {
            ctx->LogInfo("SKIP: '%s' is not live in this boot — nothing to assert", kMainPane);
            return;
        }
        WindowExpandSessionGuard guard;

        const Placement before = Capture(kMainPane);
        SmatchetWindowExpand::ToggleWindow(g_ui, kMainPane);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return CoversWorkArea(kMainPane); }));

        const ImGuiWindow* mainWin = ImGui::FindWindowByName(kMainPane);
        IM_CHECK_NO_RET(mainWin != nullptr);
        if (mainWin == nullptr) {
            return;
        }
        IM_CHECK_NO_RET((mainWin->Flags & ImGuiWindowFlags_NoTitleBar) == 0);

        // Minimize through the real control, which only exists because of that flag flip.
        ctx->ItemClick(ImGuiTestRef(ToggleIdSeededBy(mainWin->ID)));
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !FootprintCoversWorkArea(kMainPane); }));
        IM_CHECK_NO_RET(g_ui.windowExpand.ExpandedId == 0u);
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return PlacementRestored(kMainPane, before); }));
        // No post-restore Flags check: BeginDocked rewrites NoTitleBar off the NODE's
        // shape (set when the node has no tab bar, CLEARED when it does — imgui.cpp
        // "we need a title bar height ... but it won't be displayed"), so what this
        // code passes is unobservable once docked. The flag only has to hold for the
        // FLOATING frames, which is the check above — and PlacementRestored already
        // pins the window back into the exact node it left.
    };
}

} // namespace

extern "C" void SmatchetRegisterWindowExpandTests(ImGuiTestEngine* engine) {
    RegisterExpandThenMinimize(engine);
    RegisterSecondExpandMinimizesFirst(engine);
    RegisterClosedWhileExpandedSelfHeals(engine);
    RegisterTitleBarToggleClickMinimizes(engine);
    RegisterTabBarToggleClickRoundTrips(engine);
    RegisterStaleDockNodeRestoreFallsBack(engine);
    RegisterMainPaneExpandsWithTitleBar(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
