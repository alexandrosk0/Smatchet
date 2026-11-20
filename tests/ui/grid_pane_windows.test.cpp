// grid_pane_windows.test.cpp — bucket-E smoke for multi-grid-tabs Slice 2
// (docs/plans/multi-grid-tabs.md plan items 14-16, ADR-0018).
//
// Boot-open-assert coverage for the dockable grid-pane windows:
//   1. new   — click the "+" pane button in the primary grid window; assert a
//              second GridPane materialises and its window goes live.
//   2. split — DockInto the new pane window to the RIGHT of the primary window;
//              assert BOTH windows are Active the same frame (side-by-side, two
//              visible re-entrant grid renders per frame).
//   3. focus — the new pane window holds focus; assert focusedPaneId follows
//              (the Slice-2 single-live-context hand-over trigger).
//   4. close — WindowClose the new pane; assert the pane is removed and focus
//              falls back to the surviving pane (min-1 invariant).
//   5. caret — with a second Jira site configured, the "+" split button shows its
//              backend-picker caret and lists one row per Jira site (#2444).
//   6. site  — clicking the other site's row switches the active Jira site and
//              opens a pane that does not inherit the source pane's rows.
//
// Follows the funcsize_grid_render.test.cpp fixture-coupled recipe: fixture-gate,
// boot-check, sync, data-check, focus latch, bare-ref widget probes. Registered
// behind the spawn-warmup gate like every other bucket-E test.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "Config/ConfigManager.h"
#include "Config/JiraBackendInstancesPure.h"
#include "SmatchetUiSession.h"
#include "Ui/PaneBackendTargetsPure.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — docked-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

extern UiDrawSession g_ui;

namespace {

template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// Like YieldUntil, but pred() must hold on holdFrames consecutive frames — for focus state, where a
// one-frame sighting can still be reverted by the host's two-frame focus debounce.
template <typename Pred>
bool YieldUntilHeld(ImGuiTestContext* ctx, Pred pred, int holdFrames = 5, int maxFrames = 300) {
    int held = 0;
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        held = pred() ? held + 1 : 0;
        if (held >= holdFrames) {
            return true;
        }
    }
    return false;
}

bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

bool FixtureEnvSet() {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const bool set = std::getenv("SMATCHET_TEST_JIRA_BACKEND_FIXTURE") != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

// Primary grid-pane window title (the app's active-project window).
const char* kPrimary = "Smatchet - Active Project";

// Shared boot preamble for the multi-grid tests: fixture gate + backend sync + primary pane
// live/focused + panes-loaded check. Returns the booted app, or nullptr after a logged skip/check
// (the caller returns on nullptr).
AppController* BootSyncPrimaryLive(ImGuiTestContext* ctx) {
    if (!FixtureEnvSet()) {
        ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — fixture backend absent");
        return nullptr;
    }
    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return nullptr;
    }
    // Suppress the first-run Whisper setup banner (##WhisperSetupBanner), which otherwise floats over
    // the grid header row and swallows the "+##PaneAdd" click on a clean profile. Makes the panes
    // tests self-contained instead of relying on the driver pre-seeding whisper_setup_completed.
    g_ui.cfg.WhisperSetupCompleted = true;
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsSyncPendingOrActive(); });
    IM_CHECK_NO_RET(syncDone);
    IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

    ctx->SetRef(kPrimary);
    const bool primaryLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kPrimary);
    });
    IM_CHECK_NO_RET(primaryLive);
    if (!primaryLive) {
        return nullptr;
    }
    IM_CHECK_NO_RET(g_ui.gridPanesLoaded);
    IM_CHECK_NO_RET(g_ui.gridPanes.size() == 1); // IM_CHECK_EQ_NO_RET returns void — invalid in this helper
    return app;
}

} // namespace

