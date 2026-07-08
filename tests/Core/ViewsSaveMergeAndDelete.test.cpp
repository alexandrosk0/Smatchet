#include <doctest/doctest.h>

#include "SmatchetViewsDashboardUi_detail.h" // FindViewName (pulls Views.h)
#include "Views.h"                            // smatchet::views_merge helpers

#include <cstddef>
#include <string>
#include <vector>

// DR13a + DR13b — pure, ImGui-free helpers backing the persistent-views clobber fix and the
// delete/rename-wrong-view fix. See Views.h (views_merge) and SmatchetViewsDashboardUi_detail.h.

using smatchet::views_merge::MergePersistentViewsToolbarAppend;
using smatchet::views_merge::PickActiveViewIndexAfterErase;
using SmatchetViewsDashboardUiDetail::FindViewName;

namespace {

ToolbarButton MakeButton(const std::string& commandId) {
    ToolbarButton b;
    b.Kind = ToolbarButtonKind::Command;
    b.CommandId = commandId;
    return b;
}

ViewWorkspaceState MakeWorkspace(const std::string& activeId, const std::vector<std::string>& viewIds,
                                 const std::vector<std::string>& toolbarCommandIds) {
    ViewWorkspaceState ws;
    ws.ActiveViewId = activeId;
    for (const auto& id : viewIds) {
        ViewDefinition v;
        v.Id = id;
        v.Name = id;
        ws.Views.push_back(v);
    }
    for (const auto& cid : toolbarCommandIds) {
        ws.ToolbarAppend.push_back(MakeButton(cid));
    }
    return ws;
}

} // namespace

// --- DR13a: MergePersistentViewsToolbarAppend ------------------------------------------------------

TEST_CASE("MergePersistentViewsToolbarAppend: out-of-band ToolbarAppend wins over the stale snapshot") {
    // In-memory snapshot (about to be written by Views::Save) carries the boot-time — now stale —
    // ToolbarAppend. The freshly re-read disk file has the button the toolbar worker wrote out of band.
    PersistentViewsFile target;
    target.Backends["Jira"] = MakeWorkspace("v1", {"v1", "v2"}, /*toolbar=*/{});

    PersistentViewsFile fresh;
    fresh.Backends["Jira"] = MakeWorkspace("vX", {"vX"}, /*toolbar=*/{"cmd.refresh"});

    MergePersistentViewsToolbarAppend(target, fresh);

    // Views / ActiveViewId stay owned by the wrapper's snapshot ...
    CHECK(target.Backends["Jira"].ActiveViewId == "v1");
    REQUIRE(target.Backends["Jira"].Views.size() == 2);
    // ... but the out-of-band ToolbarAppend is preserved (was empty, now folded back in).
    REQUIRE(target.Backends["Jira"].ToolbarAppend.size() == 1);
    CHECK(target.Backends["Jira"].ToolbarAppend[0].CommandId == "cmd.refresh");
}

TEST_CASE("MergePersistentViewsToolbarAppend: a disk-only backend bucket is preserved whole") {
    PersistentViewsFile target;
    target.Backends["Jira"] = MakeWorkspace("v1", {"v1"}, {});

    PersistentViewsFile fresh;
    fresh.Backends["Jira"] = MakeWorkspace("v1", {"v1"}, {});
    fresh.Backends["Plane"] = MakeWorkspace("p1", {"p1", "p2"}, {"cmd.plane"});

    MergePersistentViewsToolbarAppend(target, fresh);

    REQUIRE(target.Backends.count("Plane") == 1);
    CHECK(target.Backends["Plane"].ActiveViewId == "p1");
    CHECK(target.Backends["Plane"].Views.size() == 2);
    REQUIRE(target.Backends["Plane"].ToolbarAppend.size() == 1);
    CHECK(target.Backends["Plane"].ToolbarAppend[0].CommandId == "cmd.plane");
}

TEST_CASE("MergePersistentViewsToolbarAppend: a backend only in the snapshot keeps its ToolbarAppend") {
    // The wrapper created a bucket this session that is not yet on disk — its in-memory ToolbarAppend
    // must survive the merge untouched (nothing on disk to fold in).
    PersistentViewsFile target;
    target.Backends["GitHub"] = MakeWorkspace("g1", {"g1"}, {"cmd.local"});

    PersistentViewsFile fresh; // empty disk

    MergePersistentViewsToolbarAppend(target, fresh);

    REQUIRE(target.Backends["GitHub"].ToolbarAppend.size() == 1);
    CHECK(target.Backends["GitHub"].ToolbarAppend[0].CommandId == "cmd.local");
}

// --- DR13b: PickActiveViewIndexAfterErase ----------------------------------------------------------

TEST_CASE("PickActiveViewIndexAfterErase: keeps the same slot when it still exists") {
    // Erase index 1 of {0,1,2} -> remaining 2 entries {0,2}; slot 1 still valid -> lands on new [1].
    CHECK(PickActiveViewIndexAfterErase(/*erasedIndex=*/1, /*remainingCount=*/2) == 1);
}

TEST_CASE("PickActiveViewIndexAfterErase: deleting the last view lands on the new last") {
    // Erase index 2 of {0,1,2} -> remaining 2 entries; slot 2 gone -> clamp to last (1).
    CHECK(PickActiveViewIndexAfterErase(/*erasedIndex=*/2, /*remainingCount=*/2) == 1);
}

TEST_CASE("PickActiveViewIndexAfterErase: deleting the first view stays on the new first") {
    CHECK(PickActiveViewIndexAfterErase(/*erasedIndex=*/0, /*remainingCount=*/3) == 0);
}

// --- DR13b: FindViewName ---------------------------------------------------------------------------

TEST_CASE("FindViewName: resolves a non-active target's display name for the delete prompt") {
    ViewsStore store;
    ViewDefinition a;
    a.Id = "v1";
    a.Name = "Active";
    ViewDefinition b;
    b.Id = "v2";
    b.Name = "Other";
    store.Views = {a, b};

    // The delete-confirm targets the RIGHT-CLICKED id, not the active one.
    CHECK(FindViewName(store, "v2", /*fallback=*/"Active") == "Other");
    CHECK(FindViewName(store, "v1", "Active") == "Active");
}

TEST_CASE("FindViewName: unknown id falls back to the active view name") {
    ViewsStore store;
    ViewDefinition a;
    a.Id = "v1";
    a.Name = "Active";
    store.Views = {a};

    CHECK(FindViewName(store, "gone", /*fallback=*/"Active") == "Active");
}
