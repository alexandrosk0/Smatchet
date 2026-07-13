// TEST_COVERAGE_GAP_MAP.md Tier 2 close-out — the last untested backend HTTP shell.
// GitHubCommitsForUser (Vcs/GitHubCommits.cpp) fetch loop driven at the in-process
// httplib loopback over real cpr HTTP, same rig as GitHubClientHttp.test.cpp: guard
// fail-fasts, tracker-repo fallback, per-repo non-fatal error accumulation, and the
// newest-first merge + maxN truncation. Characterization tests — they pin CURRENT
// behaviour of the shipped shell.

#include "JiraCatalogHttpFixture.h"
#include "Vcs/GitHubCommits.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

// A TrackerConfig pointed at the loopback. GitHubCommitsForUser resolves its base URL
// via ResolveGitHubRequestAuth(cfg.GitHubBaseUrl, ...), so only the GitHub fields matter.
TrackerConfig CommitFeedConfig(const JiraCatalogHttpFixture& fx) {
    TrackerConfig cfg;
    cfg.GitHubBaseUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.GitHubPat = "feed-pat";
    cfg.GitCommitRepos = "o/r";
    return cfg;
}

nlohmann::json CommitEntry(const std::string& sha, const std::string& message, const std::string& date) {
    return nlohmann::json{
        {"sha", sha},
        {"html_url", "https://example.invalid/commit/" + sha},
        {"commit", {{"message", message}, {"author", {{"date", date}, {"name", "Alice"}}}}},
    };
}

} // namespace

TEST_CASE("GitHubCommitsForUser — unusable-source guards fail fast without HTTP") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg = CommitFeedConfig(fx);
    std::vector<Vcs::VcsSubmission> rows;
    std::string error;

    SUBCASE("empty PAT") {
        cfg.GitHubPat.clear();
        CHECK_FALSE(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 5, rows, error));
        CHECK(error == "GitHub PAT not configured");
    }
    SUBCASE("no repo list and no tracker owner/repo to fall back to") {
        cfg.GitCommitRepos.clear();
        CHECK_FALSE(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 5, rows, error));
        CHECK(error == "no git repos configured");
    }
    SUBCASE("empty author") {
        CHECK_FALSE(Vcs::GitHubCommitsForUser(cfg, "", 5, rows, error));
        CHECK(error == "empty git author");
    }
    CHECK(rows.empty());
    CHECK(fx.RequestCount("/repos/o/r/commits") == 0);
}

TEST_CASE("GitHubCommitsForUser — empty git_commit_repos falls back to the tracker's owner/repo") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg = CommitFeedConfig(fx);
    cfg.GitCommitRepos.clear();
    cfg.GitHubOwner = " o "; // trimmed before use
    cfg.GitHubRepo = "r";
    fx.ScriptJson("/repos/o/r/commits",
                  nlohmann::json::array({CommitEntry("aaa", "subject\nbody", "2026-07-01T10:00:00Z")}));

    std::vector<Vcs::VcsSubmission> rows;
    std::string error;
    CHECK(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 5, rows, error));
    CHECK(error.empty());
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].Id == "aaa");
    CHECK(rows[0].FirstLine == "subject");
    CHECK(rows[0].Author == "Alice");
}

TEST_CASE("GitHubCommitsForUser — author reaches the wire URL-encoded; maxN<=0 caps per_page at 1") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg = CommitFeedConfig(fx);
    std::string seenAuthor;
    std::string seenPerPage;
    fx.ScriptHandler("/repos/o/r/commits", [&](const httplib::Request& req) {
        seenAuthor = req.get_param_value("author");
        seenPerPage = req.get_param_value("per_page");
        return nlohmann::json::array();
    });

    std::vector<Vcs::VcsSubmission> rows;
    std::string error;
    CHECK(Vcs::GitHubCommitsForUser(cfg, "dev name@example.com", 0, rows, error));
    // httplib decodes query params, so an exact round-trip proves UrlEncode was applied.
    CHECK(seenAuthor == "dev name@example.com");
    CHECK(seenPerPage == "1");
    CHECK(rows.empty());
}

TEST_CASE("GitHubCommitsForUser — malformed entries and per-repo HTTP failures are non-fatal") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg = CommitFeedConfig(fx);

    SUBCASE("a list of only malformed entries returns true with zero rows and every error appended") {
        cfg.GitCommitRepos = "noslash,/leading,trailing/";
        std::vector<Vcs::VcsSubmission> rows;
        std::string error;
        CHECK(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 5, rows, error));
        CHECK(rows.empty());
        CHECK(error == "bad repo entry 'noslash' (want owner/repo); "
                       "bad repo entry '/leading' (want owner/repo); "
                       "bad repo entry 'trailing/' (want owner/repo)");
    }
    SUBCASE("a failing repo appends its GitHub error message; the healthy repo's rows survive") {
        cfg.GitCommitRepos = "o/r,o2/r2";
        fx.ScriptJson("/repos/o/r/commits", nlohmann::json::array({CommitEntry("aaa", "ok", "2026-07-01T10:00:00Z")}));
        fx.ScriptRaw("/repos/o2/r2/commits", 502, R"({"message":"upstream sad"})", "application/json");
        std::vector<Vcs::VcsSubmission> rows;
        std::string error;
        CHECK(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 5, rows, error));
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].Id == "aaa");
        CHECK(error == "o2/r2: upstream sad");
    }
}

TEST_CASE("GitHubCommitsForUser — multi-repo feeds merge newest-first and truncate to maxN") {
    JiraCatalogHttpFixture fx;
    TrackerConfig cfg = CommitFeedConfig(fx);
    cfg.GitCommitRepos = "o/r,o2/r2";
    fx.ScriptJson("/repos/o/r/commits", nlohmann::json::array({CommitEntry("d1", "newest", "2026-07-04T00:00:00Z"),
                                                               CommitEntry("d3", "oldest", "2026-07-01T00:00:00Z")}));
    fx.ScriptJson("/repos/o2/r2/commits",
                  nlohmann::json::array({CommitEntry("d2", "middle", "2026-07-02T00:00:00Z"),
                                         CommitEntry("d4", "dropped by maxN", "2026-06-30T00:00:00Z")}));

    std::vector<Vcs::VcsSubmission> rows;
    std::string error;
    CHECK(Vcs::GitHubCommitsForUser(cfg, "alice@example.com", 3, rows, error));
    CHECK(error.empty());
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].Id == "d1");
    CHECK(rows[1].Id == "d2");
    CHECK(rows[2].Id == "d3");
}