// ---------------------------------------------------------------------------
// GridPanes / PaneNewSplitFocusClose
// ---------------------------------------------------------------------------
static void RegisterGridPaneNewSplitFocusClose(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridPanes", "PaneNewSplitFocusClose");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }

        // 1. NEW — click the "+" pane button (submitted right after the window Begin). The
        // ##WhisperSetupBanner that would otherwise float over this header row is suppressed in
        // BootSyncPrimaryLive (cfg.WhisperSetupCompleted), so the click lands on any profile.
        ctx->ItemClick("+##PaneAdd");
        const bool paneAdded = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 2; });
        IM_CHECK_NO_RET(paneAdded);
        if (!paneAdded) {
            return;
        }

        // The new pane's window name is cached during its first draw.
        const bool nameReady = YieldUntil(ctx, [&] { return !g_ui.gridPanes[1].cachedWindowName.empty(); });
        IM_CHECK_NO_RET(nameReady);
        if (!nameReady) {
            return;
        }
        const std::string paneTwoName = g_ui.gridPanes[1].cachedWindowName; // "<title>###GridPane:pane-2"
        const std::string paneTwoId = g_ui.gridPanes[1].id;
        const bool paneTwoLive = YieldUntil(ctx, [&] { return WindowIsLive(paneTwoName.c_str()); });
        IM_CHECK_NO_RET(paneTwoLive);

        // 3. FOCUS — the freshly opened pane window takes ImGui focus; the host must
        // flip focusedPaneId (the Slice-2 live-context hand-over trigger).
        const bool focusFollowed = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == paneTwoId; });
        IM_CHECK_NO_RET(focusFollowed);

        // 2. SPLIT — drag pane 2 to the RIGHT of the primary window; both grids must
        // then be visible (Active) in the same frame: two re-entrant renders/frame.
        bool splitDone = false;
        if (paneTwoLive) {
            // Absolute "//" refs — the test is under SetRef(kPrimary), and DockInto
            // resolves bare names relative to that (which dangles for window names).
            const std::string paneTwoAbsRef = std::string("//") + paneTwoName;
            const std::string primaryAbsRef = std::string("//") + kPrimary;
            ctx->DockInto(paneTwoAbsRef.c_str(), primaryAbsRef.c_str(), ImGuiDir_Right);
            const bool bothVisible =
                YieldUntil(ctx, [&] { return WindowIsLive(kPrimary) && WindowIsLive(paneTwoName.c_str()); });
            IM_CHECK_NO_RET(bothVisible);
            splitDone = bothVisible;
        }

        // 3b. SORT-CLICK INTO A NOT-YET-FOCUSED PANE — a
        // header sort click lands on the SAME frame ImGui moves window focus; the
        // view mirror must run off the live focus report, not last frame's
        // pane.focused. Focus the PRIMARY pane first, then click pane 2's "ID"
        // header and require the session view-sort dirty flags to flip (the mirror
        // marks dirty only when the view's sort rules actually changed).
        if (splitDone) {
            // Programmatic window focus — the requestActiveProjectFocus latch only
            // applies to the session-FOCUSED pane, which is pane 2 right now.
            ctx->WindowFocus("//Smatchet - Active Project");
            const bool primaryRefocused = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == "main"; });
            IM_CHECK_NO_RET(primaryRefocused);
            if (primaryRefocused) {
                // column-view-save-simplification: sort autosaves straight onto the live view
                // (no more viewSortDirty/viewsDirty flags) — the mirror having run is now
                // observed via the debounced layout-autosave arm going from disarmed
                // (time_point::max()) to a concrete due time.
                g_ui.viewLayoutSaveAt = std::chrono::steady_clock::time_point::max();
                const std::string paneTwoTableRef = std::string("//") + paneTwoName + "/TicketGrid";
                ctx->TableClickHeader(paneTwoTableRef.c_str(), "ID");
                const bool sortMirrored = YieldUntil(
                    ctx, [&] { return g_ui.viewLayoutSaveAt != std::chrono::steady_clock::time_point::max(); });
                IM_CHECK_NO_RET(sortMirrored); // HIGH-3: mirror survives the focus-click frame
                const bool focusFollowedClick = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == paneTwoId; });
                IM_CHECK_NO_RET(focusFollowedClick);
            }
        }

        // Record the survivor's saved view BEFORE the close so we can assert the
        // focused-close hand-over never rebinds it.
        const std::string survivorViewIdBeforeClose = g_ui.gridPanes.front().viewId;

        // 4. CLOSE — flip the pane's open flag exactly as the tab X writes it (the X
        // is stock ImGui; ImGuiTestContext::WindowClose can't reach a docked tab's
        // close button by window ref). The host sweep must remove the pane, keep the
        // min-1 invariant, and fall focus back to the survivor.
        if (GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId)) {
            paneTwo->open = false;
        }
        const bool paneClosed = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 1; });
        IM_CHECK_NO_RET(paneClosed);
        if (paneClosed) {
            IM_CHECK_EQ_NO_RET(g_ui.gridPanes.front().open, true);
            const bool focusFellBack = YieldUntil(ctx, [&] { return g_ui.focusedPaneId == g_ui.gridPanes.front().id; });
            IM_CHECK_NO_RET(focusFellBack);
            // HIGH-2: the survivor keeps ITS OWN saved view across the host focus
            // reassignment + the following frames' steady-state sync (the bug rebound
            // it to the closed pane's still-active view and persisted the loss).
            ctx->Yield();
            ctx->Yield();
            ctx->Yield();
            IM_CHECK_STR_EQ_NO_RET(g_ui.gridPanes.front().viewId.c_str(), survivorViewIdBeforeClose.c_str());
        }
    };
}

