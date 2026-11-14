#include <doctest/doctest.h>

#include "FilterDropdownPure.h"

#include <string>
#include <vector>

using namespace smatchet::dropdown::pure;

namespace {

ItemView MakeItem(const char* id, const char* label, const char* secondary = nullptr, bool checked = false,
                  bool disabled = false) {
    ItemView v;
    v.Id = id;
    v.Label = label;
    v.Secondary = secondary;
    v.Checked = checked;
    v.Disabled = disabled;
    return v;
}

// Twelve options ("opt0".."opt11"): more than the default RecentsMinOptions of 10.
struct TwelveOptions {
    std::vector<std::string> Ids;
    std::vector<std::string> Labels;
    std::vector<ItemView> Views;
    TwelveOptions() {
        for (int i = 0; i < 12; ++i) {
            Ids.push_back("opt" + std::to_string(i));
            Labels.push_back("Option " + std::to_string(i));
        }
        for (int i = 0; i < 12; ++i) {
            Views.push_back(
                MakeItem(Ids[static_cast<std::size_t>(i)].c_str(), Labels[static_cast<std::size_t>(i)].c_str()));
        }
    }
};

std::string Shape(const std::vector<Row>& rows) {
    std::string out;
    for (const Row& row : rows) {
        switch (row.Kind) {
        case RowKind::HeaderRecent:
            out += 'R';
            break;
        case RowKind::HeaderAll:
            out += 'A';
            break;
        case RowKind::Clear:
            out += 'x';
            break;
        case RowKind::Create:
            out += '+';
            break;
        case RowKind::Item:
            out += row.RecentIndex >= 0 ? 'r' : 'i';
            break;
        }
    }
    return out;
}

} // namespace

TEST_CASE("FindCaseInsensitive: offset of the first match, ASCII case ignored") {
    CHECK(FindCaseInsensitive("In Progress", "progress") == 3);
    CHECK(FindCaseInsensitive("In Progress", "in") == 0);
    CHECK(FindCaseInsensitive("In Progress", "") == 0);
    CHECK(FindCaseInsensitive("In Progress", "done") == std::string::npos);
    CHECK(FindCaseInsensitive("ab", "abc") == std::string::npos);
    CHECK(FindCaseInsensitive(nullptr, "a") == std::string::npos);
    CHECK(FindCaseInsensitive(nullptr, "") == 0);
}

TEST_CASE("NormalizeFilter / TrimFilter: trim both ends; Normalize also lower-cases") {
    CHECK(NormalizeFilter("  Hello World \t") == "hello world");
    CHECK(TrimFilter("  Hello World \t") == "Hello World");
    CHECK(NormalizeFilter("   ").empty());
    CHECK(NormalizeFilter(nullptr).empty());
    CHECK(TrimFilter(nullptr).empty());
}

TEST_CASE("ItemMatches: label, secondary text or id; empty filter matches all") {
    const ItemView alice = MakeItem("acc-1", "Alice Smith", "alice@example.com");
    CHECK(ItemMatches(alice, ""));
    CHECK(ItemMatches(alice, "smith"));
    CHECK(ItemMatches(alice, "example.com"));
    CHECK(ItemMatches(alice, "acc-"));
    CHECK_FALSE(ItemMatches(alice, "bob"));
    CHECK_FALSE(ItemMatches(MakeItem("1", "Bug"), "x"));
}

TEST_CASE("BuildRows: no filter, no recents -> every option in order") {
    TwelveOptions opts;
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "iiiiiiiiiiii");
    CHECK(rows[0].ItemIndex == 0);
    CHECK(rows[11].ItemIndex == 11);
}

