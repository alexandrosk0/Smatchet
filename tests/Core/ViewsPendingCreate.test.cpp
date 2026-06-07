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
