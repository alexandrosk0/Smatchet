#include <doctest/doctest.h>

#include "TrackerFieldValueParser.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

TEST_CASE("BuildTrackerOptionDisplayValue handles null + primitive") {
    CHECK(BuildTrackerOptionDisplayValue(nlohmann::json(nullptr)) == "");
    CHECK(BuildTrackerOptionDisplayValue(nlohmann::json("hello")) == "hello");
    CHECK(BuildTrackerOptionDisplayValue(nlohmann::json(42)) == "42");
    CHECK(BuildTrackerOptionDisplayValue(nlohmann::json(true)) == "true");
}

TEST_CASE("BuildTrackerOptionDisplayValue prefers issue key + summary") {
    nlohmann::json issue;
    issue["key"] = "PROJ-7";
    issue["fields"]["summary"] = "Hello world";
    CHECK(BuildTrackerOptionDisplayValue(issue) == "PROJ-7 - Hello world");
}

TEST_CASE("BuildTrackerOptionDisplayValue falls back through preferred label keys") {
    nlohmann::json o;
    o["displayName"] = "Alice";
    o["value"] = "ignored";
    CHECK(BuildTrackerOptionDisplayValue(o) == "Alice");

    nlohmann::json o2;
    o2["value"] = "v";
    o2["name"] = "n";
    CHECK(BuildTrackerOptionDisplayValue(o2) == "v");

    nlohmann::json o3;
    o3["label"] = "L";
    CHECK(BuildTrackerOptionDisplayValue(o3) == "L");

    nlohmann::json o4;
    o4["id"] = "123";
    CHECK(BuildTrackerOptionDisplayValue(o4) == "123");
}

TEST_CASE("BuildTrackerOptionId prefers id over accountId / value / name") {
    nlohmann::json o;
    o["id"] = "10042";
    o["accountId"] = "abc";
    o["value"] = "Bug";
    CHECK(BuildTrackerOptionId(o) == "10042");

    nlohmann::json o2;
    o2["accountId"] = "abc";
    o2["value"] = "Alice";
    CHECK(BuildTrackerOptionId(o2) == "abc");

    nlohmann::json o3;
    o3["value"] = "X";
    CHECK(BuildTrackerOptionId(o3) == "X");

    nlohmann::json o4;
    o4["name"] = "fallback";
    CHECK(BuildTrackerOptionId(o4) == "fallback");
}

TEST_CASE("BuildTrackerOptionId handles primitives and null") {
    CHECK(BuildTrackerOptionId(nlohmann::json(nullptr)) == "");
    CHECK(BuildTrackerOptionId(nlohmann::json("plain")) == "plain");
    CHECK(BuildTrackerOptionId(nlohmann::json(7)) == "7");
}

TEST_CASE("TrackerFieldOptionFromJson populates id, value, description, payload, children") {
    nlohmann::json o;
    o["id"] = "10001";
    o["value"] = "Bug";
    o["description"] = "A defect";
    o["disabled"] = false;
    nlohmann::json child;
    child["id"] = "20001";
    child["value"] = "Critical";
    o["children"] = nlohmann::json::array({child});

    TrackerFieldOption opt = TrackerFieldOptionFromJson(o);
    CHECK(opt.Id == "10001");
    CHECK(opt.Value == "Bug");
    CHECK(opt.SecondaryValue == "A defect");
    CHECK(opt.Disabled == false);
    REQUIRE(opt.Children.size() == 1);
    CHECK(opt.Children[0].Id == "20001");
    CHECK(opt.Children[0].Value == "Critical");
    // Payload preserves the original JSON for downstream re-emission.
    CHECK_FALSE(opt.PayloadJson.empty());
}

TEST_CASE("TrackerFieldOptionFromJson reads disabled flag") {
    nlohmann::json o;
    o["id"] = "10001";
    o["value"] = "Old";
    o["disabled"] = true;
    TrackerFieldOption opt = TrackerFieldOptionFromJson(o);
    CHECK(opt.Disabled == true);
}

TEST_CASE("TrackerFieldOptionFromJson defaults Value to Id when value missing") {
    nlohmann::json o;
    o["id"] = "10042";
    TrackerFieldOption opt = TrackerFieldOptionFromJson(o);
    CHECK(opt.Id == "10042");
    CHECK(opt.Value == "10042");
}

