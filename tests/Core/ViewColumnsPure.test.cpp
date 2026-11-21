// Bucket-A doctest for ViewColumnsPure.h — the ordered-column-list model that replaces
// ViewDefinition::ColumnOrder + ColumnWidths + a separately-mutable Fields
// (column-view-save-simplification). No ImGui, no session state.

#include "ViewColumnsPure.h"

#include <doctest/doctest.h>

#include <limits>

#include <algorithm>

namespace {

ViewDefinition MakeView(std::vector<ViewColumn> columns) {
    ViewDefinition v;
    v.Id = "v1";
    v.Name = "Test View";
    v.Columns = std::move(columns);
    NormalizeViewDefinition(v);
    return v;
}

} // namespace

TEST_CASE("DefaultColumnWidthPx / EffectiveColumnWidth") {
    CHECK(DefaultColumnWidthPx("id") == doctest::Approx(90.0f));
    CHECK(DefaultColumnWidthPx("field:summary") == doctest::Approx(180.0f));

    ViewDefinition v;
    v.Columns = {{"id", 0.0f}, {"field:summary", 320.0f}};
    // A stored width of 0 (unset) falls back to the kind default.
    CHECK(EffectiveColumnWidth(v, "id") == doctest::Approx(90.0f));
    CHECK(EffectiveColumnWidth(v, "field:summary") == doctest::Approx(320.0f));
    // A key not on the view at all also falls back to the kind default rather than 0.
    CHECK(EffectiveColumnWidth(v, "field:status") == doctest::Approx(180.0f));
}

TEST_CASE("ShouldCaptureColumnWidth") {
    CHECK_FALSE(ShouldCaptureColumnWidth(180.0f, 180.0f, 0.5f));
    CHECK_FALSE(ShouldCaptureColumnWidth(180.0f, 180.3f, 0.5f));
    CHECK(ShouldCaptureColumnWidth(180.0f, 240.0f, 0.5f));
    CHECK_FALSE(ShouldCaptureColumnWidth(90.0f, 90.0f, 0.5f));
}

TEST_CASE("FieldIdsFromColumns strips the field: prefix, in order, id excluded") {
    const std::vector<ViewColumn> columns = {{"id", 90.0f}, {"field:status", 0.0f}, {"field:summary", 0.0f}};
    CHECK(FieldIdsFromColumns(columns) == std::vector<std::string>{"status", "summary"});
}

