#include <doctest/doctest.h>

#include "EditMetaScopePure.h"

#include <string>

namespace {
using namespace smatchet::editmeta;
} // namespace

TEST_CASE("ProjectOfIssue: a Jira-shaped key names its project, anything else has none") {
    CHECK(ProjectOfIssue("PROJ-12") == "PROJ");
    CHECK(ProjectOfIssue("ab_c-7") == "ab_c");
    CHECK(ProjectOfIssue("").empty());
    CHECK(ProjectOfIssue("local-draft").empty());
    CHECK(ProjectOfIssue("550e8400-e29b-41d4-a716-446655440000").empty());
    CHECK(ProjectOfIssue("owner/repo#3").empty());
}

TEST_CASE("BuildScopeStoreKey / SplitScopeStoreKey round-trip, including separators in the parts") {
    const std::string project = "P|ROJ\\x";
    const std::string type = "sub|task";
    const std::string key = BuildScopeStoreKey(project, type);
    REQUIRE_FALSE(key.empty());
    std::string p;
    std::string t;
    REQUIRE(SplitScopeStoreKey(key, p, t));
    CHECK(p == project);
    CHECK(t == type);

    CHECK(BuildScopeStoreKey("A|B", "c") != BuildScopeStoreKey("A", "B|c")); // never collide
}

TEST_CASE("BuildScopeStoreKey: an empty project is a backend-wide scope; an empty type has no key") {
    const std::string key = BuildScopeStoreKey("", "bug");
    std::string p = "x";
    std::string t;
    REQUIRE(SplitScopeStoreKey(key, p, t));
    CHECK(p.empty());
    CHECK(t == "bug");
    CHECK(BuildScopeStoreKey("PROJ", "").empty());
}

TEST_CASE("SplitScopeStoreKey rejects malformed keys") {
    std::string p;
    std::string t;
    CHECK_FALSE(SplitScopeStoreKey("", p, t));
    CHECK_FALSE(SplitScopeStoreKey("noseparator", p, t));
    CHECK_FALSE(SplitScopeStoreKey("PROJ|", p, t));
    CHECK_FALSE(SplitScopeStoreKey("PROJ|a|b", p, t));
    CHECK_FALSE(SplitScopeStoreKey("PROJ|bug\\", p, t));
}

TEST_CASE("IsRepresentativeEditPermissions needs one editable field") {
    CHECK_FALSE(IsRepresentativeEditPermissions({}));
    CHECK_FALSE(IsRepresentativeEditPermissions({{"summary", false}, {"labels", false}}));
    CHECK(IsRepresentativeEditPermissions({{"summary", false}, {"labels", true}}));
}

TEST_CASE("EditMetaScopePure constants hold the plan's budgets") {
    CHECK(kSavedEditPermissionsMaxAgeSec == 2592000);
    CHECK(kMaxCandidatesPerScope == 3);
    CHECK(kMaxExtraCandidateFetchesPerWarm == 8);
    CHECK(kMaxScopesPerWarm == 64);
    CHECK(kNoRepresentativeBackoffSec == 1800);
}
