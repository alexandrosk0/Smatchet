// offline_conflict_modal_panes.test.cpp — bucket-E (ImGui Test Engine) coverage
// for the per-kind offline-conflict resolution modal (DrawOfflineConflictModal,
// SmatchetOfflineQueueUi.cpp, ADR-0016).
//
// The modal dispatches on the parsed `kind` of conflict_context_json into one of
// four panes. This test covers the two scalar-vs-rich-text alternatives the legacy
// `text` pane does NOT exercise:
//   - kind:"scalar"     -> DrawConflictPaneScalar    (Use Mine / Use Theirs / "Save value")
//   - kind:"unverified" -> DrawConflictPaneUnverified ("Force Mine" / Discard my edit)
//
// BEHAVIORAL assertion (no screenshot / golden baseline): the modal is opened by
// seeding the three g_ui trigger fields the production right-click / double-click
// handlers set (showConflictResolveModal + conflictContextJson + conflictResolveDbId),
// then we assert the popup window goes live AND the per-kind branch's signature
// button is reachable. "Save value" exists ONLY in the scalar pane; "Force Mine"
// exists ONLY in the unverified pane — so each button's presence proves the
// correct per-kind branch was reached and rendered without an in-frame ImGui
// assertion (the engine traps any Begin/End or ID imbalance).
//
// FIXTURE + LOCAL-CACHE GATED (mirrors data_dependent_windows_smoke.test.cpp case 8):
// DrawOfflineConflictModal is a file-local static reachable only through
// DrawUnifiedOfflineQueuesPanel, which (a) is drawn inline by the docked
// "Smatchet - Active Project" host pane — live only once tickets load (fixture) —
// and (b) early-returns when the offline queue is empty (total==0). So we sync the
// deterministic Jira fixture to make the host pane live, then enqueue one real
// pending field edit (QueueFieldEditOffline — a pure local-SQLite write, needs the
// SMATCHET_UITEST_WITH_LOCAL_CACHE=1 cache) so total>0 forces the panel body — and
// thus DrawOfflineConflictModal — to run every frame. When either gate is absent the
// test SKIPS with a log rather than asserting on an unreachable modal.

#if defined(SMATCHET_BUILD_UI_TESTS)

#include "AppController.h"
#include "Commands/Scenarios/UiTestScenario.h"
#include "Config/ConfigManager.h" // disable fresh-install read-only mode so the queue write lands
#include "SmatchetUiSession.h"

#include "imgui.h"
#include "imgui_internal.h" // ImGuiWindow, FindWindowByName — real-window probe
#include "imgui_te_context.h"
#include "imgui_te_engine.h"

#include <cstdlib> // getenv — fixture / local-cache env gates
#include <string>

// g_ui — the shared bag of UI-thread visibility flags / modal trigger state.
extern UiDrawSession g_ui;

