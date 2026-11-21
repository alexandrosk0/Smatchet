#include <doctest/doctest.h>

#include "Ui/SmatchetProjectPicker_detail.h"

#include <cstdint>
#include <string>
#include <vector>

using SmatchetProjectPicker::detail::AllProjectKey;
using SmatchetProjectPicker::detail::AllProjectPasses;
using SmatchetProjectPicker::detail::ContainsCi;
using SmatchetProjectPicker::detail::MakeRowLabel;
using SmatchetProjectPicker::detail::RecentEntryPasses;
using SmatchetProjectPicker::detail::RecentSeedKeys;

namespace {

FieldCatalogCache::CachedProjectEntry MakeEntry(const std::string& key, const std::string& backend,
                                                const std::string& endpoint) {
    FieldCatalogCache::CachedProjectEntry e;
    e.projectKey = key;
    e.backend = backend;
    e.endpoint = endpoint;
    return e;
}

RemoteProject MakeProject(const std::string& id, const std::string& key, const std::string& displayName) {
    RemoteProject p;
    p.id = id;
    p.key = key;
    p.displayName = displayName;
    return p;
}

} // namespace

TEST_CASE("ContainsCi is case-insensitive and empty-needle matches" * doctest::test_suite("[ui]")) {
    CHECK(ContainsCi("HelloWorld", "world"));
    CHECK(ContainsCi("HelloWorld", "HELLO"));
    CHECK(ContainsCi("anything", ""));
    CHECK_FALSE(ContainsCi("abc", "xyz"));
    CHECK(ContainsCi("\xCE\xA0\xCF\x81\xCF\x8C\xCE\xB3\xCF\x81\xCE\xB1\xCE\xBC\xCE\xBC\xCE\xB1",
                     "\xCE\xA0\xCE\xA1\xCE\x8C")); // Πρόγραμμα / ΠΡΌ
    CHECK(ContainsCi("\xD0\x9F\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82", "\xD0\xBF\xD0\xA0\xD0\x9E")); // Проект / пРО
}

TEST_CASE("RecentEntryPasses rejects empty key" * doctest::test_suite("[ui]")) {
    CHECK_FALSE(RecentEntryPasses(MakeEntry("", "Jira", "ep"), "Jira", "ep", ""));
}

TEST_CASE("RecentEntryPasses filters on backend mismatch" * doctest::test_suite("[ui]")) {
    CHECK_FALSE(RecentEntryPasses(MakeEntry("PROJ", "Plane", "ep"), "Jira", "ep", ""));
    CHECK(RecentEntryPasses(MakeEntry("PROJ", "Jira", "ep"), "Jira", "ep", ""));
}

TEST_CASE("RecentEntryPasses treats empty entry backend/endpoint as wildcard" * doctest::test_suite("[ui]")) {
    CHECK(RecentEntryPasses(MakeEntry("PROJ", "", ""), "Jira", "ep", ""));
}

TEST_CASE("RecentEntryPasses empty filter backend/endpoint disables that filter" * doctest::test_suite("[ui]")) {
    CHECK(RecentEntryPasses(MakeEntry("PROJ", "Plane", "other"), "", "", ""));
}

TEST_CASE("RecentEntryPasses filters on endpoint mismatch" * doctest::test_suite("[ui]")) {
    CHECK_FALSE(RecentEntryPasses(MakeEntry("PROJ", "Jira", "epA"), "Jira", "epB", ""));
}

TEST_CASE("RecentEntryPasses filters on search text" * doctest::test_suite("[ui]")) {
    CHECK_FALSE(RecentEntryPasses(MakeEntry("PROJ", "Jira", "ep"), "Jira", "ep", "zzz"));
    CHECK(RecentEntryPasses(MakeEntry("PROJ", "Jira", "ep"), "Jira", "ep", "pro"));
}

