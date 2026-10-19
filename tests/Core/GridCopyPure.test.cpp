// Bucket-A doctest for the pure helpers behind the Key cell's "Copy N selected rows as CSV"
// item (GridCopyPure.h) and the shared RFC 4180 field escape (StringUtil.h CsvEscapeField).
// Header-only, no ImGui.

#include "StringUtil.h"
#include "Ui/GridCopyPure.h"

#include <doctest/doctest.h>

#include <initializer_list>
#include <set>
#include <string>
#include <vector>

TEST_CASE("ResolveGridRowsToTicketIndices maps sort-order rows through sortedIdx, ascending") {
    const std::vector<std::size_t> sorted = {3, 0, 2, 1};
    const std::vector<std::size_t> got = ResolveGridRowsToTicketIndices({0, 2, 3}, sorted, 4);
    CHECK(got == std::vector<std::size_t>{3, 2, 1});
}

TEST_CASE("ResolveGridRowsToTicketIndices uses the identity mapping when sortedIdx is empty") {
    const std::vector<std::size_t> got = ResolveGridRowsToTicketIndices({1, 3}, {}, 5);
    CHECK(got == std::vector<std::size_t>{1, 3});
}

TEST_CASE("ResolveGridRowsToTicketIndices skips out-of-range rows and indices") {
    SUBCASE("negative rows") {
        CHECK(ResolveGridRowsToTicketIndices({-2, -1, 0}, {}, 3) == std::vector<std::size_t>{0});
    }
    SUBCASE("rows past sortedIdx") {
        const std::vector<std::size_t> sorted = {1, 0};
        CHECK(ResolveGridRowsToTicketIndices({0, 2, 7}, sorted, 2) == std::vector<std::size_t>{1});
    }
    SUBCASE("mapped index >= ticketCount") {
        const std::vector<std::size_t> sorted = {0, 9};
        CHECK(ResolveGridRowsToTicketIndices({0, 1}, sorted, 3) == std::vector<std::size_t>{0});
    }
    SUBCASE("identity mapping row >= ticketCount") {
        CHECK(ResolveGridRowsToTicketIndices({2, 3}, {}, 3) == std::vector<std::size_t>{2});
    }
    SUBCASE("no rows") { CHECK(ResolveGridRowsToTicketIndices({}, {}, 3).empty()); }
}

TEST_CASE("BuildCsvLine joins escaped cells with commas and a trailing newline") {
    SUBCASE("plain cells") { CHECK(BuildCsvLine({"a", "b", "c"}) == "a,b,c\n"); }
    SUBCASE("single cell") { CHECK(BuildCsvLine({"only"}) == "only\n"); }
    SUBCASE("empty cells stay empty") { CHECK(BuildCsvLine({"a", "", "b"}) == "a,,b\n"); }
    SUBCASE("comma is quoted") { CHECK(BuildCsvLine({"a,b", "c"}) == "\"a,b\",c\n"); }
    SUBCASE("quote is doubled") { CHECK(BuildCsvLine({"say \"hi\""}) == "\"say \"\"hi\"\"\"\n"); }
    SUBCASE("embedded LF and CR are quoted, not flattened") {
        CHECK(BuildCsvLine({"l1\nl2", "x\ry"}) == "\"l1\nl2\",\"x\ry\"\n");
    }
}

TEST_CASE("CsvEscapeField - quotes only when needed, doubles inner quotes") {
    CHECK(CsvEscapeField("plain") == "plain");
    CHECK(CsvEscapeField("a,b") == "\"a,b\"");
    CHECK(CsvEscapeField("say \"hi\"") == "\"say \"\"hi\"\"\"");
    CHECK(CsvEscapeField("line1\nline2") == "\"line1\nline2\"");
    CHECK(CsvEscapeField("cr\rhere") == "\"cr\rhere\"");
    CHECK(CsvEscapeField("") == "");
}

// ---- Spreadsheet formula guard (#2337) ----
// Tracker text that starts like a formula must paste / open as text in Excel, LibreOffice and Sheets.

