#include "SmatchetJqlProjectPill_detail.h"

#include <doctest/doctest.h>

using SmatchetJqlProjectPill::detail::EntryPassesPillFilter;

namespace {
FieldCatalogCache::CachedProjectEntry MakeEntry(const std::string& key, const std::string& backend,
                                                const std::string& endpoint) {
    FieldCatalogCache::CachedProjectEntry e;
    e.projectKey = key;
    e.backend = backend;
    e.endpoint = endpoint;
    return e;
}
} // namespace

TEST_CASE("EntryPassesPillFilter: an empty project key is always rejected") {
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("", "Jira", "acme.atlassian.net"), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a mismatched backend is rejected") {
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("PROJ", "Plane", ""), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty backend is treated as a wildcard and passes") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "", ""), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a Jira entry with a matching endpoint passes") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "acme.atlassian.net"), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a Jira entry with a mismatched endpoint is rejected") {
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "other.atlassian.net"), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty endpoint is a wildcard regardless of domain") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", ""), "Jira", "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty endpoint filter disables the endpoint filter") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "other.atlassian.net"), "Jira", ""));
}

TEST_CASE("EntryPassesPillFilter: a Plane entry with a matching endpoint passes") {
    CHECK(EntryPassesPillFilter(MakeEntry("uuid-1", "Plane", "https://api.plane.so|acme"), "Plane",
                                "https://api.plane.so|acme"));
}
