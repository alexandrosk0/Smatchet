#include <doctest/doctest.h>

#include "P4AnnotateParse.h"
#include "StringUtil.h"
#include "Vcs/GitHubCommitsParse.h"
#include "Vcs/VcsSubmission.h"

#include <string>
#include <vector>

using P4AnnotateParse::ParseChangesForUserOutput;

// --- p4 `changes -u` parse (Slice 1, spec 6.2) ---

TEST_CASE("ParseChangesForUserOutput: summary lines, domain strip, noise skipped") {
    const std::string out = "Change 12345 on 2026/06/01 by alice@ws-main 'Fix the grid sort'\n"
                            "some unrelated noise line\n"
                            "Change 12001 on 2026/05/28 by alice@ws-main 'Long description truncated by p4\n";
    const std::vector<P4ChangeSummary> changes = ParseChangesForUserOutput(out);
    REQUIRE(changes.size() == 2);
    CHECK(changes[0].Changelist == "12345");
    CHECK(changes[0].Date == "2026/06/01");
    CHECK(changes[0].User == "alice");
    CHECK(changes[0].Description == "Fix the grid sort");
    // Trailing quote optional — p4 truncates long descriptions mid-quote.
    CHECK(changes[1].Changelist == "12001");
    CHECK(changes[1].Description == "Long description truncated by p4");
}

TEST_CASE("ParseChangesForUserOutput: empty / no-match input yields empty") {
    CHECK(ParseChangesForUserOutput("").empty());
    CHECK(ParseChangesForUserOutput("p4 changes: no such user.\n").empty());
}

// --- P4UserFromEmail (spec 6.1, identity edge-case §11) ---

TEST_CASE("P4UserFromEmail: local-part, no-@ passthrough, empty") {
    CHECK(Vcs::P4UserFromEmail("alice@corp.io") == "alice");
    CHECK(Vcs::P4UserFromEmail("bob") == "bob");
    CHECK(Vcs::P4UserFromEmail("") == "");
    // First @ wins on malformed doubles.
    CHECK(Vcs::P4UserFromEmail("a@b@c") == "a");
}

// --- FromP4Change date normalisation ---

TEST_CASE("FromP4Change: p4 date normalised to ISO, other shapes passed through") {
    P4ChangeSummary c;
    c.Changelist = "777";
    c.Date = "2026/06/01";
    c.User = "alice";
    c.Description = "desc first line";

    const Vcs::VcsSubmission s = Vcs::FromP4Change(c);
    CHECK(s.Source == Vcs::VcsSource::P4);
    CHECK(s.Id == "777");
    CHECK(s.Timestamp == "2026-06-01");
    CHECK(s.Author == "alice");
    CHECK(s.FirstLine == "desc first line");
    CHECK(s.Url == "");

    SUBCASE("non-p4-shaped date passes through unchanged") {
        c.Date = "yesterday";
        CHECK(Vcs::FromP4Change(c).Timestamp == "yesterday");
        c.Date = "2026/6/1"; // not exactly 10 chars
        CHECK(Vcs::FromP4Change(c).Timestamp == "2026/6/1");
        c.Date = "";
        CHECK(Vcs::FromP4Change(c).Timestamp == "");
    }
}

// --- KeepOnOrAfterCutoff (spec 7.2) ---

TEST_CASE("KeepOnOrAfterCutoff: leading-10-char compare, short timestamps kept") {
    const std::string cutoff = "2026-05-10";
    CHECK(Vcs::KeepOnOrAfterCutoff("2026-05-10", cutoff));           // on cutoff
    CHECK(Vcs::KeepOnOrAfterCutoff("2026-05-11", cutoff));           // after
    CHECK_FALSE(Vcs::KeepOnOrAfterCutoff("2026-05-09", cutoff));     // before
    CHECK(Vcs::KeepOnOrAfterCutoff("2026-05-10T00:00:00Z", cutoff)); // ISO datetime, leading 10 only
    CHECK_FALSE(Vcs::KeepOnOrAfterCutoff("2026-05-09T23:59:59Z", cutoff));
    // Shorter than 10 chars — source date missing/unparseable — kept unconditionally.
    CHECK(Vcs::KeepOnOrAfterCutoff("", cutoff));
    CHECK(Vcs::KeepOnOrAfterCutoff("yesterday", cutoff));
}

// --- ParseGitHubCommitListJson (Slice 2, bucket-A) ---