TEST_CASE("TrackerFieldValueParser: MergeTrackerFieldOption_DoesNotDuplicateById [high-risk]") {
    // Guard: catalog merges from multiple Jira projects must dedup by id (lowercase). A regression
    // here would balloon allowed-value lists with near-duplicates that disagree on metadata.
    std::vector<TrackerFieldOption> target;
    TrackerFieldOption a;
    a.Id = "ABC-1";
    a.Value = "Bug";
    MergeTrackerFieldOption(target, a);
    REQUIRE(target.size() == 1);

    TrackerFieldOption a_again;
    a_again.Id = "ABC-1";
    a_again.Value = "Bug";
    a_again.SecondaryValue = "A defect";
    MergeTrackerFieldOption(target, a_again);
    REQUIRE(target.size() == 1);
    CHECK(target[0].SecondaryValue == "A defect");

    // Case-insensitive id dedup: "abc-1" must collapse onto "ABC-1" because the merge key
    // is computed via ToLowerAsciiCopy on the id.
    TrackerFieldOption a_case;
    a_case.Id = "abc-1";
    a_case.Value = "BUG-DUP";
    MergeTrackerFieldOption(target, a_case);
    REQUIRE(target.size() == 1);
    CHECK(target[0].Id == "ABC-1"); // first-write wins; case-different incoming did not push a new row
}

TEST_CASE("MergeTrackerFieldOption dedups by value when id is empty") {
    std::vector<TrackerFieldOption> target;
    TrackerFieldOption a;
    a.Value = "Same";
    MergeTrackerFieldOption(target, a);
    TrackerFieldOption a_lower;
    a_lower.Value = "same"; // case-insensitive value match
    MergeTrackerFieldOption(target, a_lower);
    CHECK(target.size() == 1);
}

TEST_CASE("MergeTrackerFieldOption recursively merges children") {
    std::vector<TrackerFieldOption> target;
    TrackerFieldOption parent;
    parent.Id = "p1";
    parent.Value = "Parent";
    TrackerFieldOption child1;
    child1.Id = "c1";
    child1.Value = "C1";
    parent.Children.push_back(child1);
    MergeTrackerFieldOption(target, parent);

    TrackerFieldOption parent2;
    parent2.Id = "p1";
    TrackerFieldOption child2;
    child2.Id = "c2";
    child2.Value = "C2";
    parent2.Children.push_back(child2);
    MergeTrackerFieldOption(target, parent2);

    REQUIRE(target.size() == 1);
    CHECK(target[0].Children.size() == 2);
}

TEST_CASE("MergeTrackerFieldOption propagates Disabled via OR") {
    std::vector<TrackerFieldOption> target;
    TrackerFieldOption a;
    a.Id = "x";
    a.Disabled = false;
    MergeTrackerFieldOption(target, a);
    TrackerFieldOption a_disabled;
    a_disabled.Id = "x";
    a_disabled.Disabled = true;
    MergeTrackerFieldOption(target, a_disabled);
    REQUIRE(target.size() == 1);
    CHECK(target[0].Disabled == true);
}

TEST_CASE("RefreshTrackerAllowedValuesFromOptions copies non-empty values") {
    TrackerField f;
    TrackerFieldOption o1;
    o1.Value = "A";
    TrackerFieldOption o2;
    o2.Value = ""; // skipped
    TrackerFieldOption o3;
    o3.Value = "B";
    f.AllowedValueOptions = {o1, o2, o3};
    RefreshTrackerAllowedValuesFromOptions(f);
    REQUIRE(f.AllowedValues.size() == 2);
    CHECK(f.AllowedValues[0] == "A");
    CHECK(f.AllowedValues[1] == "B");
}

TEST_CASE("ClassifyTrackerFieldFamily recognises labels / status / issuetype") {
    TrackerField f;
    f.Id = "labels";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Labels);
    f.Id = "status";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Status);
    f.Id = "issuetype";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::IssueType);
}

TEST_CASE("ClassifyTrackerFieldFamily recognises sprint via custom schema") {
    TrackerField f;
    f.Id = "customfield_10020";
    f.SchemaCustom = "com.pyxis.greenhopper.jira:gh-sprint";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Sprint);
}