TEST_CASE("BuildRows: Recent section on top, newest first, options repeated below under All") {
    TwelveOptions opts;
    const std::vector<std::string> recents = {"opt7", "opt2"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrrAiiiiiiiiiiii");
    CHECK(rows[1].ItemIndex == 7);
    CHECK(rows[1].RecentIndex == 0);
    CHECK(rows[2].ItemIndex == 2);
    CHECK(rows[2].RecentIndex == 1);
    CHECK(rows[4].ItemIndex == 0); // the full list still holds every option, recent ones included
    CHECK(rows[4].RecentIndex == -1);
}

TEST_CASE("BuildRows: Recent section needs MORE options than RecentsMinOptions") {
    TwelveOptions opts;
    const std::vector<std::string> recents = {"opt1"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Recents = &recents;
    std::vector<Row> rows;

    in.Count = 10; // exactly the threshold: no section
    BuildRows(in, rows);
    CHECK(Shape(rows) == "iiiiiiiiii");

    in.Count = 11;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrAiiiiiiiiiii");

    in.Count = 3;
    in.RecentsMinOptions = 0; // a caller that always wants the section
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrAiii");
}

TEST_CASE("BuildRows: a recent id that is not an option is hidden, unless RecentsShowUnknown") {
    TwelveOptions opts;
    const std::vector<std::string> recents = {"gone", "opt3"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrAiiiiiiiiiiii");
    CHECK(rows[1].ItemIndex == 3);
    CHECK(rows[1].RecentIndex == 1);

    in.RecentsShowUnknown = true;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrrAiiiiiiiiiiii");
    CHECK(rows[1].ItemIndex == -1);
    CHECK(rows[1].RecentIndex == 0);

    const std::vector<std::string> onlyGone = {"gone"};
    in.Recents = &onlyGone;
    in.RecentsShowUnknown = false;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "iiiiiiiiiiii"); // nothing resolves: no empty section
}

TEST_CASE("BuildRows: an empty option list still shows recents with RecentsMinOptions -1 + RecentsShowUnknown") {
    // The project picker before its list has loaded: the last picks stay reachable offline.
    const std::vector<std::string> recents = {"PROJ", "OPS"};
    ModelInput in;
    in.Items = nullptr;
    in.Count = 0;
    in.Recents = &recents;
    in.RecentsShowUnknown = true;
    std::vector<Row> rows;
    in.RecentsMinOptions = 0; // "more than 0 options" fails on an empty list
    BuildRows(in, rows);
    CHECK(rows.empty());
    in.RecentsMinOptions = -1;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "RrrA");
    CHECK(rows[1].ItemIndex == -1);
    CHECK(rows[1].RecentIndex == 0);
    CHECK(rows[2].RecentIndex == 1);
    CHECK(IsSelectableRow(rows[1], nullptr, 0));
    CHECK(MoveHighlight(rows, nullptr, 0, -1, 1) == 1);
}

TEST_CASE("BuildRows: typing a filter hides the Recent section and the clear row") {
    TwelveOptions opts;
    const std::vector<std::string> recents = {"opt7", "opt2"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    in.ShowClear = true;
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "xRrrAiiiiiiiiiiii");

    in.FilterLower = "option 1"; // Option 1, Option 10, Option 11
    BuildRows(in, rows);
    CHECK(Shape(rows) == "iii");
    CHECK(rows[0].ItemIndex == 1);
    CHECK(rows[1].ItemIndex == 10);
    CHECK(rows[2].ItemIndex == 11);
}

TEST_CASE("BuildRows: KeepCheckedVisible keeps ticked options a filter would hide") {
    std::vector<ItemView> items = {MakeItem("a", "Alpha", nullptr, true), MakeItem("b", "Beta"),
                                   MakeItem("g", "Gamma")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 3;
    in.FilterLower = "gam";
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(Shape(rows) == "i");
    in.KeepCheckedVisible = true;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "ii");
    CHECK(rows[0].ItemIndex == 0);
    CHECK(rows[1].ItemIndex == 2);
}

TEST_CASE("BuildRows: the create row is offered only for text no option is named") {
    std::vector<ItemView> items = {MakeItem("bug", "bug"), MakeItem("ui", "UI")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 2;
    in.AllowCreate = true;
    std::vector<Row> rows;

    BuildRows(in, rows); // no filter: nothing to create
    CHECK(Shape(rows) == "ii");

    in.FilterLower = "u"; // matches "bug" and "UI", equals neither
    BuildRows(in, rows);
    CHECK(Shape(rows) == "ii+");

    in.FilterLower = "ui"; // equals "UI" ignoring case
    BuildRows(in, rows);
    CHECK(Shape(rows) == "i");

    in.FilterLower = "backend"; // matches nothing: the create row alone
    BuildRows(in, rows);
    CHECK(Shape(rows) == "+");

    in.AllowCreate = false;
    BuildRows(in, rows);
    CHECK(rows.empty());
}

TEST_CASE("MoveHighlight: steps over headers and disabled options, stops at the ends") {
    TwelveOptions opts;
    opts.Views[1].Disabled = true;
    const std::vector<std::string> recents = {"opt7"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    std::vector<Row> rows;
    BuildRows(in, rows); // R r A i(0) i(1,disabled) i(2) ...
    REQUIRE(Shape(rows) == "RrAiiiiiiiiiiii");
    const ItemView* items = opts.Views.data();

    CHECK(MoveHighlight(rows, items, 12, -1, 1) == 1);   // nothing highlighted: Down lands on the first row
    CHECK(MoveHighlight(rows, items, 12, -1, -1) == 14); // Up lands on the last
    CHECK(MoveHighlight(rows, items, 12, 1, 1) == 3);    // over the All header
    CHECK(MoveHighlight(rows, items, 12, 3, 1) == 5);    // over the disabled option
    CHECK(MoveHighlight(rows, items, 12, 5, -1) == 3);
    CHECK(MoveHighlight(rows, items, 12, 3, -1) == 1);
    CHECK(MoveHighlight(rows, items, 12, 1, -1) == 1);    // top: stays
    CHECK(MoveHighlight(rows, items, 12, 14, 1) == 14);   // bottom: stays
    CHECK(MoveHighlight(rows, items, 12, 1, 100) == 14);  // a page past the end clamps
    CHECK(MoveHighlight(rows, items, 12, 14, -100) == 1); // and past the start
    CHECK(MoveHighlight(rows, items, 12, 0, 1) == 1);     // from a header: the next selectable row
}

TEST_CASE("MoveHighlight: no selectable row -> -1") {
    std::vector<ItemView> items = {MakeItem("a", "A", nullptr, false, true)};
    ModelInput in;
    in.Items = items.data();
    in.Count = 1;
    std::vector<Row> rows;
    BuildRows(in, rows);
    CHECK(MoveHighlight(rows, items.data(), 1, -1, 1) == -1);
    CHECK(MoveHighlight(std::vector<Row>(), nullptr, 0, -1, 1) == -1);
}

TEST_CASE("DefaultHighlight: the current value in the full list, else the first option") {
    TwelveOptions opts;
    opts.Views[7].Checked = true;
    const std::vector<std::string> recents = {"opt7", "opt2"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    in.ShowClear = true;
    std::vector<Row> rows;
    BuildRows(in, rows); // x R r(7) r(2) A i(0) ... i(7) ...
    REQUIRE(Shape(rows) == "xRrrAiiiiiiiiiiii");
    const ItemView* items = opts.Views.data();

    CHECK(DefaultHighlight(rows, items, 12, true, "") == 12); // option 7 in the full list, not its Recent copy
    CHECK(DefaultHighlight(rows, items, 12, false, "") == 2); // first option row: past the clear row and header

    in.FilterLower = "option 1";
    BuildRows(in, rows);
    CHECK(DefaultHighlight(rows, items, 12, false, in.FilterLower) == 0); // Enter picks the top match
}

TEST_CASE("DefaultHighlight: a create row only when no option matches the filter") {
    std::vector<ItemView> items = {MakeItem("bug", "bug")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 1;
    in.AllowCreate = true;
    in.FilterLower = "bu";
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "i+");
    CHECK(DefaultHighlight(rows, items.data(), 1, false, in.FilterLower) == 0);

    in.FilterLower = "zzz";
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "+");
    CHECK(DefaultHighlight(rows, items.data(), 1, false, in.FilterLower) == 0);

    CHECK(DefaultHighlight(std::vector<Row>(), nullptr, 0, true, "") == -1);
}

TEST_CASE("DefaultHighlight: a ticked option kept visible by KeepCheckedVisible never takes Enter from the match") {
    // "Backend" is ticked and sorts first; the user types "front". Enter must tick Frontend, not untick Backend.
    std::vector<ItemView> items = {MakeItem("be", "Backend", nullptr, true), MakeItem("fe", "Frontend")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 2;
    in.KeepCheckedVisible = true;
    in.FilterLower = "front";
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "ii");
    CHECK(DefaultHighlight(rows, items.data(), 2, false, in.FilterLower) == 1);

    // No option matches: the create row takes Enter, not the kept ticked option.
    in.AllowCreate = true;
    in.FilterLower = "new-label";
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "i+");
    CHECK(DefaultHighlight(rows, items.data(), 2, false, in.FilterLower) == 1);

    // No match and no create row: the kept option is all there is.
    in.AllowCreate = false;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "i");
    CHECK(DefaultHighlight(rows, items.data(), 2, false, in.FilterLower) == 0);
}

TEST_CASE("DefaultHighlight: a label match outranks an id or secondary-text match") {
    // Typing "ab" for Abby: an opaque id, an e-mail and a mid-word hit all contain "ab" and sort first.
    std::vector<ItemView> items = {MakeItem("5ab01", "Zed"), MakeItem("u2", "Carol", "abby.fan@example.com"),
                                   MakeItem("u3", "Crabby"), MakeItem("u4", "Abby"), MakeItem("u5", "Abel")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 5;
    in.FilterLower = "ab";
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "iiiii");
    CHECK(MatchRank(items[0], "ab") == 2);
    CHECK(MatchRank(items[1], "ab") == 2);
    CHECK(MatchRank(items[2], "ab") == 1);
    CHECK(MatchRank(items[3], "ab") == 0);
    CHECK(DefaultHighlight(rows, items.data(), 5, false, in.FilterLower) == 3); // Abby, the first prefix match

    in.Count = 3; // no label starts with it: the label that contains it
    BuildRows(in, rows);
    CHECK(DefaultHighlight(rows, items.data(), 3, false, in.FilterLower) == 2);

    in.Count = 2; // only id / secondary matches: the first of them
    BuildRows(in, rows);
    CHECK(DefaultHighlight(rows, items.data(), 2, false, in.FilterLower) == 0);
}

TEST_CASE("DefaultHighlight: never the clear row") {
    // A picker whose options have not arrived shows only its clear row: Enter must not clear the field.
    ModelInput in;
    in.ShowClear = true;
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "x");
    CHECK(DefaultHighlight(rows, nullptr, 0, true, "") == -1);
    CHECK(DefaultHighlight(rows, nullptr, 0, false, "") == -1);
    CHECK(MoveHighlight(rows, nullptr, 0, -1, 1) == 0); // still reachable with the arrow keys
}

