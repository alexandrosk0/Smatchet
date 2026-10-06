#include <doctest/doctest.h>

#include "Ui/SmatchetProjectPicker_detail.h"

using SmatchetProjectPicker::detail::AllProjectKey;
using SmatchetProjectPicker::detail::AllProjectPasses;
using SmatchetProjectPicker::detail::ContainsCi;
using SmatchetProjectPicker::detail::MakeRowLabel;
using SmatchetProjectPicker::detail::RecentEntryPasses;

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
}

TEST_CASE("MakeRowLabel formats Jira and Plane rows" * doctest::test_suite("[ui]")) {
    CHECK(MakeRowLabel(MakeProject("id", "PROJ", "Project X"), "Jira") == "PROJ \xE2\x80\x94 Project X");
    CHECK(MakeRowLabel(MakeProject("id", "PROJ", ""), "Jira") == "PROJ");
    CHECK(MakeRowLabel(MakeProject("id", "", "Plane Proj"), "Plane") == "Plane Proj");
    CHECK(MakeRowLabel(MakeProject("uuid-only", "", ""), "Plane") == "uuid-only");
}