TEST_CASE("ClassifyTrackerFieldFamily recognises Date / DateTime / Number") {
    TrackerField f;
    f.Type = "date";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Date);
    f.Type = "datetime";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::DateTime);
    f.Type = "number";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Number);
}

TEST_CASE("ClassifyTrackerFieldFamily handles user single / multi") {
    TrackerField f;
    f.IsUserType = true;
    f.IsArray = false;
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::UserSingle);
    f.IsArray = true;
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::UserMulti);
}

TEST_CASE("TrackerFieldValueParser: ClassifyTrackerFieldFamily_DetectsCascadingSelect [high-risk]") {
    // Guard: cascading-select detection drives the parent/child grid editor. Two paths must
    // both fire: (1) explicit cascadingselect in SchemaCustom; (2) options with non-empty
    // Children. Regressing either silently downgrades the field to flat select and breaks
    // edit UX.
    TrackerField fByCustom;
    fByCustom.SchemaCustom = "com.atlassian.jira.plugin.system.customfieldtypes:cascadingselect";
    CHECK(ClassifyTrackerFieldFamily(fByCustom) == TrackerFieldFamily::CascadingSelect);

    TrackerField fByChildren;
    TrackerFieldOption parent;
    parent.Id = "p";
    parent.Value = "Parent";
    TrackerFieldOption child;
    child.Id = "c";
    child.Value = "Child";
    parent.Children.push_back(child);
    fByChildren.AllowedValueOptions.push_back(parent);
    CHECK(ClassifyTrackerFieldFamily(fByChildren) == TrackerFieldFamily::CascadingSelect);
}

TEST_CASE("ClassifyTrackerFieldFamily picks Select single/multi for flat options") {
    TrackerField f;
    TrackerFieldOption opt;
    opt.Id = "a";
    opt.Value = "A";
    f.AllowedValueOptions.push_back(opt);
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::SelectSingle);
    f.IsArray = true;
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::SelectMulti);
}

TEST_CASE("ClassifyTrackerFieldFamily falls back to Text when no signals") {
    TrackerField f;
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::Text);
}

TEST_CASE("ClassifyTrackerFieldFamily picks SelectMulti for components with no options") {
    TrackerField f;
    f.Id = "components";
    f.IsArray = true;
    f.ItemsType = "component";
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::SelectMulti);
}

TEST_CASE("ClassifyTrackerFieldFamily keeps components as SelectMulti when options are populated") {
    TrackerField f;
    f.Id = "components";
    f.IsArray = true;
    f.ItemsType = "component";
    TrackerFieldOption opt;
    opt.Id = "10001";
    opt.Value = "Backend";
    f.AllowedValueOptions.push_back(opt);
    CHECK(ClassifyTrackerFieldFamily(f) == TrackerFieldFamily::SelectMulti);
}

TEST_CASE("NormalizeTrackerFieldValue handles primitives + nulls") {
    CHECK(NormalizeTrackerFieldValue(nlohmann::json(nullptr)) == "");
    CHECK(NormalizeTrackerFieldValue(nlohmann::json("hello")) == "hello");
    CHECK(NormalizeTrackerFieldValue(nlohmann::json(42)) == "42");
    CHECK(NormalizeTrackerFieldValue(nlohmann::json(true)) == "true");
}

TEST_CASE("NormalizeTrackerFieldValue resolves user objects via accountId + displayName") {
    nlohmann::json user;
    user["accountId"] = "abc";
    user["displayName"] = "Alice Smith";
    CHECK(NormalizeTrackerFieldValue(user) == "Alice Smith");

    nlohmann::json userIdOnly;
    userIdOnly["accountId"] = "abc";
    CHECK(NormalizeTrackerFieldValue(userIdOnly) == "abc");
}

TEST_CASE("NormalizeTrackerFieldValue resolves parent issue link to KEY - SUMMARY") {
    nlohmann::json parent;
    parent["key"] = "PROJ-10";
    parent["fields"]["summary"] = "Parent task";
    CHECK(NormalizeTrackerFieldValue(parent) == "PROJ-10 - Parent task");

    nlohmann::json parentKeyOnly;
    parentKeyOnly["key"] = "PROJ-11";
    CHECK(NormalizeTrackerFieldValue(parentKeyOnly) == "PROJ-11");
}