TEST_CASE("MigrateLegacyColumns reproduces the pre-v3 TicketGridColumnsBuilder::Build order") {
    SUBCASE("empty ColumnOrder — id then fields in Fields order") {
        const auto columns = MigrateLegacyColumns({"summary", "status", "priority"}, {}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:summary", "field:status", "field:priority"});
    }
    SUBCASE("complete ColumnOrder — that order wins outright") {
        const auto columns = MigrateLegacyColumns({"summary", "status"}, {"id", "field:status", "field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:summary"});
    }
    SUBCASE("short ColumnOrder — a field added elsewhere is appended in Fields order, not dropped") {
        const auto columns = MigrateLegacyColumns({"summary", "status", "assignee"}, {"id", "field:status"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:summary", "field:assignee"});
    }
    SUBCASE("ColumnOrder missing id — id is not silently invented by this pass, it is what "
            "MigrateLegacyColumns is given, and 'id' only comes from Fields' own allKeys seed") {
        const auto columns = MigrateLegacyColumns({"summary"}, {"field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        // "id" is always in allKeys (MigrateLegacyColumns seeds it unconditionally) and was not
        // consumed by columnOrder, so it lands in the tail-append.
        CHECK(keys == std::vector<std::string>{"field:summary", "id"});
    }
    SUBCASE("stale ColumnOrder key naming a field that no longer exists is dropped") {
        const auto columns = MigrateLegacyColumns({"summary"}, {"id", "field:gone", "field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:summary"});
    }
    SUBCASE("legacy comment alias folds onto comments, deduped") {
        const auto columns = MigrateLegacyColumns({"comment"}, {"id", "field:comments"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:comments"});
    }
    SUBCASE("widths carry over by key") {
        const auto columns = MigrateLegacyColumns({"summary"}, {}, {{"id", 90.0f}, {"field:summary", 250.0f}});
        REQUIRE(columns.size() == 2);
        CHECK(columns[0].Key == "id");
        CHECK(columns[0].Width == doctest::Approx(90.0f));
        CHECK(columns[1].Width == doctest::Approx(250.0f));
    }
}

TEST_CASE("NormalizeViewDefinition") {
    SUBCASE("idempotent for every legacy shape MigrateLegacyColumns can produce") {
        ViewDefinition v = MakeView(MigrateLegacyColumns({"summary", "status"}, {"field:status"}, {}));
        const std::vector<ViewColumn> once = v.Columns;
        NormalizeViewDefinition(v);
        CHECK(v.Columns == once);
    }
    SUBCASE("dedupes, keeping the first occurrence's width") {
        ViewDefinition v;
        v.Columns = {{"field:summary", 200.0f}, {"field:summary", 999.0f}, {"id", 90.0f}};
        NormalizeViewDefinition(v);
        REQUIRE(v.Columns.size() == 2);
        const auto it = std::find_if(v.Columns.begin(), v.Columns.end(),
                                     [](const ViewColumn& c) { return c.Key == "field:summary"; });
        REQUIRE(it != v.Columns.end());
        CHECK(it->Width == doctest::Approx(200.0f));
    }
    SUBCASE("a key the grid cannot render is dropped") {
        // A bare field id (not "field:<id>") or an empty "field:" never renders, so keeping it would make
        // the rendered column order differ from Columns on every frame.
        ViewDefinition v;
        v.Columns = {{"id", 90.0f}, {"summary", 0.0f}, {"field:", 0.0f}, {"field:status", 0.0f}};
        NormalizeViewDefinition(v);
        REQUIRE(v.Columns.size() == 2);
        CHECK(v.Columns[0].Key == "id");
        CHECK(v.Columns[1].Key == "field:status");
    }
    SUBCASE("a non-finite or oversized width falls back to the default") {
        ViewDefinition v;
        v.Columns = {{"id", std::numeric_limits<float>::infinity()},
                     {"field:summary", 1.0e9f},
                     {"field:status", std::numeric_limits<float>::quiet_NaN()}};
        NormalizeViewDefinition(v);
        REQUIRE(v.Columns.size() == 3);
        CHECK(v.Columns[0].Width == doctest::Approx(90.0f));
        CHECK(v.Columns[1].Width == doctest::Approx(180.0f));
        CHECK(v.Columns[2].Width == doctest::Approx(180.0f));
    }
    SUBCASE("id is prepended when absent") {
        ViewDefinition v;
        v.Columns = {{"field:summary", 0.0f}};
        NormalizeViewDefinition(v);
        REQUIRE(!v.Columns.empty());
        CHECK(v.Columns.front().Key == "id");
    }
    SUBCASE("every zero/negative width is filled from the kind default") {
        ViewDefinition v;
        v.Columns = {{"id", 0.0f}, {"field:summary", -5.0f}};
        NormalizeViewDefinition(v);
        for (const auto& c : v.Columns) {
            CHECK(c.Width > 0.0f);
        }
    }
    SUBCASE("Fields is regenerated from Columns, in Columns order — never assigned by hand") {
        ViewDefinition v;
        v.Fields = {"zzz", "aaa"}; // stale / hand-set — must be overwritten
        v.Columns = {{"id", 0.0f}, {"field:status", 0.0f}, {"field:summary", 0.0f}};
        NormalizeViewDefinition(v);
        CHECK(v.Fields == std::vector<std::string>{"status", "summary"});
    }
    SUBCASE("SortSpecs on a key no longer in Columns is pruned") {
        ViewDefinition v;
        v.Columns = {{"id", 0.0f}, {"field:status", 0.0f}};
        v.SortSpecs = {{"field:status", 1}, {"field:gone", 2}};
        NormalizeViewDefinition(v);
        REQUIRE(v.SortSpecs.size() == 1);
        CHECK(v.SortSpecs.front().ColumnKey == "field:status");
    }
    SUBCASE("a fresh bootstrap-shaped view (only id has a stored width) normalizes to a stable, "
            "fully-widthed shape — the direct regression guard for the launch-time phantom dirty") {
        ViewDefinition v;
        v.Fields = {"summary", "assignee", "priority"};
        v.Columns = MigrateLegacyColumns(v.Fields, {}, {{"id", 90.0f}});
        NormalizeViewDefinition(v);
        for (const auto& c : v.Columns) {
            CHECK(c.Width > 0.0f);
        }
        CHECK(EffectiveColumnWidth(v, "field:summary") == doctest::Approx(180.0f));
    }
}

TEST_CASE("ReorderViewColumns") {
    SUBCASE("a complete permutation reorders with no ignored keys — the header-drag case") {
        ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 180.0f}, {"field:status", 180.0f}});
        const auto ignored = ReorderViewColumns({"field:status", "id", "field:summary"}, v);
        CHECK(ignored.empty());
        std::vector<std::string> keys;
        for (const auto& c : v.Columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"field:status", "id", "field:summary"});
    }
    SUBCASE("widths survive the reorder") {
        ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 320.0f}});
        ReorderViewColumns({"field:summary", "id"}, v);
        REQUIRE(v.Columns.size() == 2);
        CHECK(v.Columns.front().Key == "field:summary");
        CHECK(v.Columns.front().Width == doctest::Approx(320.0f));
    }
    SUBCASE("a partial order tail-appends the rest instead of dropping them — the command-surface case") {
        ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}, {"field:status", 0.0f}});
        const auto ignored = ReorderViewColumns({"field:status"}, v);
        CHECK(ignored.empty());
        std::vector<std::string> keys;
        for (const auto& c : v.Columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"field:status", "id", "field:summary"});
    }
    SUBCASE("an unknown key is reported in ignored, not silently dropped or invented") {
        ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}});
        const auto ignored = ReorderViewColumns({"field:typo", "id", "field:summary"}, v);
        REQUIRE(ignored.size() == 1);
        CHECK(ignored.front() == "field:typo");
        std::vector<std::string> keys;
        for (const auto& c : v.Columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:summary"});
    }
}