TEST_CASE("RecentSeedKeys: this tracker's listed saved-catalog projects, most recently used first, capped" *
          doctest::test_suite("[ui]")) {
    std::vector<FieldCatalogCache::CachedProjectEntry> entries;
    const auto add = [&entries](const char* key, const char* backend, const char* endpoint, std::int64_t used) {
        FieldCatalogCache::CachedProjectEntry e = MakeEntry(key, backend, endpoint);
        e.lastUsedUnix = used;
        entries.push_back(e);
    };
    add("OLD", "Jira", "ep", 10);
    add("NEW", "Jira", "ep", 40);
    add("OTHER", "Plane", "ep", 50); // another tracker kind
    add("ELSE", "Jira", "epB", 60);  // another instance
    add("MID", "Jira", "ep", 30);
    add("NEW", "Jira", "ep", 20); // the same key again: listed once, at its newest use
    add("", "Jira", "ep", 70);    // no key

    const std::vector<std::string> listed{"OLD", "MID", "NEW", "ELSE", "OTHER", "UNSAVED"};
    CHECK(RecentSeedKeys(entries, "Jira", "ep", listed, 5) == std::vector<std::string>{"NEW", "MID", "OLD"});
    CHECK(RecentSeedKeys(entries, "Jira", "ep", listed, 2) == std::vector<std::string>{"NEW", "MID"});
    CHECK(RecentSeedKeys(entries, "Jira", "ep", listed, 0).empty());
    CHECK(RecentSeedKeys({}, "Jira", "ep", listed, 5).empty());
    // A saved-catalog project the tracker's own list does not hold is never taken, and does not use up the cap.
    CHECK(RecentSeedKeys(entries, "Jira", "ep", {"OLD", "MID"}, 1) == std::vector<std::string>{"MID"});
    CHECK(RecentSeedKeys(entries, "Jira", "ep", {}, 5).empty());
}

TEST_CASE("AllProjectKey uses id for Plane, key otherwise" * doctest::test_suite("[ui]")) {
    CHECK(AllProjectKey(MakeProject("uuid-1", "KEY", "Name"), "Plane") == "uuid-1");
    CHECK(AllProjectKey(MakeProject("uuid-1", "KEY", "Name"), "Jira") == "KEY");
}

TEST_CASE("AllProjectPasses rejects empty key" * doctest::test_suite("[ui]")) {
    CHECK_FALSE(AllProjectPasses("", MakeProject("id", "", "Name"), ""));
}

TEST_CASE("AllProjectPasses matches key or display name on filter" * doctest::test_suite("[ui]")) {
    RemoteProject p = MakeProject("id", "ABC", "Widget Service");
    CHECK(AllProjectPasses("ABC", p, "abc"));
    CHECK(AllProjectPasses("ABC", p, "widget"));
    CHECK_FALSE(AllProjectPasses("ABC", p, "nomatch"));
    CHECK(AllProjectPasses("ABC", p, ""));
    // Words in any order, each in the key or the name.
    CHECK(AllProjectPasses("ABC", p, "service widget"));
    CHECK(AllProjectPasses("ABC", p, "SERVICE abc"));
    CHECK_FALSE(AllProjectPasses("ABC", p, "service nomatch"));
    CHECK(RecentEntryPasses(MakeEntry("ALPHA-BETA", "Jira", "ep"), "Jira", "ep", "beta alpha"));
}

TEST_CASE("MakeRowLabel formats Jira and Plane rows" * doctest::test_suite("[ui]")) {
    CHECK(MakeRowLabel(MakeProject("id", "PROJ", "Project X"), "Jira") == "PROJ \xE2\x80\x94 Project X");
    CHECK(MakeRowLabel(MakeProject("id", "PROJ", ""), "Jira") == "PROJ");
    CHECK(MakeRowLabel(MakeProject("id", "", "Plane Proj"), "Plane") == "Plane Proj");
    CHECK(MakeRowLabel(MakeProject("uuid-only", "", ""), "Plane") == "uuid-only");
}