TEST_CASE("NormalizeTrackerFieldValue resolves cascading selection 'parent > child'") {
    nlohmann::json cascade;
    cascade["value"] = "Outer";
    cascade["child"]["value"] = "Inner";
    CHECK(NormalizeTrackerFieldValue(cascade) == "Outer > Inner");
}

TEST_CASE("NormalizeTrackerFieldValue arrays produce comma-joined values") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json a;
    a["value"] = "Red";
    nlohmann::json b;
    b["value"] = "Green";
    arr.push_back(a);
    arr.push_back(b);
    CHECK(NormalizeTrackerFieldValue(arr) == "Red, Green");
}

TEST_CASE("NormalizeTrackerFieldValue skips empty items in arrays") {
    nlohmann::json arr = nlohmann::json::array();
    arr.push_back(nlohmann::json(nullptr));
    nlohmann::json keep;
    keep["value"] = "kept";
    arr.push_back(keep);
    CHECK(NormalizeTrackerFieldValue(arr) == "kept");
}

TEST_CASE("NormalizeTrackerFieldValue passes ADF doc content through text extraction") {
    nlohmann::json doc;
    doc["type"] = "doc";
    nlohmann::json para;
    para["type"] = "paragraph";
    nlohmann::json text;
    text["type"] = "text";
    text["text"] = "hello there";
    para["content"] = nlohmann::json::array({text});
    doc["content"] = nlohmann::json::array({para});
    const std::string result = NormalizeTrackerFieldValue(doc);
    // exact whitespace varies; just check the visible text survived
    CHECK(result.find("hello there") != std::string::npos);
}

TEST_CASE("ADF text extraction preserves shallow doc output (no regression from depth cap)") {
    // Regression guard: the recursion-depth cap added for the unbounded-recursion
    // DoS fix must not alter output for legitimate (shallow) ADF. A two-level doc
    // with two paragraphs must still extract both visible texts in order.
    nlohmann::json doc;
    doc["type"] = "doc";
    nlohmann::json para1;
    para1["type"] = "paragraph";
    nlohmann::json text1;
    text1["type"] = "text";
    text1["text"] = "first line";
    para1["content"] = nlohmann::json::array({text1});
    nlohmann::json para2;
    para2["type"] = "paragraph";
    nlohmann::json text2;
    text2["type"] = "text";
    text2["text"] = "second line";
    para2["content"] = nlohmann::json::array({text2});
    doc["content"] = nlohmann::json::array({para1, para2});
    const std::string result = NormalizeTrackerFieldValue(doc);
    const std::string::size_type firstPos = result.find("first line");
    const std::string::size_type secondPos = result.find("second line");
    CHECK(firstPos != std::string::npos);
    CHECK(secondPos != std::string::npos);
    CHECK(firstPos < secondPos);
}

// Build an ADF document nested `depth` levels deep, in place, WITHOUT triggering
// nlohmann::json's own recursive copy constructor during setup. We descend via
// references and push each new level into the parent's "content" array.
// IMPORTANT: nlohmann::json's destructor is ALSO recursive, so the natural
// teardown of a deep tree stack-overflows under ASAN's inflated frames regardless
// of the walker's cap — the #adf-deep-nest-fixture-asan-unsafe fixture fragility
// (categories/test.md). The fix is structural: every deep tree built here is torn
// down iteratively via DismantleDeepJson() (below) so teardown is depth-independent,
// which frees the fixture to nest as far past the cap as we like.
static const int kDeepAdfDepth = 400;  // > kMaxAdfRecursionDepth (256) so the cap triggers
static nlohmann::json BuildDeepAdfDoc(int depth) {
    nlohmann::json doc;
    doc["type"] = "doc";
    doc["content"] = nlohmann::json::array();
    nlohmann::json* contentArr = &doc["content"];
    for (int i = 0; i < depth; ++i) {
        nlohmann::json para;
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array();
        contentArr->push_back(std::move(para));
        contentArr = &(*contentArr)[0]["content"];
    }
    nlohmann::json text;
    text["type"] = "text";
    text["text"] = "buried";
    contentArr->push_back(std::move(text));
    return doc;
}