namespace {

std::vector<std::string> KeysOf(const ViewDefinition& v) {
    std::vector<std::string> keys;
    for (const auto& c : v.Columns) {
        keys.push_back(c.Key);
    }
    return keys;
}

} // namespace

TEST_CASE("ViewDraftHasUnsavedEdits") {
    SUBCASE("a fresh draft equal to saved is never unsaved") {
        const ViewDefinition saved = MakeView(MigrateLegacyColumns({"summary", "status"}, {}, {{"id", 90.0f}}));
        CHECK_FALSE(ViewDraftHasUnsavedEdits(saved, saved));
    }
    SUBCASE("name, query and column set / order are editor edits") {
        const ViewDefinition saved = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}, {"field:status", 0.0f}});
        ViewDefinition draft = saved;
        draft.Name = "Renamed";
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));
        draft = saved;
        draft.Jql = "status = Open";
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));
        draft = saved;
        std::swap(draft.Columns[1], draft.Columns[2]);
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));
        draft = saved;
        draft.Columns.pop_back();
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));
    }
    SUBCASE("layout is never an unsaved edit — the grid and Sort tab autosave it") {
        // Regression guard: counting these made every view switch after a grid resize, sort or
        // hierarchy toggle raise "You have unsaved changes" with no edit behind it.
        const ViewDefinition saved = MakeView({{"id", 90.0f}, {"field:summary", 180.0f}, {"field:status", 0.0f}});
        ViewDefinition draft = saved;
        draft.Columns[1].Width = 400.0f;
        draft.SortSpecs.push_back({"field:status", 1});
        draft.HideParents = !saved.HideParents;
        draft.StoryGroupSort = !saved.StoryGroupSort;
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
    }
}

TEST_CASE("MergeDraftEditsOntoSaved") {
    ViewDefinition saved = MakeView({{"id", 90.0f}, {"field:summary", 320.0f}, {"field:status", 150.0f}});
    saved.SortSpecs = {{"field:status", 2}};
    saved.HideParents = true;
    ViewDefinition draft = saved;
    draft.Name = "Edited";
    draft.Jql = "project = X";
    draft.Columns = {{"id", 90.0f}, {"field:status", 1.0f}, {"field:summary", 1.0f}, {"field:assignee", 0.0f}};

    const ViewDefinition merged = MergeDraftEditsOntoSaved(draft, saved);
    CHECK(merged.Id == saved.Id);
    CHECK(merged.Name == "Edited");
    CHECK(merged.Jql == "project = X");
    CHECK(KeysOf(merged) == std::vector<std::string>{"id", "field:status", "field:summary", "field:assignee"});
    // Widths come from the saved view, never from the draft's load-time copy.
    CHECK(EffectiveColumnWidth(merged, "field:summary") == doctest::Approx(320.0f));
    CHECK(EffectiveColumnWidth(merged, "field:status") == doctest::Approx(150.0f));
    CHECK(EffectiveColumnWidth(merged, "field:assignee") == doctest::Approx(180.0f));
    // Layout is the saved view's.
    CHECK(merged.SortSpecs == saved.SortSpecs);
    CHECK(merged.HideParents);
    CHECK(merged.Fields == std::vector<std::string>{"status", "summary", "assignee"});
}

