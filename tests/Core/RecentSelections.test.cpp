#include <doctest/doctest.h>

#include "RecentSelections.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet::recents::kMaxKeyBytes;
using smatchet::recents::kMaxPerScope;
using smatchet::recents::kMaxScopes;
using smatchet::recents::MakeScopeKey;
using smatchet::recents::RecentSelectionStore;

namespace {
using Ids = std::vector<std::string>;
} // namespace

TEST_CASE("RecentSelectionStore: newest pick comes first") {
    RecentSelectionStore store;
    CHECK(store.Record("jira|assignee", "alice"));
    CHECK(store.Record("jira|assignee", "bob"));
    CHECK(store.Record("jira|assignee", "carol"));
    CHECK(store.Get("jira|assignee") == Ids{"carol", "bob", "alice"});
}

TEST_CASE("RecentSelectionStore: keeps at most five ids per scope") {
    RecentSelectionStore store;
    for (int i = 1; i <= 8; ++i) {
        store.Record("s", "id" + std::to_string(i));
    }
    REQUIRE(kMaxPerScope == 5);
    CHECK(store.Get("s") == Ids{"id8", "id7", "id6", "id5", "id4"});
}

TEST_CASE("RecentSelectionStore: picking a listed id again moves it to the front, no duplicate") {
    RecentSelectionStore store;
    store.Record("s", "a");
    store.Record("s", "b");
    store.Record("s", "c");
    CHECK(store.Record("s", "a"));
    CHECK(store.Get("s") == Ids{"a", "c", "b"});
}

TEST_CASE("RecentSelectionStore: re-picking the newest id in the newest scope changes nothing") {
    RecentSelectionStore store;
    store.Record("s", "a");
    CHECK_FALSE(store.Record("s", "a"));
    CHECK(store.Get("s") == Ids{"a"});
}

TEST_CASE("RecentSelectionStore: scopes do not see each other's ids") {
    RecentSelectionStore store;
    store.Record("jira|assignee", "alice");
    store.Record("jira|priority", "high");
    store.Record("plane|assignee", "zed");
    CHECK(store.Get("jira|assignee") == Ids{"alice"});
    CHECK(store.Get("jira|priority") == Ids{"high"});
    CHECK(store.Get("plane|assignee") == Ids{"zed"});
    CHECK(store.Get("jira|reporter").empty());
    CHECK(store.ScopeCount() == 3);
}

TEST_CASE("RecentSelectionStore: empty or over-long scope and id are not recorded") {
    RecentSelectionStore store;
    CHECK_FALSE(store.Record("", "a"));
    CHECK_FALSE(store.Record("s", ""));
    CHECK_FALSE(store.Record(std::string(kMaxKeyBytes + 1, 'x'), "a"));
    CHECK_FALSE(store.Record("s", std::string(kMaxKeyBytes + 1, 'x')));
    CHECK(store.ScopeCount() == 0);
    CHECK(store.Record("s", std::string(kMaxKeyBytes, 'x')));
}

TEST_CASE("RecentSelectionStore: past the scope cap the scope touched longest ago is dropped") {
    RecentSelectionStore store;
    for (std::size_t i = 0; i < kMaxScopes; ++i) {
        store.Record("scope" + std::to_string(i), "v");
    }
    CHECK(store.ScopeCount() == kMaxScopes);
    store.Record("scope0", "w"); // touch the oldest: scope1 is now the oldest
    store.Record("fresh", "v");
    CHECK(store.ScopeCount() == kMaxScopes);
    CHECK(store.Get("scope0") == Ids{"w", "v"});
    CHECK(store.Get("scope1").empty());
    CHECK(store.Get("fresh") == Ids{"v"});
}

TEST_CASE("RecentSelectionStore: JSON round trip keeps ids, their order and the scope order") {
    RecentSelectionStore store;
    store.Record("jira|assignee", "alice");
    store.Record("jira|assignee", "bob");
    store.Record("plane|state", "done");

    const nlohmann::json j = store.ToJson();
    REQUIRE(j.is_array());
    REQUIRE(j.size() == 2);
    CHECK(j[0]["scope"] == "plane|state"); // most recently touched scope first
    CHECK(j[1]["scope"] == "jira|assignee");

    RecentSelectionStore loaded;
    loaded.FromJson(j);
    CHECK(loaded.Get("jira|assignee") == Ids{"bob", "alice"});
    CHECK(loaded.Get("plane|state") == Ids{"done"});
    CHECK(loaded.ToJson() == j);
}