// Tear a deeply-nested json tree down iteratively so the destructor never recurses
// past depth 1 — nlohmann's own dtor is recursive and a deep tree (ADF chain OR
// plain-object chain) overflows the stack under ASAN otherwise. We pop each node,
// move its still-structured children onto a worklist, then let the node destruct
// holding only moved-from (null) children. Depth-independent; safe past any cap.
static void DismantleDeepJson(nlohmann::json& root) {
    std::vector<nlohmann::json> worklist;
    worklist.push_back(std::move(root));
    while (!worklist.empty()) {
        nlohmann::json node = std::move(worklist.back());
        worklist.pop_back();
        if (node.is_structured()) {
            for (nlohmann::json& child : node) {
                if (child.is_structured() && !child.empty()) {
                    worklist.push_back(std::move(child));
                }
            }
        }
        // `node` destructs here with only moved-from (null) children → depth 1.
    }
}

TEST_CASE("ADF doc with hostile deep nesting parses without stack overflow (ExtractAdfTextToStream bound)") {
    // Security regression guard (Pillar 3 — Never crash): a malicious/buggy tracker
    // can return ADF nested far past any legitimate document. Before the depth cap this
    // blew the C++ stack via ExtractAdfTextToStream. A doc nested past the 256 cap must
    // return (bounded text, process survives) rather than crash; the walker stops at the
    // cap and degrades. (The deep tree is torn down iteratively via DismantleDeepJson at
    // the end so nlohmann's recursive dtor doesn't overflow the test itself.)
    nlohmann::json doc = BuildDeepAdfDoc(kDeepAdfDepth);

    // The hard requirement is that this returns at all (no stack overflow / no thrown
    // exception unwinding the parse). Output is bounded/truncated.
    const std::string result = NormalizeTrackerFieldValue(doc);
    CHECK(result.size() < 100000);

    DismantleDeepJson(doc);
}

TEST_CASE("ADF comment body with hostile deep nesting parses without stack overflow (CollectAdfText bound)") {
    // The comment path exercises BOTH recursive walkers: ExtractAdfTextToStream first,
    // then CollectAdfText as the empty-extraction fallback. This guards the second
    // entry point cited by the finding (CollectAdfText) under hostile depth.
    nlohmann::json body = BuildDeepAdfDoc(kDeepAdfDepth);

    nlohmann::json comments = nlohmann::json::array();
    nlohmann::json c;
    c["author"]["displayName"] = "Attacker";
    c["created"] = "2024-01-15T12:34:56.000Z";
    c["body"] = std::move(body);
    comments.push_back(std::move(c));

    // Must return without crashing; bounded output.
    const std::string out = ParseComments(comments);
    CHECK(out.size() < 100000);

    DismantleDeepJson(comments);
}

// The #1220 ADF cap bounded the WALKERS, but a deeply-nested value that is NOT a
// recognised ADF/tracker shape falls through to a value.dump() fallback —
// nlohmann's serializer::dump() recurses per level, so a hostile depth stack-
// overflows (it crashed the ASAN doctest rig). SafeJsonDump must bound it: a
// value past the cap returns a marker instead of recursing into the serializer.
TEST_CASE("NormalizeTrackerFieldValue — deep non-ADF object hits a bounded safe-dump (no stack overflow) [high-risk]") {
    // Build a deep plain-object chain in place (refs — no recursive copy-ctor at
    // setup); kDeepAdfDepth (> the 256 cap) is past the dump cap. Teardown is via
    // DismantleDeepJson at the end so nlohmann's recursive dtor doesn't overflow.
    nlohmann::json deep = nlohmann::json::object();
    nlohmann::json* cur = &deep;
    for (int i = 0; i < kDeepAdfDepth; ++i) {
        (*cur)["nested"] = nlohmann::json::object();
        cur = &(*cur)["nested"];
    }
    (*cur)["leaf"] = "x";
    const std::string out = NormalizeTrackerFieldValue(deep); // would SIGSEGV pre-fix
    CHECK(out.size() < 10000);                                // bounded marker, not a 400-level dump
    CHECK(out.find("depth cap") != std::string::npos);

    DismantleDeepJson(deep);
}