TEST_CASE("RebaseViewDraft") {
    const ViewDefinition loaded =
        MakeView({{"id", 90.0f}, {"field:summary", 180.0f}, {"field:status", 180.0f}, {"field:priority", 180.0f}});

    SUBCASE("no change underneath is a no-op") {
        ViewDefinition draft = loaded;
        draft.Name = "mid-edit";
        ViewDefinition base = loaded;
        const ViewDraftRebaseResult r = RebaseViewDraft(draft, base, loaded);
        CHECK_FALSE(r.NameAdopted);
        CHECK_FALSE(r.JqlAdopted);
        CHECK(draft.Name == "mid-edit");
    }
    SUBCASE("a grid drag + resize + sort on a clean draft is adopted, and the draft stays clean") {
        // The reported bug: drag a column in the grid, then switch view -> phantom "unsaved"
        // prompt, and "Save & switch" wrote the stale order back over the drag.
        ViewDefinition draft = loaded;
        ViewDefinition base = loaded;
        ViewDefinition saved = loaded;
        ReorderViewColumns({"id", "field:priority", "field:summary", "field:status"}, saved);
        saved.Columns[2].Width = 333.0f;
        saved.SortSpecs = {{"field:priority", 1}};

        RebaseViewDraft(draft, base, saved);
        CHECK(draft == saved);
        CHECK(base == saved);
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
        CHECK(MergeDraftEditsOntoSaved(draft, saved) == saved);
    }
    SUBCASE("an editor column edit survives a grid change underneath; widths follow the saved view") {
        ViewDefinition draft = loaded;
        ViewDefinition base = loaded;
        draft.Columns.pop_back(); // user unticked priority in the Fields tab
        ViewDefinition saved = loaded;
        ReorderViewColumns({"id", "field:status", "field:summary", "field:priority"}, saved); // grid drag
        saved.Columns[1].Width = 250.0f;                                                      // grid resize

        RebaseViewDraft(draft, base, saved);
        CHECK(KeysOf(draft) == std::vector<std::string>{"id", "field:summary", "field:status"});
        CHECK(EffectiveColumnWidth(draft, "field:status") == doctest::Approx(250.0f));
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));
        CHECK(base == saved);
    }
    SUBCASE("query and name saved elsewhere are adopted unless the user edited them") {
        ViewDefinition draft = loaded;
        ViewDefinition base = loaded;
        ViewDefinition saved = loaded;
        saved.Jql = "status = Done"; // grid search box / view.update command
        saved.Name = "Renamed by command";
        ViewDraftRebaseResult r = RebaseViewDraft(draft, base, saved);
        CHECK(r.JqlAdopted);
        CHECK(r.NameAdopted);
        CHECK(draft.Jql == "status = Done");
        CHECK(draft.Name == "Renamed by command");

        draft.Jql = "my own edit";
        ViewDefinition saved2 = saved;
        saved2.Jql = "another external edit";
        r = RebaseViewDraft(draft, base, saved2);
        CHECK_FALSE(r.JqlAdopted);
        CHECK(draft.Jql == "my own edit");
        CHECK(ViewDraftHasUnsavedEdits(draft, saved2));
    }
    SUBCASE("an editor edit equal to the new saved value is simply clean") {
        ViewDefinition draft = loaded;
        ViewDefinition base = loaded;
        draft.Name = "Same";
        ViewDefinition saved = loaded;
        saved.Name = "Same";
        RebaseViewDraft(draft, base, saved);
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
    }
}

