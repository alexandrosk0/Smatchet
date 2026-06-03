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
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("", "Jira", "acme.atlassian.net"), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a non-Jira backend is rejected") {
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("PROJ", "Plane", ""), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty backend is treated as a wildcard and passes") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "", ""), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a Jira entry with a matching endpoint passes") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "acme.atlassian.net"), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: a Jira entry with a mismatched endpoint is rejected") {
    CHECK_FALSE(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "other.atlassian.net"), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty endpoint is a wildcard regardless of domain") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", ""), "acme.atlassian.net"));
}

TEST_CASE("EntryPassesPillFilter: an empty domain disables the endpoint filter") {
    CHECK(EntryPassesPillFilter(MakeEntry("PROJ", "Jira", "other.atlassian.net"), ""));
}
