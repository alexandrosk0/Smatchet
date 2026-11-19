// Bucket-A doctest cases for the shared matcher (Source/Core/include/TextMatchPure.h,
// docs/plans/input-ux-w2-one-matcher.md). Included by exactly one registered test TU
// (tests/Core/TicketGridFilterPure.test.cpp, which runs in both the Linux subset and SmatchetTests):
// a tests/Core/TextMatchPure.test.cpp of its own needs tests/CMakeLists.txt, plan-locked when this
// landed (debt 2026-11-19-one-matcher-deferred). Move these into one when the lock clears.

#pragma once

#include "TextMatchPure.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace textmatch_cases {

using smatchet::textmatch::ByteRange;
using smatchet::textmatch::Field;
using smatchet::textmatch::MakeQuery;
using smatchet::textmatch::Match;
using smatchet::textmatch::MatchFields;
using smatchet::textmatch::Mode;
using smatchet::textmatch::Query;
using smatchet::textmatch::RankKey;
using smatchet::textmatch::Result;
using smatchet::textmatch::Scratch;

inline Result Fuzzy(const char* query, const std::string& text) { return Match(MakeQuery(query), text, Mode::Fuzzy); }
inline Result Exact(const char* query, const std::string& text) {
    return Match(MakeQuery(query), text, Mode::Substring);
}

// The matched text of each range, for readable assertions.
inline std::vector<std::string> MatchedParts(const char* query, const std::string& text) {
    std::vector<ByteRange> ranges;
    Match(MakeQuery(query), text, Mode::Fuzzy, &ranges);
    std::vector<std::string> parts;
    for (const ByteRange& r : ranges) {
        parts.push_back(text.substr(r.Begin, r.End - r.Begin));
    }
    return parts;
}

TEST_CASE("TextMatch — FoldCodepoint folds the regular blocks and leaves the rest alone") {
    using smatchet::textmatch::FoldCodepoint;
    CHECK(FoldCodepoint(U'A') == U'a');
    CHECK(FoldCodepoint(U'z') == U'z');
    CHECK(FoldCodepoint(U'@') == U'@');
    CHECK(FoldCodepoint(0xC9) == 0xE9);   // É -> é
    CHECK(FoldCodepoint(0xD7) == 0xD7);   // multiplication sign is not a letter
    CHECK(FoldCodepoint(0xDF) == 0xDF);   // ß has no simple upper-lower pair
    CHECK(FoldCodepoint(0xB5) == 0x3BC);  // micro sign -> mu
    CHECK(FoldCodepoint(0x100) == 0x101); // Ā -> ā
    CHECK(FoldCodepoint(0x101) == 0x101);
    CHECK(FoldCodepoint(0x139) == 0x13A);   // Ĺ -> ĺ (odd upper run)
    CHECK(FoldCodepoint(0x130) == 0x130);   // İ: no simple fold
    CHECK(FoldCodepoint(0x178) == 0xFF);    // Ÿ -> ÿ
    CHECK(FoldCodepoint(0x17D) == 0x17E);   // Ž -> ž
    CHECK(FoldCodepoint(0x391) == 0x3B1);   // Α -> α
    CHECK(FoldCodepoint(0x3A3) == 0x3C3);   // Σ -> σ
    CHECK(FoldCodepoint(0x3C2) == 0x3C3);   // final ς -> σ
    CHECK(FoldCodepoint(0x386) == 0x3AC);   // Ά -> ά
    CHECK(FoldCodepoint(0x38F) == 0x3CE);   // Ώ -> ώ
    CHECK(FoldCodepoint(0x3A2) == 0x3A2);   // unassigned gap in the capitals
    CHECK(FoldCodepoint(0x370) == 0x371);   // Heta
    CHECK(FoldCodepoint(0x37F) == 0x3F3);   // yot
    CHECK(FoldCodepoint(0x3D0) == 0x3B2);   // beta symbol
    CHECK(FoldCodepoint(0x3F4) == 0x3B8);   // capital theta symbol
    CHECK(FoldCodepoint(0x3F9) == 0x3F2);   // capital lunate sigma
    CHECK(FoldCodepoint(0x3FD) == 0x37B);   // reversed lunate sigma
    CHECK(FoldCodepoint(0x3F2) == 0x3F2);   // already lower case
    CHECK(FoldCodepoint(0x1E9B) == 0x1E61); // long s with dot above
    CHECK(FoldCodepoint(0x401) == 0x451);   // Ё -> ё
    CHECK(FoldCodepoint(0x42F) == 0x44F);   // Я -> я
    CHECK(FoldCodepoint(0x460) == 0x461);   // Ѡ -> ѡ
    CHECK(FoldCodepoint(0x4C1) == 0x4C2);   // Ӂ -> ӂ
    CHECK(FoldCodepoint(0x4C0) == 0x4CF);   // palochka
    CHECK(FoldCodepoint(0x531) == 0x561);   // Armenian Ա -> ա
    CHECK(FoldCodepoint(0x1E9E) == 0xDF);   // ẞ -> ß
    CHECK(FoldCodepoint(0x1EA0) == 0x1EA1);
    CHECK(FoldCodepoint(0xFF21) == 0xFF41); // fullwidth A
    CHECK(FoldCodepoint(0x4E2D) == 0x4E2D); // CJK unchanged
}