TEST_CASE("Save round-trip keeps column order") {
    SUBCASE("Save is identity: normalizing a view with non-alphabetical Fields and a complete "
            "column order round-trips the same key sequence — the direct regression guard for "
            "'I press Save and the columns reshuffle'") {
        ViewDefinition v;
        v.Fields = {"summary", "assignee", "priority", "status"}; // deliberately non-alphabetical
        v.Columns = MigrateLegacyColumns(
            v.Fields, {"id", "field:status", "field:summary", "field:assignee", "field:priority"}, {});
        NormalizeViewDefinition(v);
        const std::vector<ViewColumn> before = v.Columns;
        // Simulate a round-trip through Save (build -> commit -> reload) by normalizing again.
        NormalizeViewDefinition(v);
        CHECK(v.Columns == before);
        std::vector<std::string> keys;
        for (const auto& c : v.Columns)
            keys.push_back(c.Key);
        CHECK(keys ==
              std::vector<std::string>{"id", "field:status", "field:summary", "field:assignee", "field:priority"});
    }
}

TEST_CASE("CanRemoveViewColumn locks only the id column, like the Fields tab") {
    const ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}, {"field:status", 0.0f}});
    CHECK(CanRemoveViewColumn(v, "field:summary"));
    CHECK(CanRemoveViewColumn(v, " field:status "));
    CHECK_FALSE(CanRemoveViewColumn(v, "id"));
    CHECK_FALSE(CanRemoveViewColumn(v, "field:priority")); // not on this view
    CHECK_FALSE(CanRemoveViewColumn(v, "summary"));        // not a column key
    CHECK_FALSE(CanRemoveViewColumn(v, ""));

    SUBCASE("a field in the frozen first slot can go: the Fields tab locks only ID") {
        const ViewDefinition reordered = MakeView({{"field:summary", 0.0f}, {"id", 90.0f}, {"field:status", 0.0f}});
        CHECK(reordered.Columns.front().Key == "field:summary");
        CHECK(CanRemoveViewColumn(reordered, "field:summary"));
    }
}

TEST_CASE("RemoveViewColumn") {
    ViewDefinition v =
        MakeView({{"id", 90.0f}, {"field:status", 120.0f}, {"field:summary", 320.0f}, {"field:assignee", 200.0f}});
    ViewSortSpec onSummary;
    onSummary.ColumnKey = "field:summary";
    ViewSortSpec onAssignee;
    onAssignee.ColumnKey = "field:assignee";
    v.SortSpecs = {onSummary, onAssignee};

    SUBCASE("drops only that column; the others keep position and width") {
        CHECK(RemoveViewColumn("field:summary", v));
        REQUIRE(v.Columns.size() == 3);
        CHECK(v.Columns[0].Key == "id");
        CHECK(v.Columns[1].Key == "field:status");
        CHECK(v.Columns[1].Width == doctest::Approx(120.0f));
        CHECK(v.Columns[2].Key == "field:assignee");
        CHECK(v.Columns[2].Width == doctest::Approx(200.0f));
    }
    SUBCASE("Fields is regenerated and a sort rule on the column goes with it") {
        CHECK(RemoveViewColumn("field:summary", v));
        CHECK(v.Fields == std::vector<std::string>{"status", "assignee"});
        REQUIRE(v.SortSpecs.size() == 1);
        CHECK(v.SortSpecs[0].ColumnKey == "field:assignee");
    }
    SUBCASE("a refused key leaves the view untouched") {
        const ViewDefinition before = v;
        CHECK_FALSE(RemoveViewColumn("id", v));
        CHECK_FALSE(RemoveViewColumn("field:priority", v));
        CHECK(v == before);
    }
    SUBCASE("removing twice is a no-op the second time") {
        CHECK(RemoveViewColumn("field:summary", v));
        const ViewDefinition once = v;
        CHECK_FALSE(RemoveViewColumn("field:summary", v));
        CHECK(v == once);
    }
    SUBCASE("every field can go: the grid still has its id column") {
        CHECK(RemoveViewColumn("field:status", v));
        CHECK(RemoveViewColumn("field:summary", v));
        CHECK(RemoveViewColumn("field:assignee", v));
        REQUIRE(v.Columns.size() == 1);
        CHECK(v.Columns[0].Key == "id");
    }
}

