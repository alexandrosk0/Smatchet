#include <doctest/doctest.h>

#include "P4AnnotateParse.h"

#include <string>
#include <vector>

using P4AnnotateParse::ParseAnnotateTextLine;
using P4AnnotateParse::ParseLatestChangeFromChangesOutput;
using P4AnnotateParse::SplitLines;
using P4AnnotateParse::StripP4UserDomain;

TEST_CASE("ParseAnnotateTextLine: 4 annotate-line shapes") {
    std::string cl;
    std::string user;
    std::string date;
    std::string code;

    SUBCASE("rev user: code (no date)") {
        const bool ok = ParseAnnotateTextLine("12345 alice: int x = 0;", cl, user, date, code);
        CHECK(ok);
        CHECK(cl == "12345");
        CHECK(user == "alice");
        CHECK(date == "");
        CHECK(code == "int x = 0;");
    }

    SUBCASE("rev: user YYYY/MM/DD code") {
        const bool ok = ParseAnnotateTextLine("42: bob 2026/01/15 return ok;", cl, user, date, code);
        CHECK(ok);
        CHECK(cl == "42");
        CHECK(user == "bob");
        CHECK(date == "2026/01/15");
        CHECK(code == "return ok;");
    }

    SUBCASE("rev user YYYY/MM/DD code (no colon)") {
        const bool ok = ParseAnnotateTextLine("7 carol 2025/12/31 // trailing", cl, user, date, code);
        CHECK(ok);
        CHECK(cl == "7");
        CHECK(user == "carol");
        CHECK(date == "2025/12/31");
        CHECK(code == "// trailing");
    }

    SUBCASE("rev: code (no user, no date) — single-token code") {
        // Multi-token code on the "rev: code" shape is captured by reRevUserDateCode (3-token
        // shape) first, so revOnlyColonCode only wins when the code is a single token (or empty).
        const bool ok = ParseAnnotateTextLine("99: x", cl, user, date, code);
        CHECK(ok);
        CHECK(cl == "99");
        CHECK(user == "");
        CHECK(date == "");
        CHECK(code == "x");
    }
}

TEST_CASE("ParseAnnotateTextLine: strips @domain from user") {
    std::string cl;
    std::string user;
    std::string date;
    std::string code;

    SUBCASE("user@host in rev user: code shape") {
        const bool ok = ParseAnnotateTextLine("1 alice@corp.io: x;", cl, user, date, code);
        CHECK(ok);
        CHECK(user == "alice");
    }

    SUBCASE("user@host in rev: user date code shape") {
        const bool ok = ParseAnnotateTextLine("1: alice@corp.io 2026/01/01 x;", cl, user, date, code);
        CHECK(ok);
        CHECK(user == "alice");
        CHECK(date == "2026/01/01");
    }
}

TEST_CASE("ParseAnnotateTextLine: malformed input returns false and clears CL / user / date") {
    std::string cl = "stale";
    std::string user = "stale";
    std::string date = "stale";
    std::string code;
    const bool ok = ParseAnnotateTextLine("not a p4 annotate line at all", cl, user, date, code);
    CHECK_FALSE(ok);
    CHECK(cl == "");
    CHECK(user == "");
    CHECK(date == "");
    // Contract: outCode defaults to raw line so callers can still display it.
    CHECK(code == "not a p4 annotate line at all");
}

TEST_CASE("ParseAnnotateTextLine: empty input returns false without crashing") {
    std::string cl;
    std::string user;
    std::string date;
    std::string code;
    const bool ok = ParseAnnotateTextLine("", cl, user, date, code);
    CHECK_FALSE(ok);
    CHECK(cl == "");
    CHECK(user == "");
    CHECK(date == "");
    CHECK(code == "");
}