TEST_CASE("TextMatch — FoldCodepoint is idempotent") {
    using smatchet::textmatch::FoldCodepoint;
    for (char32_t c = 0; c < 0x20000; ++c) {
        const char32_t once = FoldCodepoint(c);
        if (FoldCodepoint(once) != once) {
            FAIL_CHECK("fold of fold differs at U+" << static_cast<unsigned>(c));
        }
    }
}

TEST_CASE("TextMatch — DecodeUtf8 never reads past the end and rejects malformed sequences") {
    using smatchet::textmatch::DecodeUtf8;
    using smatchet::textmatch::kInvalidByteBase;
    std::size_t len = 0;
    CHECK(DecodeUtf8("\xCE\xB1", 2, len) == 0x3B1);
    CHECK(len == 2);
    CHECK(DecodeUtf8("\xE2\x82\xAC", 3, len) == 0x20AC);
    CHECK(len == 3);
    CHECK(DecodeUtf8("\xF0\x9F\x98\x80", 4, len) == 0x1F600);
    CHECK(len == 4);
    // Truncated: the lead byte alone is all that may be read.
    CHECK(DecodeUtf8("\xCE\xB1", 1, len) == kInvalidByteBase + 0xCE);
    CHECK(len == 1);
    CHECK(DecodeUtf8("\xE2\x82", 2, len) == kInvalidByteBase + 0xE2);
    CHECK(len == 1);
    CHECK(DecodeUtf8("\x80", 1, len) == kInvalidByteBase + 0x80);             // stray continuation
    CHECK(DecodeUtf8("\xC0\x80", 2, len) == kInvalidByteBase + 0xC0);         // overlong NUL
    CHECK(DecodeUtf8("\xED\xA0\x80", 3, len) == kInvalidByteBase + 0xED);     // surrogate
    CHECK(DecodeUtf8("\xF4\x90\x80\x80", 4, len) == kInvalidByteBase + 0xF4); // above U+10FFFF
    CHECK(len == 1);
}

TEST_CASE("TextMatch — FoldCopy round-trips and keeps invalid bytes") {
    using smatchet::textmatch::FoldCopy;
    CHECK(FoldCopy("HeLLo") == "hello");
    CHECK(FoldCopy("\xCE\x91\xCE\xBB\xCE\xAD\xCE\xBE") == "\xCE\xB1\xCE\xBB\xCE\xAD\xCE\xBE"); // Αλέξ -> αλέξ
    CHECK(FoldCopy("\xD0\x9F\xD1\x80\xD0\x98") == "\xD0\xBF\xD1\x80\xD0\xB8");                 // ПрИ -> при
    CHECK(FoldCopy("ab\xC3") == "ab\xC3");       // a lone lead byte at the end is kept, not read past
    CHECK(FoldCopy("\xFF\xFEX") == "\xFF\xFEx"); // invalid bytes kept
    CHECK(FoldCopy(std::string()) == "");
    CHECK(FoldCopy(nullptr, 4) == "");
}