TEST_CASE("RecentSelectionStore: FromJson skips what is not the saved shape and re-applies the caps") {
    nlohmann::json j = nlohmann::json::array();
    j.push_back("not an object");
    j.push_back({{"scope", 7}, {"ids", nlohmann::json::array({"a"})}});
    j.push_back({{"scope", "no-ids"}});
    j.push_back({{"scope", "empty"}, {"ids", nlohmann::json::array()}});
    j.push_back({{"scope", "ok"}, {"ids", nlohmann::json::array({"a", 3, "a", "", "b", "c", "d", "e", "f", "g"})}});
    j.push_back({{"scope", "ok"}, {"ids", nlohmann::json::array({"second copy"})}});

    RecentSelectionStore store;
    store.Record("stale", "x");
    store.FromJson(j);
    CHECK(store.ScopeCount() == 1);
    CHECK(store.Get("stale").empty());
    CHECK(store.Get("ok") == Ids{"a", "b", "c", "d", "e"});

    store.FromJson(nlohmann::json::object()); // the wrong top-level type loads as empty
    CHECK(store.ScopeCount() == 0);
}

TEST_CASE("RecentSelectionStore: FromJson keeps at most kMaxScopes scopes") {
    nlohmann::json j = nlohmann::json::array();
    for (std::size_t i = 0; i < kMaxScopes + 10; ++i) {
        j.push_back({{"scope", "s" + std::to_string(i)}, {"ids", nlohmann::json::array({"v"})}});
    }
    RecentSelectionStore store;
    store.FromJson(j);
    CHECK(store.ScopeCount() == kMaxScopes);
    CHECK(store.Get("s0") == Ids{"v"});
    CHECK(store.Get("s" + std::to_string(kMaxScopes + 5)).empty());
}

TEST_CASE("MakeScopeKey: backend and field joined by a bar") {
    CHECK(MakeScopeKey("jira:https://x.atlassian.net", "assignee") == "jira:https://x.atlassian.net|assignee");
    CHECK(MakeScopeKey("", "project") == "|project");
}

TEST_CASE("shared store: picks made before the config is loaded stay in memory and are readable") {
    smatchet::recents::ResetSharedForTests();
    smatchet::recents::RecordAndPersist("t|field", "one");
    smatchet::recents::RecordAndPersist("t|field", "two");
    smatchet::recents::RecordAndPersist("t|other", "x");
    CHECK(smatchet::recents::SharedGet("t|field") == Ids{"two", "one"});
    CHECK(smatchet::recents::SharedGet("t|other") == Ids{"x"});
    smatchet::recents::ResetSharedForTests();
    CHECK(smatchet::recents::SharedGet("t|field").empty());
}

TEST_CASE("shared store: loading the merged config fills it from the recents key") {
    smatchet::recents::ResetSharedForTests();
    nlohmann::json config = nlohmann::json::object();
    nlohmann::json entry = nlohmann::json::object();
    entry["scope"] = "t|assignee";
    entry["ids"] = nlohmann::json::array({"bob", "alice"});
    config[smatchet::recents::kConfigJsonKey] = nlohmann::json::array({entry});
    smatchet::recents::LoadSharedFromConfigJson(config);
    CHECK(smatchet::recents::SharedGet("t|assignee") == Ids{"bob", "alice"});

    // A config without the key, or with a damaged value, loads as empty instead of failing.
    smatchet::recents::LoadSharedFromConfigJson(nlohmann::json::object());
    CHECK(smatchet::recents::SharedGet("t|assignee").empty());
    config[smatchet::recents::kConfigJsonKey] = "damaged";
    smatchet::recents::LoadSharedFromConfigJson(config);
    CHECK(smatchet::recents::SharedGet("t|assignee").empty());

    smatchet::recents::ResetSharedForTests(); // saving back off: later tests must not write a config file
}

TEST_CASE("shared store: a test scope starts empty and puts the earlier picks back when it ends") {
    smatchet::recents::ResetSharedForTests();
    smatchet::recents::RecordAndPersist("t|field", "kept");

    smatchet::recents::BeginSharedTestScope();
    CHECK(smatchet::recents::SharedGet("t|field").empty());
    smatchet::recents::RecordAndPersist("t|field", "scratch");
    smatchet::recents::BeginSharedTestScope(); // again before End: empties, keeps what was set aside
    CHECK(smatchet::recents::SharedGet("t|field").empty());
    smatchet::recents::RecordAndPersist("t|field", "scratch2");

    smatchet::recents::EndSharedTestScope();
    CHECK(smatchet::recents::SharedGet("t|field") == Ids{"kept"});
    smatchet::recents::EndSharedTestScope(); // without a Begin: nothing changes
    CHECK(smatchet::recents::SharedGet("t|field") == Ids{"kept"});
    smatchet::recents::ResetSharedForTests();
}