// ---------------------------------------------------------------------------
// GridPanes / NonFocusedVisiblePaneReceivesTickets (multi-grid Slice 3)
//
// Behaviour (1): a non-focused but VISIBLE pane goes live and receives its own ticket rows
// (EnsurePaneLiveSyncStarted one-shot kick). Open a 2nd pane, split it to the right of the primary
// so both windows are on screen, refocus the primary so pane 2 is non-focused-but-visible, and
// assert pane 2's own AppController context is live with a non-empty snapshot that the pane draws —
// a regression that stopped a visible pane syncing would be silent. The rows may come from the
// cache seed, so this proves the kick, not TickAllContexts streaming.
// ---------------------------------------------------------------------------
static void RegisterNonFocusedVisiblePaneSyncs(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridPanes", "NonFocusedVisiblePaneReceivesTickets");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }
        ctx->SetRef(kPrimary);

        // Open a second pane via the production "+" button.
        ctx->ItemClick("+##PaneAdd");
        const bool paneAdded = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 2; });
        IM_CHECK_NO_RET(paneAdded);
        if (!paneAdded) {
            return;
        }
        const std::string paneTwoId = g_ui.gridPanes[1].id;
        // Teardown on every exit path past this point: close pane 2 to restore the min-1 invariant,
        // so a failure here never leaves a later test booting into two panes.
        auto closePaneTwo = [&] {
            if (GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId)) {
                paneTwo->open = false;
            }
            const bool closed = YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 1; });
            IM_CHECK_NO_RET(closed);
        };
        const bool nameReady = YieldUntil(ctx, [&] {
            const GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId);
            return paneTwo != nullptr && !paneTwo->cachedWindowName.empty();
        });
        IM_CHECK_NO_RET(nameReady);
        if (!nameReady) {
            closePaneTwo();
            return;
        }
        const std::string paneTwoName = FindGridPaneById(g_ui.gridPanes, paneTwoId)->cachedWindowName;

        // Let the "+" hand-off settle before moving focus anywhere. The new pane docks as a TAB in
        // the primary's node and re-selects its own tab for selectTabFrames frames, pulling ImGui
        // focus back with it; a focus move issued inside that window is undone a frame later.
        const bool paneTwoSettled = YieldUntil(ctx, [&] {
            const GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId);
            return paneTwo != nullptr && paneTwo->selectTabFrames == 0 && g_ui.focusedPaneId == paneTwoId &&
                   WindowIsLive(paneTwoName.c_str());
        });
        IM_CHECK_NO_RET(paneTwoSettled);
        if (!paneTwoSettled) {
            closePaneTwo();
            return;
        }

        // A tab behind the primary is not visible (its Begin returns false), so split pane 2 to the
        // RIGHT of the primary first: "non-focused but visible" needs both windows on screen at once.
        const std::string paneTwoAbsRef = std::string("//") + paneTwoName;
        const std::string primaryAbsRef = std::string("//") + kPrimary;
        ctx->DockInto(paneTwoAbsRef.c_str(), primaryAbsRef.c_str(), ImGuiDir_Right);
        const bool bothVisible =
            YieldUntil(ctx, [&] { return WindowIsLive(kPrimary) && WindowIsLive(paneTwoName.c_str()); });
        IM_CHECK_NO_RET(bothVisible);
        if (!bothVisible) {
            closePaneTwo();
            return;
        }

        // Refocus the PRIMARY so pane 2 is non-focused BUT still visible. The host adopts a focus
        // report only after two consecutive frames, so require the state to HOLD rather than
        // accepting a single-frame sighting. Pane 2's context must stay live and keep receiving rows.
        ctx->WindowFocus(primaryAbsRef.c_str());
        const bool nonFocusedVisible = YieldUntilHeld(ctx, [&] {
            return g_ui.focusedPaneId == "main" && WindowIsLive(kPrimary) && WindowIsLive(paneTwoName.c_str());
        });
        IM_CHECK_NO_RET(nonFocusedVisible);
        if (!nonFocusedVisible) {
            closePaneTwo();
            return;
        }

        // The non-focused visible pane's rows come from its OWN live context (Slice-3 per-context
        // sync), not from the snapshot "+" copied off the primary at creation: that copy would fill
        // GridPane::ticketsSnapshot even if pane 2 never synced. So assert on the AppController
        // context — sync kicked, a non-empty published snapshot — and that the pane draws exactly it.
        // That snapshot may be the cache seed, so this pins the kick, not later streamed applies.
        const bool paneTwoGotRows = YieldUntil(ctx, [&] {
            const std::shared_ptr<const std::vector<CachedTicket>> live = app->GetPaneTicketsSnapshot(paneTwoId);
            const GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId);
            return app->IsPaneSyncLive(paneTwoId) && live != nullptr && !live->empty() && paneTwo != nullptr &&
                   paneTwo->ticketsSnapshot == live;
        });
        if (!paneTwoGotRows) {
            ctx->LogError("non-focused visible pane 2 never got ticket rows from its own context — "
                          "the Slice-3 per-context live-sync kick (EnsurePaneLiveSyncStarted) did not run "
                          "for it, or the pane is not drawing that context's snapshot");
            IM_CHECK_NO_RET(false);
        }

        closePaneTwo();
    };
}