TEST_CASE("TextMatch — FindFolded compares through the fold without allocating") {
    using smatchet::textmatch::FindFolded;
    using smatchet::textmatch::FoldCopy;
    CHECK(FindFolded(std::string("Login Page"), "page") == 6);
    CHECK(FindFolded(std::string("Login Page"), "") == 0);
    CHECK(FindFolded(std::string("Login Page"), "pages") == std::string::npos);
    CHECK(FindFolded(std::string("x Login login"), "login", 3) == 8);
    CHECK(FindFolded(std::string("abc"), "a", 9) == std::string::npos);
    const std::string greek =
        "\xCE\x91\xCE\xBB\xCE\xAD\xCE\xBE\xCE\xB1\xCE\xBD\xCE\xB4\xCF\x81\xCE\xBF\xCF\x82"; // Αλέξανδρος
    CHECK(FindFolded(greek, FoldCopy("\xCE\xB1\xCE\xBB\xCE\xAD\xCE\xBE")) == 0);            // αλέξ
    CHECK(FindFolded(greek, FoldCopy("\xCE\xA1\xCE\x9F\xCE\xA3")) == 14);                   // ΡΟΣ, final sigma folds
    CHECK(FindFolded(std::string("stra\xC5\xBF"
                                 "se"),
                     "strasse") == 0); // long s folds to s
    CHECK(FindFolded(nullptr, 5, "a") == std::string::npos);
}

TEST_CASE("TextMatch — MakeQuery splits on whitespace, folds and caps") {
    const Query q = MakeQuery("  Login\tCRASH \xC2\xA0 \xCE\x91 ");
    REQUIRE(q.Words.size() == 3);
    CHECK(q.WordsUtf8[0] == "login");
    CHECK(q.WordsUtf8[1] == "crash");
    CHECK(q.WordsUtf8[2] == "\xCE\xB1");
    CHECK(MakeQuery("").Empty());
    CHECK(MakeQuery(" \t\r\n ").Empty());
    CHECK(MakeQuery(static_cast<const char*>(nullptr)).Empty());

    std::string many;
    for (int i = 0; i < 40; ++i) {
        many += "w ";
    }
    CHECK(MakeQuery(many).Words.size() == smatchet::textmatch::kMaxQueryWords);
    const std::string longWord(300, 'a');
    CHECK(MakeQuery(longWord).Words[0].size() == smatchet::textmatch::kMaxWordCodepoints);
}

TEST_CASE("TextMatch — words match in any order, each anywhere, case-folded") {
    using textmatch_cases::Exact;
    using textmatch_cases::Fuzzy;
    CHECK(Exact("login crash", "Crash on the login page").Matched());
    CHECK(Exact("crash login", "Crash on the login page").Matched());
    CHECK(Exact("LOGIN", "login page").Matched());
    CHECK_FALSE(Exact("login signup", "Crash on the login page").Matched());
    CHECK(Exact("", "anything").Matched()); // an empty query matches everything
    CHECK(Exact("\xCE\xB1\xCE\xBB\xCE\xAD\xCE\xBE", "\xCE\x91\xCE\x9B\xCE\x88\xCE\x9E").Matched()); // αλέξ vs ΑΛΈΞ
    CHECK(
        Exact("\xD0\xBF\xD1\x80\xD0\xBE\xD0\xB5\xD0\xBA\xD1\x82", "\xD0\x9F\xD0\xA0\xD0\x9E\xD0\x95\xD0\x9A\xD0\xA2 X")
            .Matched()); // проект vs ПРОЕКТ
    CHECK(Fuzzy("refresh", "Refresh tracker").Substring);
}

TEST_CASE("TextMatch — Substring mode needs every word as written; Fuzzy adds letters in order") {
    using textmatch_cases::Exact;
    using textmatch_cases::Fuzzy;
    CHECK_FALSE(Exact("prj", "Project X").Matched());
    const Result fuzzy = Fuzzy("prj", "Project X");
    CHECK(fuzzy.Matched());
    CHECK_FALSE(fuzzy.Substring);
    CHECK_FALSE(Fuzzy("jrp", "Project X").Matched()); // out of order
    CHECK_FALSE(Fuzzy("zzz", "Project X").Matched());
    // A word that occurs as written makes the whole result an exact one only when every word does.
    const Result mixed = Fuzzy("project xq", "Project X quick");
    CHECK(mixed.Matched());
    CHECK_FALSE(mixed.Substring);
}