TEST_CASE("ParseGitHubCommitListJson: happy path + author fallback + first line") {
    const std::string body = R"([
        {
            "sha": "abc123",
            "html_url": "https://github.com/o/r/commit/abc123",
            "commit": {
                "message": "feat: add thing\r\n\r\nbody detail",
                "author": {"name": "Alice A", "date": "2026-06-01T12:34:56Z"}
            },
            "author": {"login": "alice-gh"}
        },
        {
            "sha": "def456",
            "html_url": "https://github.com/o/r/commit/def456",
            "commit": {
                "message": "fix: one-liner",
                "author": {"name": "", "date": "2026-05-30T08:00:00Z"}
            },
            "author": {"login": "bob-gh"}
        }
    ])";
    const std::vector<Vcs::VcsSubmission> rows = Vcs::ParseGitHubCommitListJson(body);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].Source == Vcs::VcsSource::Git);
    CHECK(rows[0].Id == "abc123");
    CHECK(rows[0].Url == "https://github.com/o/r/commit/abc123");
    CHECK(rows[0].Timestamp == "2026-06-01T12:34:56Z");
    CHECK(rows[0].Author == "Alice A");
    // First line only, trailing \r stripped.
    CHECK(rows[0].FirstLine == "feat: add thing");
    // Empty commit.author.name falls back to top-level author.login.
    CHECK(rows[1].Author == "bob-gh");
    CHECK(rows[1].FirstLine == "fix: one-liner");
}

TEST_CASE("ParseGitHubCommitListJson: malformed rows skipped, bad bodies yield empty") {
    SUBCASE("rows without sha or non-object entries are skipped") {
        const std::string body = R"([
            {"html_url": "https://x", "commit": {"message": "no sha"}},
            42,
            {"sha": "keepme", "commit": {"message": "kept"}}
        ])";
        const std::vector<Vcs::VcsSubmission> rows = Vcs::ParseGitHubCommitListJson(body);
        REQUIRE(rows.size() == 1);
        CHECK(rows[0].Id == "keepme");
        CHECK(rows[0].FirstLine == "kept");
        CHECK(rows[0].Author == "");
        CHECK(rows[0].Timestamp == "");
    }
    SUBCASE("non-array body") { CHECK(Vcs::ParseGitHubCommitListJson(R"({"message": "Bad credentials"})").empty()); }
    SUBCASE("invalid JSON") { CHECK(Vcs::ParseGitHubCommitListJson("<html>oops</html>").empty()); }
    SUBCASE("empty body") { CHECK(Vcs::ParseGitHubCommitListJson("").empty()); }
}

// --- MergeFeedsNewestFirst (spec 7.1) ---

namespace {

Vcs::VcsSubmission Row(Vcs::VcsSource src, const std::string& id, const std::string& ts) {
    Vcs::VcsSubmission s;
    s.Source = src;
    s.Id = id;
    s.Timestamp = ts;
    return s;
}

} // namespace

TEST_CASE("MergeFeedsNewestFirst: interleaves two newest-first feeds") {
    std::vector<Vcs::VcsSubmission> p4;
    p4.push_back(Row(Vcs::VcsSource::P4, "300", "2026-06-03"));
    p4.push_back(Row(Vcs::VcsSource::P4, "100", "2026-06-01"));
    std::vector<Vcs::VcsSubmission> git;
    git.push_back(Row(Vcs::VcsSource::Git, "g2", "2026-06-02T10:00:00Z"));
    git.push_back(Row(Vcs::VcsSource::Git, "g0", "2026-05-30T10:00:00Z"));

    const std::vector<Vcs::VcsSubmission> merged = Vcs::MergeFeedsNewestFirst(p4, git);
    REQUIRE(merged.size() == 4);
    CHECK(merged[0].Id == "300");
    CHECK(merged[1].Id == "g2");
    CHECK(merged[2].Id == "100");
    CHECK(merged[3].Id == "g0");
}

TEST_CASE("MergeFeedsNewestFirst: stable on ties — rows from a precede rows from b") {
    std::vector<Vcs::VcsSubmission> a;
    a.push_back(Row(Vcs::VcsSource::P4, "a1", "2026-06-01"));
    a.push_back(Row(Vcs::VcsSource::P4, "a2", "2026-06-01"));
    std::vector<Vcs::VcsSubmission> b;
    b.push_back(Row(Vcs::VcsSource::Git, "b1", "2026-06-01"));

    const std::vector<Vcs::VcsSubmission> merged = Vcs::MergeFeedsNewestFirst(a, b);
    REQUIRE(merged.size() == 3);
    CHECK(merged[0].Id == "a1");
    CHECK(merged[1].Id == "a2");
    CHECK(merged[2].Id == "b1");

    SUBCASE("empty inputs") {
        CHECK(Vcs::MergeFeedsNewestFirst({}, {}).empty());
        CHECK(Vcs::MergeFeedsNewestFirst(a, {}).size() == 2);
        CHECK(Vcs::MergeFeedsNewestFirst({}, b).size() == 1);
    }
}

// --- production-group keyword match (Slice 1 config; spec case 11) ---

TEST_CASE("ContainsCaseInsensitive: production-group keyword match") {
    CHECK(ContainsCaseInsensitive("Production-Engineers", "prod"));
    CHECK(ContainsCaseInsensitive("ART-PROD-TEAM", "prod"));
    CHECK_FALSE(ContainsCaseInsensitive("Designers", "prod"));
    // Empty keyword disables the highlight at the call site; the helper itself
    // treats empty-needle as contained — pin that so the call-site guard stays.
    CHECK(ContainsCaseInsensitive("anything", ""));
}