TEST_CASE("FormatDateIfIso length-guards the fixed-index ISO sniff (no OOB read) [high-risk]") {
    // Security regression guard (Pillar 3 — Never crash): `value` is server-supplied. The
    // function indexes value[4] and value[7] to sniff the ISO `-` separators; without the
    // size() >= 10 guard a field shorter than 8 chars is an out-of-bounds read (caught by
    // UBSan/ASan). A value below the length floor must be returned verbatim, never indexed.
    // The 7-char case is THE regression: value[7] would over-read by one past the end.
    CHECK(FormatDateIfIso(std::string()) == "");      // 0 chars — empty
    CHECK(FormatDateIfIso("2024") == "2024");         // 4 chars — value[7] would over-read
    CHECK(FormatDateIfIso("2024-01") == "2024-01");   // 7 chars — value[7] is one past the end
    CHECK(FormatDateIfIso("2024-1-2") == "2024-1-2"); // 8 chars — in-bounds but not ISO shape (no '-' at 7)

    // A well-formed ISO date / datetime is compacted to the YYYY-MM-DD prefix.
    CHECK(FormatDateIfIso("2024-01-15") == "2024-01-15");               // 10 chars — exact ISO date
    CHECK(FormatDateIfIso("2024-01-15T12:34:56.000Z") == "2024-01-15"); // datetime -> date prefix

    // A 10-char string that is NOT ISO-shaped (wrong separator positions) is returned as-is.
    CHECK(FormatDateIfIso("abcdefghij") == "abcdefghij");
}

TEST_CASE("ParseComments returns empty for non-array or empty input") {
    CHECK(ParseComments(nlohmann::json(nullptr)).empty());
    CHECK(ParseComments(nlohmann::json::object()).empty());
    CHECK(ParseComments(nlohmann::json::array()).empty());
}

TEST_CASE("ParseComments formats author + body string") {
    nlohmann::json comments = nlohmann::json::array();
    nlohmann::json c;
    c["author"]["displayName"] = "Alice";
    c["created"] = "2024-01-15T12:34:56.000Z";
    c["body"] = "First comment";
    comments.push_back(c);
    const std::string out = ParseComments(comments);
    CHECK(out.find("Alice") != std::string::npos);
    CHECK(out.find("First comment") != std::string::npos);
    // ISO-prefix gets compacted to YYYY-MM-DD
    CHECK(out.find("2024-01-15") != std::string::npos);
}

TEST_CASE("ParseComments truncates to kMaxComments (20)") {
    nlohmann::json comments = nlohmann::json::array();
    for (int i = 0; i < 50; ++i) {
        nlohmann::json c;
        c["author"]["displayName"] = "U" + std::to_string(i);
        c["created"] = "2024-01-15T12:34:56.000Z";
        c["body"] = "msg-" + std::to_string(i);
        comments.push_back(c);
    }
    const std::string out = ParseComments(comments);
    // Iterates reverse — newest first. Last 20 inserted entries should be present.
    CHECK(out.find("msg-49") != std::string::npos);
    CHECK(out.find("msg-30") != std::string::npos);
    // Older entries beyond the 20-cap should be dropped.
    CHECK(out.find("msg-0\n") == std::string::npos);
}

TEST_CASE("ParseChangelog returns empty for non-array / empty input") {
    CHECK(ParseChangelog(nlohmann::json(nullptr)).empty());
    CHECK(ParseChangelog(nlohmann::json::array()).empty());
}

TEST_CASE("ParseChangelog emits author + from -> to per item") {
    nlohmann::json history = nlohmann::json::object();
    history["author"]["displayName"] = "Bob";
    history["created"] = "2024-02-03T01:02:03.000Z";
    nlohmann::json item;
    item["field"] = "summary";
    item["fromString"] = "old title";
    item["toString"] = "new title";
    history["items"] = nlohmann::json::array({item});
    nlohmann::json hist = nlohmann::json::array({history});

    const std::string out = ParseChangelog(hist);
    CHECK(out.find("Bob") != std::string::npos);
    CHECK(out.find("summary") != std::string::npos);
    CHECK(out.find("old title") != std::string::npos);
    CHECK(out.find("new title") != std::string::npos);
    CHECK(out.find("->") != std::string::npos);
}

TEST_CASE("JsonValueToCompactString trims trailing zeros on floats") {
    CHECK(JsonValueToCompactString(nlohmann::json(1.5)) == "1.5");
    CHECK(JsonValueToCompactString(nlohmann::json(2.0)) == "2");
}

