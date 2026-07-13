// GitHubMutationPure doctest — pins the fields→PATCH-plan translation, the milestone
// title→number lookup, and the label-name extraction the GitHub write path (the
// UpdateIssueFields slice of docs/plans/shipped/github-tracker-backend.md) is built
// on. Hermetic: no cpr, no config, no filesystem.

#include "GitHubMutationPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using smatchet::github::BuildGitHubIssueUpdatePlan;
using smatchet::github::ExtractGitHubIssueLabelNames;
using smatchet::github::FindGitHubMilestoneNumberByTitle;
using smatchet::github::GitHubIssueUpdatePlan;

TEST_CASE("BuildGitHubIssueUpdatePlan — scalar ids translate to GitHub REST names") {
    SUBCASE("summary → title") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", "New title"}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"title", "New title"}});
        CHECK_FALSE(r.value().HasLabelsEdit);
        CHECK_FALSE(r.value().NeedsMilestoneResolve);
    }
    SUBCASE("summary strips exactly one leading '[PR] ' display prefix") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", "[PR] [PR] real"}});
        REQUIRE(r);
        CHECK(r.value().PatchBody["title"].get<std::string>() == "[PR] real");
    }
    SUBCASE("empty / whitespace-only summary is rejected (GitHub requires a title)") {
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", ""}}));
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", "   "}}));
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", "[PR] "}}));
    }
    SUBCASE("description → body; empty clears") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"description", ""}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"body", ""}});
    }
    SUBCASE("status → state, case-insensitive open/closed") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"status", " Closed "}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"state", "closed"}});
    }
    SUBCASE("status outside open/closed is rejected (merged-PR is read-only)") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"status", "merged-PR"}});
        REQUIRE_FALSE(r);
        CHECK(r.error().find("open") != std::string::npos);
    }
    SUBCASE("non-string scalar value is rejected") {
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", 42}}));
    }
}

TEST_CASE("BuildGitHubIssueUpdatePlan — multi-value ids accept array and CSV shapes") {
    SUBCASE("assignee array → assignees (set-replace)") {
        const auto r =
            BuildGitHubIssueUpdatePlan(nlohmann::json{{"assignee", nlohmann::json::array({"alice", "bob"})}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"assignees", nlohmann::json::array({"alice", "bob"})}});
    }
    SUBCASE("assignee CSV string (offline-queue verbatim fallback) is split + trimmed") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"assignee", " alice , bob "}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"assignees", nlohmann::json::array({"alice", "bob"})}});
    }
    SUBCASE("empty assignee clears (empty assignees array)") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"assignee", ""}});
        REQUIRE(r);
        CHECK(r.value().PatchBody == nlohmann::json{{"assignees", nlohmann::json::array()}});
    }
    SUBCASE("labels never enter PatchBody — they carry the reconcile intent instead") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"labels", "bug, p0"}});
        REQUIRE(r);
        CHECK(r.value().PatchBody.empty());
        CHECK(r.value().HasLabelsEdit);
        CHECK(r.value().LabelsIntended == std::vector<std::string>({"bug", "p0"}));
    }
    SUBCASE("empty labels edit means remove-all (intent set, empty list)") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"labels", nlohmann::json::array()}});
        REQUIRE(r);
        CHECK(r.value().HasLabelsEdit);
        CHECK(r.value().LabelsIntended.empty());
    }
    SUBCASE("non-string array element is rejected") {
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"labels", nlohmann::json::array({"ok", 7})}}));
    }
}

TEST_CASE("BuildGitHubIssueUpdatePlan — milestone shapes") {
    SUBCASE("empty clears via JSON null") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"milestone", ""}});
        REQUIRE(r);
        CHECK(r.value().PatchBody["milestone"].is_null());
        CHECK_FALSE(r.value().NeedsMilestoneResolve);
    }
    SUBCASE("all-digit value is used as the milestone number directly") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"milestone", "12"}});
        REQUIRE(r);
        CHECK(r.value().PatchBody["milestone"].get<std::int64_t>() == 12);
        CHECK_FALSE(r.value().NeedsMilestoneResolve);
    }
    SUBCASE("display title defers to the caller's lookup") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"milestone", "v1.0"}});
        REQUIRE(r);
        CHECK(r.value().NeedsMilestoneResolve);
        CHECK(r.value().MilestoneTitle == "v1.0");
        CHECK(r.value().PatchBody.find("milestone") == r.value().PatchBody.end());
    }
    SUBCASE("an over-long digit run is treated as a title, not a number (stoll range)") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"milestone", "99999999999999999999"}});
        REQUIRE(r);
        CHECK(r.value().NeedsMilestoneResolve);
    }
}

TEST_CASE("BuildGitHubIssueUpdatePlan — rejects unknown ids and empty payloads") {
    CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json::object()));
    CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json(42)));
    SUBCASE("read-only catalog ids error with the field named") {
        const auto r = BuildGitHubIssueUpdatePlan(nlohmann::json{{"comments", "3"}});
        REQUIRE_FALSE(r);
        CHECK(r.error().find("comments") != std::string::npos);
    }
    SUBCASE("a multi-key payload with one bad key rejects the whole edit") {
        CHECK_FALSE(BuildGitHubIssueUpdatePlan(nlohmann::json{{"summary", "ok"}, {"pr.head", "main"}}));
    }
    SUBCASE("a multi-key payload of valid keys combines into one plan") {
        const auto r = BuildGitHubIssueUpdatePlan(
            nlohmann::json{{"summary", "t"}, {"status", "open"}, {"labels", nlohmann::json::array({"bug"})}});
        REQUIRE(r);
        CHECK(r.value().PatchBody["title"].get<std::string>() == "t");
        CHECK(r.value().PatchBody["state"].get<std::string>() == "open");
        CHECK(r.value().HasLabelsEdit);
    }
}

TEST_CASE("FindGitHubMilestoneNumberByTitle — exact-title match over one page") {
    const nlohmann::json page = nlohmann::json::array({
        nlohmann::json{{"number", 1}, {"title", "v0.9"}},
        nlohmann::json{{"number", 3}, {"title", "v1.0"}},
        "not-an-object",
        nlohmann::json{{"title", "no-number"}},
    });
    CHECK(FindGitHubMilestoneNumberByTitle(page, "v1.0") == 3);
    CHECK(FindGitHubMilestoneNumberByTitle(page, "v2.0") == 0);
    CHECK(FindGitHubMilestoneNumberByTitle(page, "no-number") == 0);
    CHECK(FindGitHubMilestoneNumberByTitle(page, "") == 0);
    CHECK(FindGitHubMilestoneNumberByTitle(nlohmann::json::object(), "v1.0") == 0);
}

TEST_CASE("ExtractGitHubIssueLabelNames — tolerates both GitHub label shapes") {
    SUBCASE("object labels ({\"name\": ...}) and bare strings mix") {
        const nlohmann::json issue = nlohmann::json{
            {"labels", nlohmann::json::array({nlohmann::json{{"name", "bug"}}, "stale", nlohmann::json{{"id", 1}}})}};
        CHECK(ExtractGitHubIssueLabelNames(issue) == std::vector<std::string>({"bug", "stale"}));
    }
    SUBCASE("missing / non-array labels → empty") {
        CHECK(ExtractGitHubIssueLabelNames(nlohmann::json::object()).empty());
        CHECK(ExtractGitHubIssueLabelNames(nlohmann::json{{"labels", "bug"}}).empty());
        CHECK(ExtractGitHubIssueLabelNames(nlohmann::json(7)).empty());
    }
}