// ---------------------------------------------------------------------------
// GridPanes / TwoJiraSites* (#2444)
//
// With two Jira sites and no other tracker the "+" caret used to stay hidden: the picker
// counted credentialed tracker KINDS. Both tests add a second Jira site to the session config
// (the fixture serves the grid without Jira credentials, so test values are seeded for the first
// site) and restore it on every exit path through TwoJiraSitesGuard. The driver runs under an
// isolated profile (ui_test_isolate_home) with the scripted fixture backend, so the site switch's
// ConfigManager::Save and the sync on the fake second host stay inside the test.
// ---------------------------------------------------------------------------
namespace {

const char* const kPickerCaret = "\xe2\x96\xbe##PaneBackendPicker";
const char* const kSecondHost = "second-site.example.invalid";

std::string JiraSiteRowLabel(const std::string& host) { return "New Jira \xC2\xB7 " + host + " pane"; }

// The topmost open popup window (the caret popup, or a row's view submenu), else null.
ImGuiWindow* TopPopupWindow() {
    ImGuiContext& g = *GImGui;
    return g.OpenPopupStack.Size == 0 ? nullptr : g.OpenPopupStack.back().Window;
}

// Seeds a two-site Jira config into g_ui.cfg; the destructor puts back the original fields and,
// when the test switched site (`resyncApp` set), saves the restored config so ConfigManager::Load
// serves it again and re-syncs the focused pane on the original site.
struct TwoJiraSitesGuard {
    std::vector<JiraBackendInstance> sites = g_ui.cfg.JiraBackends;
    std::string active = g_ui.cfg.ActiveJiraDomain;
    std::string domain = g_ui.cfg.Domain;
    std::string email = g_ui.cfg.Email;
    std::string token = g_ui.cfg.ApiToken;
    AppController* resyncApp = nullptr;
    bool seeded = false;

