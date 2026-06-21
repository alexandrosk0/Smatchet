#include <doctest/doctest.h>

#include "Tracker/ProjectResolver.h"

#include <string>

// Coverage ramp (global-floor 65->70) — exercise the pure free-text issue-key
// scan half of ProjectResolver that ProjectComponentResolution.test.cpp does not
// touch (it only drives ExtractIssueKeyPrefix). FindFirstIssueKeyInText is the
// "add to query" / annotate free-text scanner: a maximal-token walk that defers
// key validation to ExtractIssueKeyPrefix (single-source key grammar). Pure C++14,
// no client / cpr / sqlite — ProjectResolver.cpp is already linked into the rig.

using smatchet::ExtractIssueKeyPrefix;
using smatchet::FindFirstIssueKeyInText;
using smatchet::IssueKeyMatch;

TEST_CASE("ExtractIssueKeyPrefix: accepts a well-formed KEY-123 and returns the prefix") {
    CHECK(ExtractIssueKeyPrefix("PROJ-123") == "PROJ");
    CHECK(ExtractIssueKeyPrefix("A-1") == "A");
    // Underscore and digits are legal inside the prefix (after the leading letter).
    CHECK(ExtractIssueKeyPrefix("TEAM_2-7") == "TEAM_2");
}

TEST_CASE("ExtractIssueKeyPrefix: rejects non-key shapes") {
    CHECK(ExtractIssueKeyPrefix("").empty());           // empty
    CHECK(ExtractIssueKeyPrefix("PROJ").empty());        // no dash
    CHECK(ExtractIssueKeyPrefix("-123").empty());        // dash first
    CHECK(ExtractIssueKeyPrefix("1PROJ-2").empty());     // prefix starts with a digit
    CHECK(ExtractIssueKeyPrefix("PR OJ-2").empty());     // space in prefix
    CHECK(ExtractIssueKeyPrefix("PROJ-").empty());       // empty number suffix
    CHECK(ExtractIssueKeyPrefix("PROJ-foo").empty());    // non-digit suffix
    // A UUID must NOT be mistaken for a key (digit-leading second group, hex suffix).
    CHECK(ExtractIssueKeyPrefix("550e8400-e29b").empty());
}

TEST_CASE("FindFirstIssueKeyInText: finds the first key embedded in free text with offsets") {
    const std::string text = "see PROJ-123 for details";
    const IssueKeyMatch m = FindFirstIssueKeyInText(text);
    CHECK(m.Key == "PROJ-123");
    CHECK(m.Start == 4);
    CHECK(m.End == 12);
    // Offsets bracket exactly the key token.
    CHECK(text.substr(m.Start, m.End - m.Start) == "PROJ-123");
}

TEST_CASE("FindFirstIssueKeyInText: returns the FIRST key when several are present") {
    const IssueKeyMatch m = FindFirstIssueKeyInText("noise ABC-1 then DEF-2");
    CHECK(m.Key == "ABC-1");
    CHECK(m.Start == 6);
}

TEST_CASE("FindFirstIssueKeyInText: a key at the very start of the text") {
    const IssueKeyMatch m = FindFirstIssueKeyInText("ZED-9 trailing");
    CHECK(m.Key == "ZED-9");
    CHECK(m.Start == 0);
    CHECK(m.End == 5);
}

TEST_CASE("FindFirstIssueKeyInText: no key -> empty match with Start==End") {
    const IssueKeyMatch m = FindFirstIssueKeyInText("nothing key-like here, just 550e8400-e29b");
    CHECK(m.Key.empty());
    CHECK(m.Start == m.End);
}

TEST_CASE("FindFirstIssueKeyInText: empty text -> empty match") {
    const IssueKeyMatch m = FindFirstIssueKeyInText("");
    CHECK(m.Key.empty());
    CHECK(m.Start == 0);
    CHECK(m.End == 0);
}

TEST_CASE("FindFirstIssueKeyInText: skips key-shaped-but-invalid tokens to reach a real key") {
    // "PROJ-foo" tokenizes as one token (letters/digits/_/-), fails validation, and the
    // scan must continue to the genuine "REAL-7" rather than stopping at the decoy.
    const IssueKeyMatch m = FindFirstIssueKeyInText("PROJ-foo and REAL-7");
    CHECK(m.Key == "REAL-7");
}