TEST_CASE("RemoveViewColumn against the Views editor draft") {
    const ViewDefinition loaded =
        MakeView({{"id", 90.0f}, {"field:status", 120.0f}, {"field:summary", 320.0f}, {"field:assignee", 200.0f}});
    ViewDefinition saved = loaded;
    ViewDefinition draft = loaded;
    ViewDefinition base = loaded;

    SUBCASE("no phantom unsaved edits: draft and base lose the column along with the saved view") {
        RemoveViewColumn("field:summary", saved);
        RemoveViewColumn("field:summary", draft);
        RemoveViewColumn("field:summary", base);
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
        RebaseViewDraft(draft, base, saved);
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
    }
    SUBCASE("a draft the removal did not reach still adopts it on the next rebase") {
        RemoveViewColumn("field:summary", saved);
        RebaseViewDraft(draft, base, saved);
        CHECK_FALSE(ViewDraftHasUnsavedEdits(draft, saved));
    }
    SUBCASE("a pending column edit survives, and its Apply does not bring the column back") {
        draft.Columns.push_back({"field:priority", 0.0f}); // ticked in the Fields tab, not applied yet
        RemoveViewColumn("field:summary", saved);
        RemoveViewColumn("field:summary", draft);
        RemoveViewColumn("field:summary", base);
        RebaseViewDraft(draft, base, saved);
        CHECK(ViewDraftHasUnsavedEdits(draft, saved));

        const ViewDefinition merged = MergeDraftEditsOntoSaved(draft, saved);
        std::vector<std::string> keys;
        for (const auto& c : merged.Columns)
            keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:assignee", "field:priority"});
    }
}

// A switch from outside the Views dashboard (view.next / view.previous, bound to Ctrl+PageDown / Ctrl+PageUp)
// asks only when the editor holds an edit of the user's. It cannot rebase the live draft, so it judges a copy.
TEST_CASE("ViewDraftWouldLoseEditsOnSwitch") {
    const ViewDefinition saved = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}, {"field:status", 0.0f}});
    SUBCASE("an untouched draft loses nothing") {
        CHECK_FALSE(ViewDraftWouldLoseEditsOnSwitch(saved, saved, saved, false, ""));
    }
    SUBCASE("an edit the user made in the editor would be lost") {
        ViewDefinition draft = saved;
        draft.Name = "Renamed";
        CHECK(ViewDraftWouldLoseEditsOnSwitch(draft, saved, saved, false, ""));
        draft = saved;
        draft.Jql = "status = Open";
        CHECK(ViewDraftWouldLoseEditsOnSwitch(draft, saved, saved, false, ""));
        draft = saved;
        std::swap(draft.Columns[1], draft.Columns[2]);
        CHECK(ViewDraftWouldLoseEditsOnSwitch(draft, saved, saved, false, ""));
    }
    SUBCASE("a change made elsewhere since the draft was synced is not an edit of the user's") {
        // The grid autosaved a column drag and a rename came in through view.update: the draft is a stale
        // snapshot (draft == base), not an unsaved edit, so no question is asked.
        ViewDefinition changed = saved;
        std::swap(changed.Columns[1], changed.Columns[2]);
        changed.Name = "Renamed by a command";
        CHECK(ViewDraftHasUnsavedEdits(saved, changed)); // the raw compare would have read it as "unsaved"
        CHECK_FALSE(ViewDraftWouldLoseEditsOnSwitch(saved, saved, changed, false, ""));
    }
    SUBCASE("an edit by the user survives a change made elsewhere") {
        ViewDefinition draft = saved;
        draft.Jql = "assignee = me";
        ViewDefinition changed = saved;
        changed.Name = "Renamed by a command";
        CHECK(ViewDraftWouldLoseEditsOnSwitch(draft, saved, changed, false, ""));
    }
    SUBCASE("a rename still being typed counts: it has not reached the draft yet") {
        CHECK(ViewDraftWouldLoseEditsOnSwitch(saved, saved, saved, true, "New name"));
        // The title box opened and nothing was typed: the buffer holds the saved name.
        CHECK_FALSE(ViewDraftWouldLoseEditsOnSwitch(saved, saved, saved, true, saved.Name));
        // A closed title box's buffer is a display mirror and never counts.
        CHECK_FALSE(ViewDraftWouldLoseEditsOnSwitch(saved, saved, saved, false, "stale text"));
    }
    SUBCASE("the caller's draft and base are not touched") {
        ViewDefinition draft = saved;
        draft.Name = "Renamed";
        ViewDefinition base = saved;
        ViewDefinition changed = saved;
        changed.Jql = "x = 1";
        ViewDraftWouldLoseEditsOnSwitch(draft, base, changed, false, "");
        CHECK(draft.Name == "Renamed");
        CHECK(draft.Jql == saved.Jql);
        CHECK(base.Jql == saved.Jql);
    }
}