TEST_CASE("TextMatch — an exact match outranks any letters-in-order match") {
    using textmatch_cases::Fuzzy;
    const Result exact = Fuzzy("io", "radio");
    const Result fuzzy = Fuzzy("io", "Inside Order");
    REQUIRE(exact.Matched());
    REQUIRE(fuzzy.Matched());
    CHECK(exact.Substring);
    CHECK_FALSE(fuzzy.Substring);
    CHECK(RankKey(exact) > RankKey(fuzzy));
    CHECK(RankKey(Result()) == 0);
    CHECK(smatchet::textmatch::RowListed(exact, true));
    CHECK_FALSE(smatchet::textmatch::RowListed(fuzzy, true));
    CHECK(smatchet::textmatch::RowListed(fuzzy, false));
    CHECK_FALSE(smatchet::textmatch::RowListed(Result(), false));
}

TEST_CASE("TextMatch — ranking: start, word starts, whole words and the best alignment win") {
    using textmatch_cases::Fuzzy;
    // The whole candidate beats the same word deeper inside a longer one.
    CHECK(Fuzzy("refresh", "refresh").Score > Fuzzy("refresh", "view.list.refresh.all.panes").Score);
    // A word start beats a match in the middle of a word.
    CHECK(Fuzzy("log", "Open log").Score > Fuzzy("log", "Catalog").Score);
    // Initials: the word starts of "Toggle Activity Log" beat scattered letters.
    CHECK(Fuzzy("tal", "Toggle Activity Log").Score > Fuzzy("tal", "Total").Score);
    // camelCase starts count as word starts.
    CHECK(Fuzzy("gs", "gridSearch").Score > Fuzzy("gs", "guess").Score);
    // Same shape, same score: the caller's order decides.
    CHECK(Fuzzy("run", "a.run").Score == Fuzzy("run", "b.run").Score);
}

TEST_CASE("TextMatch — matched byte ranges cover the matched letters") {
    using textmatch_cases::MatchedParts;
    CHECK(MatchedParts("log", "Open log") == std::vector<std::string>{"log"});
    // The best occurrence, not the first: the whole word after the mid-word one.
    std::vector<ByteRange> ranges;
    Match(MakeQuery("view"), std::string("overview view"), Mode::Fuzzy, &ranges);
    REQUIRE(ranges.size() == 1);
    CHECK(ranges[0].Begin == 9);
    CHECK(MatchedParts("prj", "Project") == std::vector<std::string>{"Pr", "j"});
    CHECK(MatchedParts("crash login", "Login crash") == std::vector<std::string>{"Login", "crash"});
    // A multi-byte letter is covered whole.
    CHECK(MatchedParts("\xCE\xB1", "x\xCE\x91y") == std::vector<std::string>{"\xCE\x91"});
    // Overlapping words merge into one range.
    CHECK(MatchedParts("log login", "login") == std::vector<std::string>{"login"});
    CHECK(MatchedParts("", "anything").empty());
    CHECK(MatchedParts("zzz", "anything").empty());
}

TEST_CASE("TextMatch — MatchFields: every word in some field, the best field takes it") {
    const std::string title = "Toggle activity log";
    const std::string name = "view.toggle.activity_log";
    const std::string summary = "Show or hide the log window";
    Field fields[3];
    fields[0].Text = title.data();
    fields[0].Len = title.size();
    fields[0].Weight = 3;
    fields[1].Text = name.data();
    fields[1].Len = name.size();
    fields[1].Weight = 2;
    fields[2].Text = summary.data();
    fields[2].Len = summary.size();
    fields[2].Weight = 1;
    Scratch scratch;
    std::vector<ByteRange> ranges[3];

    // "window" only occurs in the summary; "toggle" is taken by the title (heavier).
    const Result r = MatchFields(MakeQuery("toggle window"), fields, 3, Mode::Fuzzy, scratch, ranges);
    CHECK(r.Matched());
    CHECK(r.Substring);
    REQUIRE(ranges[0].size() == 1);
    CHECK(title.substr(ranges[0][0].Begin, ranges[0][0].End - ranges[0][0].Begin) == "Toggle");
    CHECK(ranges[1].empty());
    REQUIRE(ranges[2].size() == 1);
    CHECK(summary.substr(ranges[2][0].Begin, ranges[2][0].End - ranges[2][0].Begin) == "window");

    // An exact occurrence in a light field beats letters-in-order in a heavy one: the title holds h..i..d..e,
    // and its weight x score would win if the weight came first.
    const std::string spread = "h i d e";
    fields[0].Text = spread.data();
    fields[0].Len = spread.size();
    REQUIRE(Match(MakeQuery("hide"), spread, Mode::Fuzzy).Matched()); // the heavy field is a real candidate
    const Result s = MatchFields(MakeQuery("hide"), fields, 3, Mode::Fuzzy, scratch, ranges);
    CHECK(s.Substring);
    CHECK(ranges[0].empty());
    CHECK(ranges[2].size() == 1);
    fields[0].Text = title.data();
    fields[0].Len = title.size();

    CHECK_FALSE(MatchFields(MakeQuery("toggle absent"), fields, 3, Mode::Fuzzy, scratch, ranges).Matched());
    CHECK(ranges[0].empty());
    CHECK_FALSE(MatchFields(MakeQuery("x"), nullptr, 0, Mode::Fuzzy, scratch, nullptr).Matched());
}