TEST_CASE("GuardSpreadsheetFormula prefixes a quote to every formula lead character") {
    CHECK(GuardSpreadsheetFormula("=HYPERLINK(\"https://evil\",\"x\")") == "'=HYPERLINK(\"https://evil\",\"x\")");
    CHECK(GuardSpreadsheetFormula("+cmd|' /C calc'!A0") == "'+cmd|' /C calc'!A0");
    CHECK(GuardSpreadsheetFormula("-2+3") == "'-2+3");
    CHECK(GuardSpreadsheetFormula("@SUM(A1)") == "'@SUM(A1)");
    CHECK(GuardSpreadsheetFormula("\t=1") == "'\t=1");
    CHECK(GuardSpreadsheetFormula("\r=1") == "'\r=1");
    // Leading spaces are looked past: an importer that trims them would still see the formula.
    CHECK(GuardSpreadsheetFormula("   =1+1") == "'   =1+1");
    // Not numbers, so still guarded: a lone sign, a percentage, a phone number.
    CHECK(GuardSpreadsheetFormula("-") == "'-");
    CHECK(GuardSpreadsheetFormula("-5%") == "'-5%");
    CHECK(GuardSpreadsheetFormula("+44 20 7946 0958") == "'+44 20 7946 0958");
}

TEST_CASE("GuardSpreadsheetFormula leaves plain numbers and ordinary text unchanged") {
    for (const char* number : {"-5", "+5", "-3.25", "+0.5", "-.5", "-1e3", "+2.5E-4", "  -7"}) {
        CAPTURE(number);
        CHECK(GuardSpreadsheetFormula(number) == number);
    }
    CHECK(GuardSpreadsheetFormula("Fix the crash") == "Fix the crash");
    CHECK(GuardSpreadsheetFormula("a=b") == "a=b");
    CHECK(GuardSpreadsheetFormula("") == "");
    CHECK(GuardSpreadsheetFormula("   ") == "   ");
    CHECK_FALSE(IsPlainSpreadsheetNumber("-1e"));
    CHECK_FALSE(IsPlainSpreadsheetNumber("1.2.3"));
    CHECK_FALSE(IsPlainSpreadsheetNumber("-"));
}

TEST_CASE("BuildCsvLine guards formula cells before quoting them") {
    CHECK(BuildCsvLine({"=HYPERLINK(\"x\")", "-5", "@a"}) == "\"'=HYPERLINK(\"\"x\"\")\",-5,'@a\n");
    // The header row goes through the same join; ordinary labels are untouched.
    CHECK(BuildCsvLine({"Key", "Summary"}) == "Key,Summary\n");
}

TEST_CASE("TsvCopyCell flattens tabs and line breaks, then guards the cell") {
    CHECK(TsvCopyCell("=1+1\tx") == "'=1+1 x");
    CHECK(TsvCopyCell("\t=cmd") == "' =cmd"); // the tab becomes a space; the guard looks past it
    CHECK(TsvCopyCell("-12") == "-12");
    CHECK(TsvCopyCell("line1\nline2") == "line1 line2");
}

TEST_CASE("GuardSpreadsheetFormula also covers LF and the full-width formula leads") {
    CHECK(GuardSpreadsheetFormula("\n=1") == "'\n=1");
    CHECK(GuardSpreadsheetFormula("\xEF\xBC\x9D"
                                  "1+1") == "'\xEF\xBC\x9D"
                                            "1+1"); // U+FF1D FULLWIDTH EQUALS
    CHECK(GuardSpreadsheetFormula("\xEF\xBC\x8B"
                                  "1") == "'\xEF\xBC\x8B"
                                          "1"); // U+FF0B FULLWIDTH PLUS
    CHECK(GuardSpreadsheetFormula("\xEF\xBC\x8D"
                                  "1") == "'\xEF\xBC\x8D"
                                          "1"); // U+FF0D FULLWIDTH HYPHEN-MINUS
    CHECK(GuardSpreadsheetFormula("\xEF\xBC\xA0"
                                  "a") == "'\xEF\xBC\xA0"
                                          "a"); // U+FF20 FULLWIDTH AT
    // Other full-width characters, and truncated sequences, are ordinary text.
    CHECK(GuardSpreadsheetFormula("\xEF\xBC\xA1") == "\xEF\xBC\xA1"); // U+FF21 FULLWIDTH A
    CHECK(GuardSpreadsheetFormula("\xEF\xBC") == "\xEF\xBC");
    CHECK(GuardSpreadsheetFormula("\xEF") == "\xEF");
}

TEST_CASE("UnguardSpreadsheetFormula undoes exactly the quote the guard adds") {
    for (const char* original : {"=1+1", "  @x", "-2+3",
                                 "\xEF\xBC\x9D"
                                 "1",
                                 "-5", "plain", "'already", "", "'"}) {
        CAPTURE(original);
        CHECK(UnguardSpreadsheetFormula(GuardSpreadsheetFormula(original)) == original);
    }
    CHECK(UnguardSpreadsheetFormula("'5") == "'5");     // the guard never quotes a number
    CHECK(UnguardSpreadsheetFormula("'tis") == "'tis"); // nor ordinary text
    CHECK(UnguardSpreadsheetFormula("'=x") == "=x");
}