    TwoJiraSitesGuard() {
        if (!ConfigManager::BackendCredentialsPresent(g_ui.cfg, "Jira")) {
            g_ui.cfg.JiraBackends.clear(); // re-hydrated below from the seeded live fields
            g_ui.cfg.ActiveJiraDomain.clear();
            g_ui.cfg.Domain = "first-site.example.invalid";
            g_ui.cfg.ApiToken = "ui-test-token";
        }
        smatchet::jira_backends::EnsureHydrated(g_ui.cfg);
        JiraBackendInstance second;
        second.Domain = kSecondHost; // token inherited from the first site
        seeded = smatchet::jira_backends::AddExtra(g_ui.cfg, second);
    }
    ~TwoJiraSitesGuard() {
        g_ui.cfg.JiraBackends = sites;
        g_ui.cfg.ActiveJiraDomain = active;
        g_ui.cfg.Domain = domain;
        g_ui.cfg.Email = email;
        g_ui.cfg.ApiToken = token;
        if (resyncApp != nullptr) {
            ConfigManager::Save(g_ui.cfg);
            resyncApp->SyncWithBackend();
        }
    }
    std::string FirstHost() const {
        return smatchet::jira_backends::NormalizeJiraHost(g_ui.cfg.JiraBackends[0].Domain);
    }
    TwoJiraSitesGuard(const TwoJiraSitesGuard&) = delete;
    TwoJiraSitesGuard& operator=(const TwoJiraSitesGuard&) = delete;
};

// Opens the caret popup of the primary pane and returns it, or null after a logged failure.
ImGuiWindow* OpenPickerPopup(ImGuiTestContext* ctx) {
    ctx->SetRef(kPrimary);
    if (TopPopupWindow() != nullptr) {
        ctx->LogError("a popup was already open before the picker probe");
        return nullptr;
    }
    if (!YieldUntil(ctx, [&] { return ctx->ItemExists(kPickerCaret); }, 30)) {
        ctx->LogError("two Jira sites configured but the \"+\" backend-picker caret is missing (#2444)");
        return nullptr;
    }
    ctx->ItemClick(kPickerCaret);
    ImGuiWindow* popup = nullptr;
    YieldUntil(
        ctx,
        [&] {
            popup = TopPopupWindow();
            return popup != nullptr;
        },
        30);
    if (popup == nullptr) {
        ctx->LogError("the backend-picker caret did not open its popup");
    }
    return popup;
}

} // namespace

static void RegisterTwoJiraSitesShowPickerCaret(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridPanes", "TwoJiraSitesShowPickerCaret");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }
        ctx->SetRef(kPrimary);
        const bool baselineCaret =
            smatchet::pane_targets::ShowNewPaneCaret(smatchet::pane_targets::BuildNewPaneTargets(g_ui.cfg));
        {
            TwoJiraSitesGuard guard;
            IM_CHECK_NO_RET(guard.seeded);
            ImGuiWindow* popup = OpenPickerPopup(ctx);
            IM_CHECK_NO_RET(popup != nullptr);
            if (popup == nullptr) {
                return;
            }
            // Rows are queried in the caret popup itself, never via $FOCUSED: a row under the mouse
            // can open its view submenu, which would then own focus.
            ctx->SetRef(popup->ID);
            IM_CHECK_NO_RET(ctx->ItemExists(JiraSiteRowLabel(guard.FirstHost()).c_str()));
            IM_CHECK_NO_RET(ctx->ItemExists(JiraSiteRowLabel(kSecondHost).c_str()));
            ctx->KeyPress(ImGuiKey_Escape);
            ctx->Yield();
        }
        ctx->SetRef(kPrimary);
        const bool backToBaseline = YieldUntil(ctx, [&] { return ctx->ItemExists(kPickerCaret) == baselineCaret; }, 30);
        IM_CHECK_NO_RET(backToBaseline);
        IM_CHECK_NO_RET(g_ui.gridPanes.size() == 1); // the caret probe never opened a pane
    };
}