TEST_CASE("TextMatch — caps: long candidates and hostile input stay bounded") {
    using textmatch_cases::Fuzzy;
    std::string longText(20000, 'a');
    longText += "zz";
    // The candidate is read only kMaxTextBytes far: "zz" past it is not found.
    CHECK_FALSE(Fuzzy("zz", longText).Matched());
    // Letters in order are looked for in the first kMaxFuzzyCodepoints only.
    std::string far(600, '.');
    far += "p r j";
    CHECK_FALSE(Fuzzy("prj", far).Matched());
    CHECK(Fuzzy("p", far).Matched()); // an occurrence as written is found anywhere in the read bytes
    // Invalid bytes match only themselves and never crash.
    CHECK(Fuzzy("\xFF", "a\xFF"
                        "b")
              .Matched());
    CHECK_FALSE(Fuzzy("\xFF", "a\xFE"
                              "b")
                    .Matched());
    CHECK(Fuzzy("ab", "\xC3"
                      "a\xE2\x82"
                      "b")
              .Matched());
}

TEST_CASE("TextMatch — character boundaries: no match starts or ends inside a valid character") {
    using smatchet::textmatch::CapAtCharBoundary;
    using smatchet::textmatch::CharBoundaryAtOrAfter;
    using smatchet::textmatch::ContainsAllWordsFolded;
    using smatchet::textmatch::FindFolded;
    const std::string a = "\xC4\x81"; // ā
    CHECK(CharBoundaryAtOrAfter(a.data(), a.size(), 1) == 2);
    CHECK(CharBoundaryAtOrAfter(a.data(), a.size(), 0) == 0);
    CHECK(FindFolded(a, "\x81", 1) == std::string::npos);   // a lone continuation byte is not a character here
    CHECK(FindFolded(std::string("\x81"), "\x81", 0) == 0); // ...but a stray one matches itself
    CHECK_FALSE(ContainsAllWordsFolded(a, MakeQuery("\x81")));
    CHECK(ContainsAllWordsFolded(std::string("x\x81"), MakeQuery("\x81")));
    // The read cap backs off to the lead byte of a character it would split.
    const std::string text = "ab\xC3\xA9"; // ab + é
    CHECK(CapAtCharBoundary(text.data(), text.size(), 3) == 2);
    CHECK(CapAtCharBoundary(text.data(), text.size(), 2) == 2);
    CHECK(CapAtCharBoundary(text.data(), text.size(), 10) == text.size());
    const std::string bad = "ab\x80\x80";
    CHECK(CapAtCharBoundary(bad.data(), bad.size(), 3) == 3); // malformed bytes are not moved over
    std::string longText(smatchet::textmatch::kMaxTextBytes - 1, 'a');
    longText += "\xC3\xA9"; // é straddles the cap
    CHECK_FALSE(Match(MakeQuery("\xC3"), longText, Mode::Fuzzy).Matched());
}

TEST_CASE("TextMatch — ContainsAllWordsFolded is the exact check for pre-folded haystacks") {
    using smatchet::textmatch::ContainsAllWordsFolded;
    using smatchet::textmatch::FoldCopy;
    const std::string hay = FoldCopy("{\"action\":\"Field_Edit\",\"key\":\"PROJ-7\"}");
    CHECK(ContainsAllWordsFolded(hay, MakeQuery("proj-7 field_edit")));
    CHECK(ContainsAllWordsFolded(hay, MakeQuery("")));
    CHECK_FALSE(ContainsAllWordsFolded(hay, MakeQuery("proj-8")));
    CHECK_FALSE(ContainsAllWordsFolded(hay, MakeQuery("fldedit"))); // no letters-in-order here
}

} // namespace textmatch_cases
