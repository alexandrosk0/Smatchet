// Gap-map Tier 5 `_detail` extraction for AnnotateAnalysisUi_Modals.cpp — the pure
// export/clipboard escaping, quick-comment template expansion, and P4-user -> Jira-account
// selection behind the annotate triage modals (audit finding #23's TU; the fix itself is in
// the shell's callback plumbing, this pins the formatting/matching logic around it).

#include "Ui/AnnotateAnalysisUi_Modals_detail.h"

#include "ConfigManager.h"
#include "P4Annotate.h"
#include "Tracker/TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace AnnotateUiPure;

namespace {

AnnotateRowView SampleRow() {
    AnnotateRowView v;
    v.Function = "Foo::Bar";
    v.Path = "//depot/src/foo.cpp";
    v.Line = 42;
    v.User = "jdoe";
    v.Changelist = "12345";
    v.Date = "2026-07-01";
    return v;
}

} // namespace

TEST_CASE("CsvEscape — quotes only when needed, doubles inner quotes") {
    CHECK(CsvEscape("plain") == "plain");
    CHECK(CsvEscape("a,b") == "\"a,b\"");
    CHECK(CsvEscape("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(CsvEscape("line1\nline2") == "\"line1\nline2\"");
    CHECK(CsvEscape("") == "");
}

TEST_CASE("NormalizeDateDisplay — ISO dashes to slashes, everything else verbatim") {
    CHECK(NormalizeDateDisplay("2026-07-01T10:00:00Z") == "2026/07/01");
    CHECK(NormalizeDateDisplay("2026/07/01") == "2026/07/01");
    CHECK(NormalizeDateDisplay("yesterday") == "yesterday");
    CHECK(NormalizeDateDisplay("") == "");
}

TEST_CASE("TSV rows — sanitized cells, 1-based display index, stable column order") {
    P4AnnotatedLine ln;
    ln.SourceLine = 7;
    ln.Changelist = "111";
    ln.User = "alice";
    ln.Date = "2026/07/01";
    ln.Code = "if (a\tb) {\n}";
    CHECK(BuildAnnotatedRowTsv(ln) == "7\t111\talice\t2026/07/01\tif (a b) { }");

    CHECK(BuildCallstackRowTsv(SampleRow(), 0) == "1\tFoo::Bar\t//depot/src/foo.cpp:42\tjdoe\t12345\t2026-07-01");
}

TEST_CASE("quick-comment templates — configured template wins, builtin fallbacks, placeholders") {
    const AnnotateRowView row = SampleRow();
    std::vector<CommentTemplate> templates;
    CommentTemplate custom;
    custom.Id = "need_repro";
    custom.Text = "Custom for {issueKey} at {path}:{line} by {user} ({cl}) in {function}";
    templates.push_back(custom);

    {
        const std::string t = BuildQuickCommentText("PROJ-1", "need_repro", row, templates);
        CHECK(t == "Custom for PROJ-1 at //depot/src/foo.cpp:42 by jdoe (12345) in Foo::Bar");
    }
    {
        // No configured match: need_logs falls back to its builtin text.
        const std::string t = BuildQuickCommentText("PROJ-1", "need_logs", row, templates);
        CHECK(t.find("Please attach logs/diagnostics for PROJ-1") == 0);
        CHECK(t.find("Foo::Bar @ //depot/src/foo.cpp:42") != std::string::npos);
    }
    {
        // Unknown id: generic triage-handoff shape.
        const std::string t = BuildQuickCommentText("PROJ-1", "nosuch", row, templates);
        CHECK(t.find("Triage handoff for PROJ-1") == 0);
        CHECK(t.find("Suggested owner: jdoe") != std::string::npos);
        CHECK(t.find("CL: 12345") != std::string::npos);
    }
}

TEST_CASE("ReplaceStringPlaceholder — every occurrence, no recursion into replacements") {
    CHECK(ReplaceStringPlaceholder("{k} and {k}", "{k}", "x") == "x and x");
    // A replacement containing the placeholder must not loop forever.
    CHECK(ReplaceStringPlaceholder("{k}", "{k}", "{k}!") == "{k}!");
    CHECK(ReplaceStringPlaceholder("none", "{k}", "x") == "none");
}

TEST_CASE("PickJiraAccountForP4User — email local-part beats first-result fallback") {
    std::vector<TrackerUser> users;
    TrackerUser first;
    first.AccountId = "acc-first";
    first.DisplayName = "First Result";
    first.EmailAddress = "someone.else@example.com";
    users.push_back(first);
    TrackerUser exact;
    exact.AccountId = "acc-exact";
    exact.DisplayName = "J Doe";
    exact.EmailAddress = "JDoe@example.com";
    users.push_back(exact);

    std::string accountId;
    std::string err;
    {
        // Case-insensitive local-part match wins over the earlier row.
        REQUIRE(PickJiraAccountForP4User(users, "jdoe", accountId, err));
        CHECK(accountId == "acc-exact");
        CHECK(err.empty());
    }
    {
        // No local-part match: the first search result is the fallback.
        REQUIRE(PickJiraAccountForP4User(users, "zz", accountId, err));
        CHECK(accountId == "acc-first");
    }
    {
        REQUIRE_FALSE(PickJiraAccountForP4User({}, "jdoe", accountId, err));
        CHECK(accountId.empty());
        CHECK(err == "No Jira user match.");
    }
    {
        // A user with an email that has no '@' matches on the whole address.
        std::vector<TrackerUser> bare;
        TrackerUser u;
        u.AccountId = "acc-bare";
        u.EmailAddress = "jdoe";
        bare.push_back(u);
        REQUIRE(PickJiraAccountForP4User(bare, "JDOE", accountId, err));
        CHECK(accountId == "acc-bare");
    }
}

TEST_CASE("GroupLookupErrorMessage — a failure is never silent, even with no detail (#2064)") {
    // The user-profile modal only renders an error when the string is non-empty, so passing a
    // backend Result's raw error through meant an empty-Detail failure drew an empty group list
    // and nothing else — indistinguishable from "this user is in no groups". An empty Detail is
    // a real backend outcome, pinned by CollaborationPreconditionPure.test.cpp.
    CHECK(GroupLookupErrorMessage("") == "Group lookup failed.");
    CHECK_FALSE(GroupLookupErrorMessage("").empty());

    // A real detail is passed through verbatim — the fallback must not mask a better message.
    CHECK(GroupLookupErrorMessage("403 Forbidden") == "403 Forbidden");
    // Whitespace is not emptiness: no guessing about what a backend considers blank.
    CHECK(GroupLookupErrorMessage(" ") == " ");
}