TEST_CASE("HasOptionRows: a lone clear row is an empty list") {
    std::vector<ItemView> items = {MakeItem("bug", "bug")};
    ModelInput in;
    in.ShowClear = true;
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "x");
    CHECK_FALSE(HasOptionRows(rows));
    CHECK_FALSE(HasOptionRows(std::vector<Row>()));

    in.Items = items.data();
    in.Count = 1;
    BuildRows(in, rows);
    CHECK(HasOptionRows(rows));

    in.AllowCreate = true;
    in.FilterLower = "zzz";
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "+");
    CHECK(HasOptionRows(rows));
}

TEST_CASE("RowIdentity: the highlight follows its row when the list is rebuilt") {
    TwelveOptions opts;
    const std::vector<std::string> recents = {"opt7"};
    ModelInput in;
    in.Items = opts.Views.data();
    in.Count = 12;
    in.Recents = &recents;
    in.ShowClear = true;
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "xRrAiiiiiiiiiiii");
    const ItemView* items = opts.Views.data();

    RowIdentity fullListCopy;
    IdentityOfRow(rows, items, 12, &recents, 11, fullListCopy); // option 7 in the full list
    REQUIRE(fullListCopy.Valid);
    CHECK(fullListCopy.Id == "opt7");
    CHECK_FALSE(fullListCopy.InRecent);
    RowIdentity recentCopy;
    IdentityOfRow(rows, items, 12, &recents, 2, recentCopy);
    CHECK(recentCopy.InRecent);
    RowIdentity clearRow;
    IdentityOfRow(rows, items, 12, &recents, 0, clearRow);
    CHECK(clearRow.Kind == RowKind::Clear);
    RowIdentity none;
    IdentityOfRow(rows, items, 12, &recents, -1, none);
    CHECK_FALSE(none.Valid);
    CHECK(FindRowByIdentity(rows, items, 12, &recents, none) == -1);

    // The list is rebuilt shorter and in another order: options 7, 3, 1 only.
    std::vector<ItemView> fewer = {opts.Views[7], opts.Views[3], opts.Views[1]};
    in.Items = fewer.data();
    in.Count = 3;
    BuildRows(in, rows); // 3 options: no Recent section
    REQUIRE(Shape(rows) == "xiii");
    CHECK(FindRowByIdentity(rows, fewer.data(), 3, &recents, fullListCopy) == 1); // followed option 7
    CHECK(FindRowByIdentity(rows, fewer.data(), 3, &recents, recentCopy) == -1);  // its Recent copy is gone
    CHECK(FindRowByIdentity(rows, fewer.data(), 3, &recents, clearRow) == 0);

    RowIdentity gone;
    gone.Valid = true;
    gone.Id = "opt9";
    CHECK(FindRowByIdentity(rows, fewer.data(), 3, &recents, gone) == -1);
}