namespace {

// Yield frames until predicate returns true or the frame budget is exhausted.
template <typename Pred> bool YieldUntil(ImGuiTestContext* ctx, Pred pred, int maxFrames = 300) {
    for (int i = 0; i < maxFrames; ++i) {
        ctx->Yield();
        if (pred()) {
            return true;
        }
    }
    return false;
}

// True once a top-level window with `title` exists and is Active this frame.
bool WindowIsLive(const char* title) {
    const ImGuiWindow* win = ImGui::FindWindowByName(title);
    return win != nullptr && win->Active;
}

// True if the named env var is set (cross-platform getenv; MSVC C4996 quieted).
bool EnvSet(const char* name) {
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const bool set = std::getenv(name) != nullptr;
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    return set;
}

// Open the conflict modal for a given kind by reproducing the production trigger
// state (DrawOfflineRowStateCell double-click / context-menu "Resolve merge
// conflict..."), then wait for the popup window to go live. The panel's
// DrawOfflineConflictModal calls OpenPopup("ResolveMergeConflict") whenever
// showConflictResolveModal is true, so this drives the same path the user does.
bool OpenConflictModal(ImGuiTestContext* ctx, const char* contextJson, std::int64_t dbId) {
    g_ui.conflictResolveBuf.clear();
    g_ui.conflictResolveDbId = dbId;
    g_ui.conflictContextJson = contextJson;
    g_ui.showConflictResolveModal = true;
    return YieldUntil(ctx, [] { return WindowIsLive("ResolveMergeConflict"); });
}

// Cancel out of the live modal without invoking ResolveFieldEditConflict (which
// would need a backend replay). Every pane submits a "Cancel" Button that calls
// ImGui::CloseCurrentPopup(); click it through the popup scope, then wait for the
// window to drop. Falls back to clearing the trigger state if the click misses.
void CancelConflictModal(ImGuiTestContext* ctx) {
    ctx->SetRef("ResolveMergeConflict");
    if (ctx->ItemExists("Cancel")) {
        ctx->ItemClick("Cancel");
    }
    g_ui.showConflictResolveModal = false;
    g_ui.conflictResolveDbId = 0;
    g_ui.conflictContextJson.clear();
    g_ui.conflictResolveBuf.clear();
    YieldUntil(ctx, [] { return !WindowIsLive("ResolveMergeConflict"); });
}

void RunPerKindModalTest(ImGuiTestContext* ctx) {
    if (!EnvSet("SMATCHET_TEST_JIRA_BACKEND_FIXTURE")) {
        ctx->LogInfo("SKIP: SMATCHET_TEST_JIRA_BACKEND_FIXTURE not set — the docked Active Project "
                     "host pane never goes live, so DrawUnifiedOfflineQueuesPanel (and the conflict "
                     "modal it owns) is unreachable. Same gate as data_dependent case 8.");
        return;
    }
    if (!EnvSet("SMATCHET_UITEST_WITH_LOCAL_CACHE")) {
        ctx->LogInfo("SKIP: SMATCHET_UITEST_WITH_LOCAL_CACHE not set — no live local cache, so "
                     "QueueFieldEditOffline cannot persist a pending edit and the panel stays on its "
                     "total==0 early-return path (no modal call site).");
        return;
    }

    AppController* app = SmatchetActiveUiTestAppController();
    if (app == nullptr) {
        ctx->LogInfo("SKIP: SmatchetActiveUiTestAppController() returned nullptr — app not booted");
        return;
    }

    // Make the host pane live: sync the fixture so tickets load + the docked
    // "Smatchet - Active Project" window activates (mirrors data_dependent case 8).
    app->SyncWithBackend();
    const bool syncDone = YieldUntil(ctx, [&] { return !app->IsStreamingSyncActive(); });
    IM_CHECK_NO_RET(syncDone);
    IM_CHECK_NO_RET(!app->GetActiveTickets().empty());

    const char* kHostTitle = "Smatchet - Active Project";
    ctx->SetRef(kHostTitle);
    const bool hostLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kHostTitle);
    });
    IM_CHECK_NO_RET(hostLive);
    if (!hostLive) {
        return;
    }

    // The harness boots a fresh profile in the isolated SMATCHET_USER_DATA dir,
    // and ConfigManager defaults fresh installs to read-only (no config file →
    // ReadOnlyMode=true), which would reject the queue write below. Persist
    // read-only=false for the duration of the test, then restore it. We hard-flip
    // (not skip) because with both env gates satisfied the queue write MUST land —
    // a silent skip here would hide a real regression in the offline path.
    const bool prevReadOnly = ConfigManager::Load().ReadOnlyMode;
    {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.ReadOnlyMode = false;
        ConfigManager::Save(cfg);
        ConfigManager::InvalidateCache();
    }

    // Populate the queue so total>0 forces the panel body (and the modal call
    // site) to render. A pure local-SQLite write — no backend round-trip.
    std::string queueErr;
    const std::int64_t editId =
        app->QueueFieldEditOffline("SMAT-1", "summary", "{\"summary\":\"offline conflict modal pane test\"}", queueErr,
                                   std::string(), std::string("mine-original"), true);
    if (editId <= 0) {
        ctx->LogError("QueueFieldEditOffline returned %lld (err='%s') with read-only disabled + live "
                      "cache — the modal call site is unreachable; failing rather than faking green.",
                      static_cast<long long>(editId), queueErr.c_str());
    }
    IM_CHECK_NO_RET(editId > 0);
    IM_CHECK_NO_RET(!app->GetPendingFieldEdits().empty());
    if (editId <= 0) {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.ReadOnlyMode = prevReadOnly;
        ConfigManager::Save(cfg);
        ConfigManager::InvalidateCache();
        return;
    }

    // Keep the host pane the active dock tab while the modal drives.
    const bool stillLive = YieldUntil(ctx, [&] {
        g_ui.requestActiveProjectFocus = true;
        return WindowIsLive(kHostTitle);
    });
    IM_CHECK_NO_RET(stillLive);

    // ---- Pane 1: kind:"scalar" -> DrawConflictPaneScalar -------------------
    // Signature widget: "Save value" Button (exists in NO other pane: text has
    // "Save resolved", unverified has none, unknown has none).
    const bool scalarOpened = OpenConflictModal(ctx, "{\"kind\":\"scalar\",\"mine\":\"5\",\"theirs\":\"8\"}", editId);
    IM_CHECK_NO_RET(scalarOpened);
    if (scalarOpened) {
        ctx->SetRef("ResolveMergeConflict");
        const bool scalarBtn = ctx->ItemExists("Save value");
        // Negative cross-check: the unverified-only button must be absent in the
        // scalar pane, proving the dispatch is exclusive (not a both-rendered bug).
        const bool forceMineAbsent = !ctx->ItemExists("Force Mine");
        IM_CHECK_NO_RET(scalarBtn);
        IM_CHECK_NO_RET(forceMineAbsent);
    }
    CancelConflictModal(ctx);

    // ---- Pane 2: kind:"unverified" -> DrawConflictPaneUnverified -----------
    // Signature widget: "Force Mine" Button (exists in NO other pane).
    const bool unverifiedOpened =
        OpenConflictModal(ctx, "{\"kind\":\"unverified\",\"mine\":\"mine-original\"}", editId);
    IM_CHECK_NO_RET(unverifiedOpened);
    if (unverifiedOpened) {
        ctx->SetRef("ResolveMergeConflict");
        const bool forceMineBtn = ctx->ItemExists("Force Mine");
        // Negative cross-check: the scalar-only button must be absent here.
        const bool saveValueAbsent = !ctx->ItemExists("Save value");
        IM_CHECK_NO_RET(forceMineBtn);
        IM_CHECK_NO_RET(saveValueAbsent);
    }
    CancelConflictModal(ctx);

    // Restore: drop the synthetic queue row so it never leaks into a later test,
    // and put read-only mode back to where the harness had it.
    app->DeletePendingFieldEdits({editId});
    {
        TrackerConfig cfg = ConfigManager::Load();
        cfg.ReadOnlyMode = prevReadOnly;
        ConfigManager::Save(cfg);
        ConfigManager::InvalidateCache();
    }
    g_ui.requestActiveProjectFocus = false;
    ctx->Yield();
}

} // namespace

extern "C" void SmatchetRegisterOfflineConflictModalPanesTests(ImGuiTestEngine* engine) {
    ImGuiTest* t =
        IM_REGISTER_TEST(engine, "OfflineConflictModalPanes", "ConflictModal_ScalarAndUnverifiedPerKindPanes");
    t->TestFunc = [](ImGuiTestContext* ctx) { RunPerKindModalTest(ctx); };
}

#endif // SMATCHET_BUILD_UI_TESTS
