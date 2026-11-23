// ViewsPendingCreate.test.cpp — bucket-A coverage of the deferred view-create
// latch core (SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore),
// added with the June 2026 view-create crash fix (Pillar 3):
//
//   Views::Create push_backs into store.Views — a mid-frame create from a UI
//   click handler reallocated the vector and dangled every ViewDefinition*
//   resolved earlier in the frame (dashboard ctx.activeView, pane
//   activeViewForGrid); the remainder of the frame read freed memory and the
//   session died via unhandled C++ exception -> std::terminate (minidump:
//   TerminateHandler over SmatchetUI::Draw -> drawSecondaryWindows).
//
//   The fix defers the mutation: click sites copy a payload while their
//   pointers are still valid and latch it (UiDrawSession::viewsPendingCreate);
//   SmatchetUI::Draw consumes the latch at the TOP of the next frame via this
//   core, BEFORE any view pointer is resolved.
//
// Pinned invariants:
//   * consume-once: the latch applies exactly once and resets itself
//   * the created view is appended + becomes active
//   * an unset latch never mutates the store
//   * the duplicate path: the source view survives the create by value
//
// Views persists through ConfigManager statics, so each case runs under a
// TestEnvGuard-redirected user-data dir (same harness as ViewsClass.test.cpp).

#include "../support/TestEnvGuard.h"

#include "ConfigManager.h"
#include "SmatchetViewsDashboardUi_detail.h"
#include "Views.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

/// Removes smatchet_views.json while the TestEnvGuard redirect is still active.
struct ViewsFileCleanup {
    ~ViewsFileCleanup() { std::remove(ConfigManager::GetViewsPath().c_str()); }
};

TrackerConfig MakeCfg(const std::string& trackerType) {
    TrackerConfig cfg;
    cfg.TrackerType = trackerType;
    return cfg;
}

} // namespace

TEST_CASE("ApplyPendingViewCreateCore: consume-once latch appends + activates the payload view") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(views.GetStore().Views.size() == 1);
    const std::string originalActiveId = views.GetStore().ActiveViewId;

    bool requested = true;
    ViewDefinition payload;
    payload.Name = "New View";
    payload.Jql = "project = SMAT";
    payload.Fields = {"summary", "status"};

    const ViewDefinition* nowActive =
        SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore(views, requested, payload);

    REQUIRE(nowActive != nullptr);
    CHECK(nowActive->Name == "New View");
    CHECK(nowActive->Jql == "project = SMAT");
    CHECK(views.GetStore().Views.size() == 2);
    CHECK(views.GetStore().ActiveViewId == nowActive->Id);
    CHECK(nowActive->Id != originalActiveId);
    // Latch consumed: flag reset, payload cleared.
    CHECK_FALSE(requested);
    CHECK(payload.Name.empty());

    // Consume-once: a second apply with the (now reset) latch is a no-op.
    const ViewDefinition* second =
        SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore(views, requested, payload);
    CHECK(second == nullptr);
    CHECK(views.GetStore().Views.size() == 2);
}

TEST_CASE("ApplyPendingViewCreateCore: an unset latch never mutates the store") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    const std::size_t sizeBefore = views.GetStore().Views.size();
    const std::uint64_t revBefore = views.GetRevision();

    bool requested = false;
    ViewDefinition payload;
    payload.Name = "Must not appear";

    const ViewDefinition* result =
        SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore(views, requested, payload);

    CHECK(result == nullptr);
    CHECK(views.GetStore().Views.size() == sizeBefore);
    CHECK(views.GetRevision() == revBefore);
    // Payload untouched on the no-op path (the latch owner may still be filling it).
    CHECK(payload.Name == "Must not appear");
}

TEST_CASE("ApplyPendingViewCreateCore: duplicate path — the source view survives the create by value") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    REQUIRE(views.GetActiveView() != nullptr);
    const std::string sourceId = views.GetActiveView()->Id;
    const std::string sourceName = views.GetActiveView()->Name;

    // The crash-pattern sequence, made safe by deferral: the payload is a COPY of a
    // store entry (taken at click time, while the pointer is valid); the create runs
    // later. After the realloc the source entry is intact by value and the new entry
    // is a distinct, active view.
    bool requested = true;
    ViewDefinition payload = *views.GetActiveView();
    payload.Id.clear();
    payload.Name = sourceName + " (copy)";

    const ViewDefinition* nowActive =
        SmatchetViewsDashboardUiDetail::ApplyPendingViewCreateCore(views, requested, payload);

    REQUIRE(nowActive != nullptr);
    CHECK(nowActive->Name == sourceName + " (copy)");
    CHECK(nowActive->Id != sourceId);
    REQUIRE(views.GetStore().Views.size() == 2);
    // Source view unharmed (re-resolve by id — never by stale pointer).
    bool sourceFound = false;
    for (const auto& v : views.GetStore().Views) {
        if (v.Id == sourceId) {
            sourceFound = true;
            CHECK(v.Name == sourceName);
        }
    }
    CHECK(sourceFound);
}