// ---- ViewPickerPure.h: the views sidebar search and the view-switch key hint -----------------------------
// (Lives here, not in its own TU, while tests/CMakeLists.txt is edited by open PRs: debt
// 2026-11-16-text-field-migration-blocked-surfaces.)

#include "ViewPickerPure.h"

namespace {

std::vector<std::size_t> ListedIndices(const std::vector<std::string>& names, const char* query) {
    smatchet::textmatch::Scratch scratch;
    const std::vector<smatchet::views::ListedView> rows = smatchet::views::ListViewsForSearch(
        names.size(), [&names](std::size_t i) -> const std::string& { return names[i]; }, query, scratch);
    std::vector<std::size_t> out;
    for (const smatchet::views::ListedView& r : rows) {
        out.push_back(r.Index);
    }
    return out;
}

} // namespace

TEST_CASE("ListViewsForSearch: nothing typed lists every view in store order, unmarked") {
    const std::vector<std::string> names = {"Sprint", "Backlog", "Bugs"};
    CHECK(ListedIndices(names, "") == std::vector<std::size_t>({0, 1, 2}));
    CHECK(ListedIndices(names, "   ") == std::vector<std::size_t>({0, 1, 2})); // whitespace makes no word
    CHECK(ListedIndices(names, nullptr) == std::vector<std::size_t>({0, 1, 2}));
    smatchet::textmatch::Scratch scratch;
    const auto rows = smatchet::views::ListViewsForSearch(
        names.size(), [&names](std::size_t i) -> const std::string& { return names[i]; }, "", scratch);
    for (const auto& r : rows) {
        CHECK(r.NameRanges.empty());
    }
    CHECK(ListedIndices({}, "bug").empty());
}

TEST_CASE("ListViewsForSearch: words in any order, case-folded; exact rows only when there is one") {
    const std::vector<std::string> names = {"Team bugs open", "Open bugs", "Release notes", "Bugfix sprint, open"};
    // "open bugs" occurs as written (in any word order) in rows 0 and 1, so row 3, where "bugs" only matches
    // letter by letter (b-u-g ... s), is left out.
    const auto exact = ListedIndices(names, "BUGS open");
    CHECK(exact.size() == 2);
    CHECK(std::find(exact.begin(), exact.end(), std::size_t{3}) == exact.end());
    CHECK(std::find(exact.begin(), exact.end(), std::size_t{2}) == exact.end());
    // No row holds "rls" as written: names holding its letters in order are listed.
    CHECK(ListedIndices(names, "rls") == std::vector<std::size_t>({2}));
    CHECK(ListedIndices(names, "zzz").empty());
    // With no exact row, the letters-in-order row is listed.
    CHECK(ListedIndices(names, "bugs sprint") == std::vector<std::size_t>({3}));
}

TEST_CASE("ListViewsForSearch: best match first, ties keep store order, ranges mark the name") {
    const std::vector<std::string> names = {"My open sprint", "Sprint", "Old sprint board"};
    const auto order = ListedIndices(names, "sprint");
    REQUIRE(order.size() == 3);
    CHECK(order.front() == 1); // the whole name typed ranks first: Enter opens it
    const std::vector<std::string> same = {"alpha one", "alpha two"};
    CHECK(ListedIndices(same, "alpha") == std::vector<std::size_t>({0, 1}));

    smatchet::textmatch::Scratch scratch;
    const auto rows = smatchet::views::ListViewsForSearch(
        names.size(), [&names](std::size_t i) -> const std::string& { return names[i]; }, "sprint", scratch);
    REQUIRE(!rows.empty());
    REQUIRE(rows.front().NameRanges.size() == 1);
    CHECK(rows.front().NameRanges[0].Begin == 0);
    CHECK(rows.front().NameRanges[0].End == 6);
}