TEST_CASE("ResolveHighlight: stays on its own row, else follows the identity, else gives up") {
    // Two options share an id: a highlight on the second must not jump back to the first.
    std::vector<ItemView> items = {MakeItem("dup", "First"), MakeItem("dup", "Second"), MakeItem("c", "Third")};
    ModelInput in;
    in.Items = items.data();
    in.Count = 3;
    std::vector<Row> rows;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "iii");
    RowIdentity second;
    IdentityOfRow(rows, items.data(), 3, nullptr, 1, second);
    RowIdentity third;
    IdentityOfRow(rows, items.data(), 3, nullptr, 2, third);
    CHECK(FindRowByIdentity(rows, items.data(), 3, nullptr, second) == 1);
    CHECK(ResolveHighlight(rows, items.data(), 3, nullptr, second, 1) == 1);

    // An option arrives in front: the highlight follows "Third" one row down.
    std::vector<ItemView> more = {MakeItem("a", "Zero"), items[0], items[1], items[2]};
    in.Items = more.data();
    in.Count = 4;
    BuildRows(in, rows);
    REQUIRE(Shape(rows) == "iiii");
    CHECK(ResolveHighlight(rows, more.data(), 4, nullptr, third, 2) == 3);
    // Row 1 now holds "First", which shares the id: the label tells them apart.
    CHECK(ResolveHighlight(rows, more.data(), 4, nullptr, second, 1) == 2);

    RowIdentity gone;
    gone.Valid = true;
    gone.Id = "zzz";
    gone.Label = "Gone";
    CHECK(ResolveHighlight(rows, more.data(), 4, nullptr, gone, 2) == -1);
    CHECK(ResolveHighlight(rows, more.data(), 4, nullptr, RowIdentity(), 2) == -1);
    CHECK(ResolveHighlight(rows, more.data(), 4, nullptr, third, 99) == 3); // a stale index is not trusted
}

TEST_CASE("ScrollToRevealRow: scrolls only as far as needed") {
    // Rows of 20 in a 100-tall view scrolled to 40: rows 2..6 are fully visible.
    CHECK(ScrollToRevealRow(3, 20.0f, 100.0f, 40.0f) == doctest::Approx(40.0f)); // visible: unchanged
    CHECK(ScrollToRevealRow(1, 20.0f, 100.0f, 40.0f) == doctest::Approx(20.0f)); // above: its top
    CHECK(ScrollToRevealRow(7, 20.0f, 100.0f, 40.0f) == doctest::Approx(60.0f)); // below: its bottom at the edge
    CHECK(ScrollToRevealRow(-1, 20.0f, 100.0f, 40.0f) == doctest::Approx(40.0f));
    CHECK(ScrollToRevealRow(3, 0.0f, 100.0f, 40.0f) == doctest::Approx(40.0f));
}