TEST_CASE("JsonGetStringIfString returns empty when key missing or non-string") {
    nlohmann::json o;
    o["a"] = "value";
    o["b"] = 7;
    CHECK(JsonGetStringIfString(o, "a") == "value");
    CHECK(JsonGetStringIfString(o, "b") == ""); // not a string
    CHECK(JsonGetStringIfString(o, "missing") == "");
}

TEST_CASE("FormatTrackerTimetrackingDisplay joins originalEstimate / spent / remaining") {
    nlohmann::json o;
    o["originalEstimate"] = "1h";
    o["timeSpent"] = "30m";
    o["remainingEstimate"] = "30m";
    const std::string out = FormatTrackerTimetrackingDisplay(o);
    CHECK(out.find("Original estimate 1h") != std::string::npos);
    CHECK(out.find("Spent 30m") != std::string::npos);
    CHECK(out.find("Remaining 30m") != std::string::npos);
}

TEST_CASE("FormatTrackerTimetrackingDisplay returns empty for {} object") {
    CHECK(FormatTrackerTimetrackingDisplay(nlohmann::json::object()).empty());
}

TEST_CASE("SortTrackerUsersForDisplay sorts case-insensitive on display name") {
    std::vector<TrackerUser> users;
    TrackerUser u1;
    u1.DisplayName = "charlie";
    TrackerUser u2;
    u2.DisplayName = "Alice";
    TrackerUser u3;
    u3.DisplayName = "bob";
    users.push_back(u1);
    users.push_back(u2);
    users.push_back(u3);
    SortTrackerUsersForDisplay(users);
    CHECK(users[0].DisplayName == "Alice");
    CHECK(users[1].DisplayName == "bob");
    CHECK(users[2].DisplayName == "charlie");
}

TEST_CASE("AppendTrackerUsersFromJsonArray picks up valid users") {
    nlohmann::json arr = nlohmann::json::array();
    nlohmann::json u;
    u["accountId"] = "abc";
    u["displayName"] = "Alice";
    u["emailAddress"] = "alice@example.com";
    u["active"] = true;
    arr.push_back(u);
    nlohmann::json u2;
    u2["displayName"] = ""; // no accountId, no display -> skipped
    arr.push_back(u2);

    std::vector<TrackerUser> out;
    AppendTrackerUsersFromJsonArray(arr, out);
    REQUIRE(out.size() == 1);
    CHECK(out[0].AccountId == "abc");
    CHECK(out[0].DisplayName == "Alice");
    CHECK(out[0].EmailAddress == "alice@example.com");
}

TEST_CASE("AppendTrackerUsersFromJsonArray ignores non-array input") {
    std::vector<TrackerUser> out;
    AppendTrackerUsersFromJsonArray(nlohmann::json::object(), out);
    CHECK(out.empty());
}

// --- PR A2 decomposition goldens: exercise each split dispatch path so the
// --- handler extraction stays byte-for-byte with the pre-refactor branch tower.

TEST_CASE("NormalizeTrackerFieldValue resolves option 'value' then 'name' then string id") {
    nlohmann::json byValue;
    byValue["value"] = "High";
    CHECK(NormalizeTrackerFieldValue(byValue) == "High");

    nlohmann::json byName;
    byName["name"] = "In Progress";
    CHECK(NormalizeTrackerFieldValue(byName) == "In Progress");

    nlohmann::json byStringId;
    byStringId["id"] = "10042";
    CHECK(NormalizeTrackerFieldValue(byStringId) == "10042");
}

TEST_CASE("NormalizeTrackerFieldValue resolves numeric id (int + unsigned)") {
    nlohmann::json intId;
    intId["id"] = 7;
    CHECK(NormalizeTrackerFieldValue(intId) == "7");

    nlohmann::json unsignedId;
    unsignedId["id"] = 4000000000ULL;
    CHECK(NormalizeTrackerFieldValue(unsignedId) == "4000000000");
}

TEST_CASE("NormalizeTrackerFieldValue formats Jira timetracking object") {
    nlohmann::json tt;
    tt["originalEstimate"] = "1h";
    tt["timeSpentSeconds"] = 1800;
    const std::string out = NormalizeTrackerFieldValue(tt);
    CHECK(out.find("Original estimate 1h") != std::string::npos);
    // FormatWorkDurationFromSeconds always emits an hours component when the
    // higher units are empty, so 1800s renders "0h 30m" (not "30m").
    CHECK(out.find("Spent 0h 30m") != std::string::npos);
}