TEST_CASE("ListViewsForSearch: reused buffers give each frame's list, with no rows or marks left from before") {
    const std::vector<std::string> names = {"My open sprint", "Sprint", "Old sprint board", "Backlog"};
    const auto nameAt = [&names](std::size_t i) -> const std::string& { return names[i]; };
    smatchet::textmatch::Scratch scratch;
    smatchet::views::ViewListBuffers buf;

    smatchet::views::ListViewsForSearch(names.size(), nameAt, "sprint", scratch, buf);
    REQUIRE(buf.Listed.size() == 3);
    CHECK(buf.Listed.front().Index == 1);

    // A shorter list: no row left over from the longer one, and each row's marks are its own.
    smatchet::views::ListViewsForSearch(names.size(), nameAt, "backlog", scratch, buf);
    REQUIRE(buf.Listed.size() == 1);
    CHECK(buf.Listed[0].Index == 3);
    REQUIRE(buf.Listed[0].NameRanges.size() == 1);
    CHECK(buf.Listed[0].NameRanges[0].Begin == 0);
    CHECK(buf.Listed[0].NameRanges[0].End == 7);

    // Nothing typed: every view, unmarked, although the rows held marks a frame ago.
    smatchet::views::ListViewsForSearch(names.size(), nameAt, "sprint", scratch, buf);
    smatchet::views::ListViewsForSearch(names.size(), nameAt, "", scratch, buf);
    REQUIRE(buf.Listed.size() == 4);
    for (std::size_t k = 0; k < buf.Listed.size(); ++k) {
        CHECK(buf.Listed[k].Index == k);
        CHECK(buf.Listed[k].NameRanges.empty());
    }

    // The same answer as a fresh call.
    smatchet::views::ListViewsForSearch(names.size(), nameAt, "old board", scratch, buf);
    const auto fresh = smatchet::views::ListViewsForSearch(names.size(), nameAt, "old board", scratch);
    REQUIRE(buf.Listed.size() == fresh.size());
    for (std::size_t k = 0; k < fresh.size(); ++k) {
        CHECK(buf.Listed[k].Index == fresh[k].Index);
        REQUIRE(buf.Listed[k].NameRanges.size() == fresh[k].NameRanges.size());
        for (std::size_t r = 0; r < fresh[k].NameRanges.size(); ++r) {
            CHECK(buf.Listed[k].NameRanges[r].Begin == fresh[k].NameRanges[r].Begin);
            CHECK(buf.Listed[k].NameRanges[r].End == fresh[k].NameRanges[r].End);
        }
    }

    // A smaller store after a larger one (a view deleted): only its own views are listed.
    const std::vector<std::string> fewer = {"Backlog", "Sprint"};
    const auto fewerAt = [&fewer](std::size_t i) -> const std::string& { return fewer[i]; };
    smatchet::views::ListViewsForSearch(fewer.size(), fewerAt, "sprint", scratch, buf);
    REQUIRE(buf.Listed.size() == 1);
    CHECK(buf.Listed[0].Index == 1);
    REQUIRE(buf.Listed[0].NameRanges.size() == 1);
    CHECK(buf.Listed[0].NameRanges[0].Begin == 0);
    CHECK(buf.Listed[0].NameRanges[0].End == 6);
    smatchet::views::ListViewsForSearch(fewer.size(), fewerAt, "", scratch, buf);
    CHECK(buf.Listed.size() == 2);
}

TEST_CASE("ViewSwitchKeysTooltip names the bound view keys, read live") {
    KeybindingsConfig cfg = KeybindingsConfig::Defaults();
    const std::string tip =
        smatchet::views::ViewSwitchKeysTooltip("Lead.", cfg.Bindings, "Next view:", "Previous view:");
    CHECK(tip == "Lead.\nNext view: Ctrl+PageDown\nPrevious view: Ctrl+PageUp");

    // A rebind shows at once; an unbound or disabled command adds no line.
    const int next = cfg.FindBindingIndex("view.next", "{}");
    const int prev = cfg.FindBindingIndex("view.previous", "{}");
    REQUIRE(next >= 0);
    REQUIRE(prev >= 0);
    cfg.Bindings[static_cast<std::size_t>(next)].Hotkeys = {"Alt+Right"};
    cfg.Bindings[static_cast<std::size_t>(prev)].Enabled = false;
    CHECK(smatchet::views::ViewSwitchKeysTooltip("Lead.", cfg.Bindings, "N:", "P:") == "Lead.\nN: Alt+Right");
    CHECK(smatchet::views::ViewSwitchKeysTooltip("Lead.", {}, "N:", "P:") == "Lead.");
}