TEST_CASE("ParseLatestChangeFromChangesOutput: Change N on DATE by USER") {
    SUBCASE("header parse + latest-change extraction") {
        const P4LineAnnotate b =
            ParseLatestChangeFromChangesOutput("Change 9876 on 2026/03/14 by alice@corp 'commit msg'\n", "");
        CHECK(b.Changelist == "9876");
        CHECK(b.User == "alice");
        CHECK(b.Approximate);
        CHECK(b.Error == "");
    }

    SUBCASE("multi-line output picks first match") {
        const std::string out = "Change 1000 on 2026/05/01 by bob@host 'first'\n"
                                "Change 999 on 2026/04/30 by carol@host 'second'\n";
        const P4LineAnnotate b = ParseLatestChangeFromChangesOutput(out, "");
        CHECK(b.Changelist == "1000");
        CHECK(b.User == "bob");
    }

    SUBCASE("malformed-header rejection falls back to stderr") {
        const P4LineAnnotate b = ParseLatestChangeFromChangesOutput("garbage header\n", "p4 says no\n");
        CHECK(b.Changelist == "");
        CHECK(b.User == "");
        CHECK(b.Approximate);
        CHECK(b.Error == "p4 says no\n");
    }

    SUBCASE("no match + empty stderr → canned error string") {
        const P4LineAnnotate b = ParseLatestChangeFromChangesOutput("nothing useful here", "");
        CHECK(b.Changelist == "");
        CHECK(b.Error == "p4 changes produced no match");
    }
}

TEST_CASE("ParseLatestChangeFromChangesOutput: empty input") {
    const P4LineAnnotate b = ParseLatestChangeFromChangesOutput("", "");
    CHECK(b.Changelist == "");
    CHECK(b.User == "");
    CHECK(b.Approximate);
    CHECK(b.Error == "p4 changes produced no match");
}

TEST_CASE("SplitLines: CRLF and LF") {
    SUBCASE("LF-only multi-line") {
        const std::vector<std::string> lines = SplitLines("alpha\nbeta\ngamma");
        REQUIRE(lines.size() == 3);
        CHECK(lines[0] == "alpha");
        CHECK(lines[1] == "beta");
        CHECK(lines[2] == "gamma");
    }

    SUBCASE("CRLF mixed in — CR consumed by TrimCopy") {
        const std::vector<std::string> lines = SplitLines("alpha\r\nbeta\r\ngamma\r\n");
        // Trailing newline produces no extra entry because the inner loop exits on i == s.size().
        REQUIRE(lines.size() == 3);
        CHECK(lines[0] == "alpha");
        CHECK(lines[1] == "beta");
        CHECK(lines[2] == "gamma");
    }

    SUBCASE("empty input") {
        const std::vector<std::string> lines = SplitLines("");
        CHECK(lines.empty());
    }

    SUBCASE("trailing-newline-absent: final partial line emitted") {
        const std::vector<std::string> lines = SplitLines("one\ntwo");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "one");
        CHECK(lines[1] == "two");
    }

    SUBCASE("double-newline preserves an empty middle line") {
        const std::vector<std::string> lines = SplitLines("first\n\nthird");
        REQUIRE(lines.size() == 3);
        CHECK(lines[0] == "first");
        CHECK(lines[1] == "");
        CHECK(lines[2] == "third");
    }

    SUBCASE("whitespace around tokens is trimmed") {
        const std::vector<std::string> lines = SplitLines("  padded  \n\t\ttabby\t");
        REQUIRE(lines.size() == 2);
        CHECK(lines[0] == "padded");
        CHECK(lines[1] == "tabby");
    }
}

TEST_CASE("StripP4UserDomain: bare user") {
    std::string user = "alice";
    StripP4UserDomain(user);
    CHECK(user == "alice");
}

TEST_CASE("StripP4UserDomain: user@domain") {
    std::string user = "bob@host";
    StripP4UserDomain(user);
    CHECK(user == "bob");
}

TEST_CASE("StripP4UserDomain: user@domain.with.dots") {
    std::string user = "carol@corp.example.io";
    StripP4UserDomain(user);
    CHECK(user == "carol");
}

TEST_CASE("StripP4UserDomain: empty input") {
    std::string user;
    StripP4UserDomain(user);
    CHECK(user == "");
}

TEST_CASE("StripP4UserDomain: leading @ → empty") {
    std::string user = "@onlydomain";
    StripP4UserDomain(user);
    CHECK(user == "");
}
