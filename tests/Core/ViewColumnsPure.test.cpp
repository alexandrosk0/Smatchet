// Bucket-A doctest for ViewColumnsPure.h — the ordered-column-list model that replaces
// ViewDefinition::ColumnOrder + ColumnWidths + a separately-mutable Fields
// (column-view-save-simplification). No ImGui, no session state.

#include "ViewColumnsPure.h"

#include <doctest/doctest.h>

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
        for (const auto& c : columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:summary", "field:status", "field:priority"});
    }
    SUBCASE("complete ColumnOrder — that order wins outright") {
        const auto columns = MigrateLegacyColumns({"summary", "status"}, {"id", "field:status", "field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:summary"});
    }
    SUBCASE("short ColumnOrder — a field added elsewhere is appended in Fields order, not dropped") {
        const auto columns = MigrateLegacyColumns({"summary", "status", "assignee"}, {"id", "field:status"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:summary", "field:assignee"});
    }
    SUBCASE("ColumnOrder missing id — id is not silently invented by this pass, it is what "
            "MigrateLegacyColumns is given, and 'id' only comes from Fields' own allKeys seed") {
        const auto columns = MigrateLegacyColumns({"summary"}, {"field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns) keys.push_back(c.Key);
        // "id" is always in allKeys (MigrateLegacyColumns seeds it unconditionally) and was not
        // consumed by columnOrder, so it lands in the tail-append.
        CHECK(keys == std::vector<std::string>{"field:summary", "id"});
    }
    SUBCASE("stale ColumnOrder key naming a field that no longer exists is dropped") {
        const auto columns = MigrateLegacyColumns({"summary"}, {"id", "field:gone", "field:summary"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:summary"});
    }
    SUBCASE("legacy comment alias folds onto comments, deduped") {
        const auto columns = MigrateLegacyColumns({"comment"}, {"id", "field:comments"}, {});
        std::vector<std::string> keys;
        for (const auto& c : columns) keys.push_back(c.Key);
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
        for (const auto& c : v.Columns) keys.push_back(c.Key);
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
        for (const auto& c : v.Columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"field:status", "id", "field:summary"});
    }
    SUBCASE("an unknown key is reported in ignored, not silently dropped or invented") {
        ViewDefinition v = MakeView({{"id", 90.0f}, {"field:summary", 0.0f}});
        const auto ignored = ReorderViewColumns({"field:typo", "id", "field:summary"}, v);
        REQUIRE(ignored.size() == 1);
        CHECK(ignored.front() == "field:typo");
        std::vector<std::string> keys;
        for (const auto& c : v.Columns) keys.push_back(c.Key);
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
        saved.Columns[1].Width = 250.0f;                                                       // grid resize

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
        v.Columns = MigrateLegacyColumns(v.Fields, {"id", "field:status", "field:summary", "field:assignee",
                                                    "field:priority"}, {});
        NormalizeViewDefinition(v);
        const std::vector<ViewColumn> before = v.Columns;
        // Simulate a round-trip through Save (build -> commit -> reload) by normalizing again.
        NormalizeViewDefinition(v);
        CHECK(v.Columns == before);
        std::vector<std::string> keys;
        for (const auto& c : v.Columns) keys.push_back(c.Key);
        CHECK(keys == std::vector<std::string>{"id", "field:status", "field:summary", "field:assignee",
                                               "field:priority"});
    }
}