TEST_CASE("NormalizeTrackerFieldValue dumps an unrecognised object verbatim") {
    nlohmann::json mystery;
    mystery["foo"] = "bar";
    // No comments/doc/key/accountId/displayName/value/name/id/timetracking shape
    // -> falls through to raw dump.
    CHECK(NormalizeTrackerFieldValue(mystery) == mystery.dump());
}

TEST_CASE("NormalizeTrackerFieldValue empty value/name/id object falls through to dump") {
    nlohmann::json empties;
    empties["value"] = "";
    empties["name"] = "";
    empties["id"] = "";
    // Every preferred label key is empty -> original returned the raw dump.
    CHECK(NormalizeTrackerFieldValue(empties) == empties.dump());
}

TEST_CASE("ParseChangelog falls back to from/to id keys when *String absent") {
    nlohmann::json history = nlohmann::json::object();
    history["author"]["displayName"] = "Carol";
    nlohmann::json item;
    item["field"] = "assignee";
    item["from"] = 101;    // numeric id fallback
    item["to"] = "acct-2"; // string id fallback
    history["items"] = nlohmann::json::array({item});
    nlohmann::json hist = nlohmann::json::array({history});

    const std::string out = ParseChangelog(hist);
    CHECK(out.find("Carol") != std::string::npos);
    CHECK(out.find("assignee: 101 -> acct-2") != std::string::npos);
}

TEST_CASE("ParseChangelog formats time-duration field values") {
    nlohmann::json history = nlohmann::json::object();
    history["author"]["displayName"] = "Dan";
    nlohmann::json item;
    item["field"] = "timespent"; // recognised time-duration field
    item["fromString"] = "3600"; // seconds -> "1h"
    item["toString"] = "7200";   // seconds -> "2h"
    history["items"] = nlohmann::json::array({item});
    nlohmann::json hist = nlohmann::json::array({history});

    const std::string out = ParseChangelog(hist);
    CHECK(out.find("1h") != std::string::npos);
    CHECK(out.find("2h") != std::string::npos);
}

TEST_CASE("ParseChangelog truncates over-long values to 200 chars + ellipsis") {
    const std::string longVal(500, 'x');
    nlohmann::json history = nlohmann::json::object();
    history["author"]["displayName"] = "Eve";
    nlohmann::json item;
    item["field"] = "description";
    item["fromString"] = "";
    item["toString"] = longVal;
    history["items"] = nlohmann::json::array({item});
    nlohmann::json hist = nlohmann::json::array({history});

    const std::string out = ParseChangelog(hist);
    CHECK(out.find(std::string(200, 'x') + "...") != std::string::npos);
    // The full 500-char run must not survive un-truncated.
    CHECK(out.find(std::string(201, 'x')) == std::string::npos);
}

TEST_CASE("ParseChangelog caps at 60 entries and appends truncated marker") {
    nlohmann::json hist = nlohmann::json::array();
    for (int i = 0; i < 80; ++i) {
        nlohmann::json history = nlohmann::json::object();
        history["author"]["displayName"] = "U";
        nlohmann::json item;
        item["field"] = "status";
        item["fromString"] = "a" + std::to_string(i);
        item["toString"] = "b" + std::to_string(i);
        history["items"] = nlohmann::json::array({item});
        hist.push_back(history);
    }
    const std::string out = ParseChangelog(hist);
    CHECK(out.find("[... truncated ...]") != std::string::npos);
    // 60th entry (index 59) present; 61st (index 60) dropped.
    CHECK(out.find("a59 -> b59") != std::string::npos);
    CHECK(out.find("a60 -> b60") == std::string::npos);
}

TEST_CASE("ParseChangelog dumps raw when no formattable items produced") {
    // Array is non-empty but every history lacks an items array -> raw dump path.
    nlohmann::json hist = nlohmann::json::array();
    nlohmann::json history = nlohmann::json::object();
    history["author"]["displayName"] = "NoItems";
    hist.push_back(history);
    const std::string out = ParseChangelog(hist);
    CHECK(out == hist.dump());
}