// Clicking the other site's row switches the global active site and opens a pane that starts
// without the source pane's rows (those belong to the previous site).
static void RegisterTwoJiraSitesRowOpensPaneOnSite(ImGuiTestEngine* engine) {
    ImGuiTest* t = IM_REGISTER_TEST(engine, "GridPanes", "TwoJiraSitesRowOpensPaneOnSite");
    t->TestFunc = [](ImGuiTestContext* ctx) {
        AppController* app = BootSyncPrimaryLive(ctx);
        if (app == nullptr) {
            return;
        }
        const std::shared_ptr<const std::vector<CachedTicket>> sourceRows = g_ui.gridPanes.front().ticketsSnapshot;
        const std::string sourceViewName = g_ui.gridPanes.front().title;
        TwoJiraSitesGuard guard;
        IM_CHECK_NO_RET(guard.seeded);
        ImGuiWindow* popup = OpenPickerPopup(ctx);
        IM_CHECK_NO_RET(popup != nullptr);
        if (popup == nullptr) {
            return;
        }
        guard.resyncApp = app; // from the click on, the switch may have saved the config
        ctx->SetRef(popup->ID);
        ctx->ItemClick(JiraSiteRowLabel(kSecondHost).c_str());
        // With saved Jira views the row is a submenu: pick the view the source pane shows.
        ctx->Yield();
        ImGuiWindow* submenu = TopPopupWindow();
        if (g_ui.gridPanes.size() == 1 && submenu != nullptr && submenu != popup) {
            ctx->SetRef(submenu->ID);
            ctx->ItemClick(sourceViewName.c_str());
        }
        // Read the new pane's rows on the FIRST frame it exists, before its own sync can publish.
        std::shared_ptr<const std::vector<CachedTicket>> rowsAtCreation;
        const bool paneAdded = YieldUntil(ctx, [&] {
            if (g_ui.gridPanes.size() != 2) {
                return false;
            }
            rowsAtCreation = g_ui.gridPanes[1].ticketsSnapshot;
            return true;
        });
        IM_CHECK_NO_RET(paneAdded);
        if (!paneAdded) {
            return;
        }
        const std::string paneTwoId = g_ui.gridPanes[1].id;
        IM_CHECK_NO_RET(smatchet::jira_backends::HostsMatch(g_ui.cfg.ActiveJiraDomain, kSecondHost));
        IM_CHECK_NO_RET(smatchet::jira_backends::HostsMatch(g_ui.cfg.Domain, kSecondHost));
        IM_CHECK_STR_EQ_NO_RET(g_ui.gridPanes[1].backendKey.c_str(), "Jira");
        // Never the source pane's rows: they were fetched from the first site.
        IM_CHECK_NO_RET(sourceRows != nullptr); // the booted primary pane has rows, so the check below bites
        IM_CHECK_NO_RET(rowsAtCreation != sourceRows);

        // Teardown: close the new pane, let the switch's sync settle, then the guard restores.
        if (GridPane* paneTwo = FindGridPaneById(g_ui.gridPanes, paneTwoId)) {
            paneTwo->open = false;
        }
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return g_ui.gridPanes.size() == 1; }));
        IM_CHECK_NO_RET(YieldUntil(ctx, [&] { return !app->IsSyncPendingOrActive(); }));
    };
}

extern "C" void SmatchetRegisterGridPaneWindowsTests(ImGuiTestEngine* engine) {
    RegisterGridPaneNewSplitFocusClose(engine);
    RegisterNonFocusedVisiblePaneSyncs(engine);
    RegisterTwoJiraSitesShowPickerCaret(engine);
    RegisterTwoJiraSitesRowOpensPaneOnSite(engine);
}

#endif // SMATCHET_BUILD_UI_TESTS