// ---- View-delete Undo (docs/plans/input-ux-w2-view-delete-undo.md) ----------------------------------------

namespace {

struct FakePane {
    std::string id;
    std::string backendKey;
    std::string viewId;
};

} // namespace

TEST_CASE("PaneIdsShowingView: lists the panes bound to the view, nothing for an empty id") {
    const std::vector<FakePane> panes = {{"main", "jira", "v1"}, {"pane-2", "jira", "v2"}, {"pane-3", "plane", "v1"}};
    const std::vector<std::string> ids = SmatchetViewsDashboardUiDetail::PaneIdsShowingView(panes, "v1");
    REQUIRE(ids.size() == 2);
    CHECK(ids[0] == "main");
    CHECK(ids[1] == "pane-3");
    CHECK(SmatchetViewsDashboardUiDetail::PaneIdsShowingView(panes, "").empty());
}

TEST_CASE("RebindPanesToRestoredView: rebinds listed same-tracker panes still on the fallback, never the focused one") {
    // After the delete of "v1", the panes that showed it fell back to the active view "v2"; "pane-5" is hidden
    // and still names "v1"; "pane-6" showed it and the user has since switched it to "v3".
    std::vector<FakePane> panes = {{"main", "jira", "v2"},   {"pane-2", "jira", "v2"}, {"pane-3", "plane", "v2"},
                                   {"pane-4", "jira", "v2"}, {"pane-5", "jira", "v1"}, {"pane-6", "jira", "v3"}};
    SmatchetViewsDashboardUiDetail::PaneRestoreTarget target;
    target.PaneIds = {"main", "pane-2", "pane-3", "pane-5", "pane-6"};
    target.BackendKey = "jira";
    target.DeletedId = "v1";
    target.FallbackId = "v2";
    target.RestoredId = "v1-restored";
    CHECK(SmatchetViewsDashboardUiDetail::RebindPanesToRestoredView(panes, "main", target));
    CHECK(panes[0].viewId == "v2");          // focused: follows the active view
    CHECK(panes[1].viewId == "v1-restored"); // showed the view, fell back: back on it
    CHECK(panes[2].viewId == "v2");          // another tracker's pane
    CHECK(panes[3].viewId == "v2");          // never showed it
    CHECK(panes[4].viewId == "v1-restored"); // hidden, still on the deleted id
    CHECK(panes[5].viewId == "v3");          // the user moved it to another view since
    CHECK_FALSE(SmatchetViewsDashboardUiDetail::RebindPanesToRestoredView(panes, "main", target));
}

TEST_CASE("RestoredViewTakesFocus: follows the pane focused at Undo time, else whether the view was active") {
    const std::vector<FakePane> panes = {{"p1", "jira", "v2"}, {"p2", "jira", "c"}};
    const std::vector<std::string> showed = {"p1"};
    // The focused pane showed the deleted view: it takes focus, whether or not it was active at the delete.
    CHECK(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "p1", "p1", showed, true));
    CHECK(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "p1", "p2", showed, false));
    // Focus moved to a pane that showed another view during the Undo window: the user stays there.
    CHECK_FALSE(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "p2", "p1", showed, true));
    // Mobile: the focused pane had not followed the active view (no grid drawn), so it is not in `paneIds`;
    // the deleted view was active and focus did not move, so it takes focus.
    CHECK(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "p2", "p2", showed, true));
    CHECK_FALSE(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "p2", "p2", showed, false));
    // No focused pane (none, or an id no pane has): whether it was the active view decides.
    CHECK(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "", "", showed, true));
    CHECK_FALSE(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(panes, "gone", "gone", showed, false));
    const std::vector<FakePane> none;
    CHECK(SmatchetViewsDashboardUiDetail::RestoredViewTakesFocus(none, "p1", "p1", showed, true));
}

TEST_CASE("View-delete Undo: re-creating the captured definition keeps its id and makes it active") {
    smatchet_tests::TestEnvGuard env;
    ViewsFileCleanup cleanup;

    Views views;
    views.EnsureLoaded(MakeCfg("Jira"));
    ViewDefinition proto;
    proto.Name = "Undo me";
    proto.Jql = "project = UNDO";
    REQUIRE(views.Create(proto));
    const ViewDefinition captured = *views.GetActiveView();
    REQUIRE(views.DeleteActive());
    REQUIRE(views.Find(captured.Id) == nullptr);

    REQUIRE(views.Create(captured));
    const ViewDefinition* restored = views.GetActiveView();
    REQUIRE(restored != nullptr);
    CHECK(restored->Id == captured.Id);
    CHECK(restored->Jql == "project = UNDO");
}
