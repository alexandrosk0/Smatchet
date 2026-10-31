// ParseCallstackText's format scans (#2311) checked against the std::regex parser they replaced,
// plus the long-line cases that stalled the UI thread or lost a frame under the regexes.
//
// The oracle below is the previous TryParsePathLinePair verbatim (with its regex_error guard). The
// generated lines are short, so neither its super-linear scans nor a matcher complexity budget come
// into play; a sample the oracle's matcher refuses anyway is skipped.

#include "CallstackParser.h"

#include <doctest/doctest.h>

#include <cctype>
#include <chrono>
#include <climits>
#include <cstdint>
#include <initializer_list>
#include <random>
#include <regex>
#include <string>
#include <vector>

namespace {

namespace oracle {

bool ParseLineNumberInRange(const std::string& digits, int& outLine) {
    try {
        const long long v = std::stoll(digits);
        if (v < 0 || v > static_cast<long long>(INT_MAX)) {
            return false;
        }
        outLine = static_cast<int>(v);
        return true;
    } catch (...) { // catch-all-ok: oracle copy of the parser's stoll guard
        return false;
    }
}

std::string Trim(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

void TryExtractUnrealOrModuleFunctionPrefix(const std::string& line, const std::string& path,
                                            std::string& outFunction) {
    if (path.empty() || !outFunction.empty()) {
        return;
    }
    const size_t pos = line.find(path);
    if (pos == std::string::npos || pos == 0) {
        return;
    }
    std::string prefix = line.substr(0, pos);
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
        prefix.pop_back();
    }
    while (!prefix.empty() && prefix.back() == '[') {
        prefix.pop_back();
    }
    while (!prefix.empty() && std::isspace(static_cast<unsigned char>(prefix.back()))) {
        prefix.pop_back();
    }
    prefix = Trim(prefix);
    if (prefix.empty()) {
        return;
    }
    const size_t bang = prefix.find('!');
    outFunction = (bang != std::string::npos) ? Trim(prefix.substr(bang + 1)) : prefix;
}

bool HasCharThenDigit(const std::string& line, char lead) {
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        if (line[i] == lead && line[i + 1] >= '0' && line[i + 1] <= '9') {
            return true;
        }
    }
    return false;
}

struct Refused {};

bool Search(const std::string& line, std::smatch& m, const std::regex& re) {
    try {
        return std::regex_search(line, m, re);
    } catch (const std::regex_error&) {
        throw Refused();
    }
}

bool TryParsePathLinePair(const std::string& line, std::string& outPath, int& outLine, std::string& outFunction) {
    outPath.clear();
    outLine = 0;
    outFunction.clear();
    if (HasCharThenDigit(line, '(')) {
        static const std::regex re(
            R"(([A-Za-z]:[^()\r\n]*|\S(?:[^\s:()]*[/\\])?[^\s:()]*\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m))\(([0-9]+)\))",
            std::regex::icase);
        std::smatch m;
        if (Search(line, m, re)) {
            outPath = Trim(m[1].str());
            if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                return false;
            }
            const size_t pos = line.find(m[0].str());
            if (pos != std::string::npos && pos > 0) {
                std::string fn = Trim(line.substr(0, pos));
                const size_t bang = fn.find('!');
                if (bang != std::string::npos) {
                    fn = Trim(fn.substr(bang + 1));
                }
                outFunction = fn;
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }
    if (!HasCharThenDigit(line, ':')) {
        return false;
    }
    {
        static const std::regex re(
            R"(([A-Za-z]:[^:\r\n]+|(?:/|\.\.?/|\S[/\\])[^\s:]+\.(?:cpp|c|cc|cxx|h|hpp|inl|cs|java|mm|m)):([0-9]+)(?::[0-9]+)?)",
            std::regex::icase);
        std::smatch m;
        if (Search(line, m, re)) {
            outPath = Trim(m[1].str());
            if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                return false;
            }
            static const std::regex atFn(R"(at\s+([^\s]+)\s+)", std::regex::icase);
            std::smatch mf;
            if (Search(line, mf, atFn)) {
                outFunction = Trim(mf[1].str());
            }
            TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
            return true;
        }
    }
    {
        static const std::regex re(R"(\[?\s*([A-Za-z]:[^\]\r\n]+|(?:/|\S[/\\])[^\]\r\n]+):([0-9]+)\s*\]?)",
                                   std::regex::icase);
        std::smatch m;
        if (Search(line, m, re)) {
            std::string p = Trim(m[1].str());
            if (!p.empty() && (p.find('.') != std::string::npos || p.find('/') != std::string::npos ||
                               p.find('\\') != std::string::npos)) {
                outPath = p;
                if (!ParseLineNumberInRange(m[2].str(), outLine)) {
                    return false;
                }
                TryExtractUnrealOrModuleFunctionPrefix(line, outPath, outFunction);
                return true;
            }
        }
    }
    return false;
}

// The old ParseCallstackText loop for one already-trimmed, non-empty line.
std::vector<ParsedCallstackFrame> ParseLine(const std::string& line) {
    std::vector<ParsedCallstackFrame> out;
    ParsedCallstackFrame f;
    f.RawLine = line;
    std::string path;
    int lineNum = 0;
    std::string fn;
    if (TryParsePathLinePair(line, path, lineNum, fn)) {
        f.FilePath = path;
        f.LineNumber = lineNum;
        f.Function = fn.empty() ? std::string("<unknown>") : fn;
        out.push_back(f);
    }
    return out;
}

} // namespace oracle

// Pieces that steer generated lines into every branch of the three formats: drive letters, the
// three path prefixes, extensions in mixed case and their near misses, the line-number anchors, the
// characters each run stops at, and the "at" function prefix.
const char* const kFragments[] = {
    "C:",        "d:",       "Z:\\",
    "x",         "a",        "Q",
    "/",         "\\",       "./",
    "../",       ".",        "..",
    "/home/dev", "\\src\\",  "foo",
    "bar_baz",   "My Class", "-",
    "_",         ".cpp",     ".CPP",
    ".c",        ".cc",      ".Cxx",
    ".h",        ".HPP",     ".inl",
    ".cs",       ".java",    ".mm",
    ".m",        ".md",      ".cp",
    ".hh",       "(",        ")",
    "(1)",       "(42)",     "(2147483648)",
    "(x)",       "()",       ":",
    ":7",        ":42:3",    "::1",
    ":0",        "[",        "]",
    "[ ",        " ]",       "!",
    "Mod!",      "Func()",   "at ",
    "AT  ",      "aT",       " ",
    "  ",        "\t",       "\r",
    "0",         "9",        "12",
    "@",         "<",        ">",
    ",",         "'",
};

std::string GenerateLine(std::mt19937& rng) {
    const std::size_t fragmentCount = sizeof(kFragments) / sizeof(kFragments[0]);
    const std::uint32_t pieces = 1 + rng() % 14;
    std::string line;
    for (std::uint32_t i = 0; i < pieces; ++i) {
        line += kFragments[rng() % fragmentCount];
    }
    return line;
}

std::string TrimForParser(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.erase(0, 1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.pop_back();
    }
    return s;
}

bool SameFrames(const std::vector<ParsedCallstackFrame>& a, const std::vector<ParsedCallstackFrame>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].RawLine != b[i].RawLine || a[i].Function != b[i].Function || a[i].FilePath != b[i].FilePath ||
            a[i].LineNumber != b[i].LineNumber) {
            return false;
        }
    }
    return true;
}

std::string Describe(const std::vector<ParsedCallstackFrame>& frames) {
    if (frames.empty()) {
        return "(no frame)";
    }
    const ParsedCallstackFrame& f = frames.front();
    return "path=[" + f.FilePath + "] line=" + std::to_string(f.LineNumber) + " fn=[" + f.Function + "]";
}

} // namespace

TEST_CASE("ParseCallstackText scans agree with the regex parser they replaced (differential)" *
          doctest::test_suite("[high-risk]")) {
    std::mt19937 rng(0x2311u);
    int compared = 0;
    int skipped = 0;
    for (int i = 0; i < 30000; ++i) {
        const std::string line = TrimForParser(GenerateLine(rng));
        if (line.empty()) {
            continue;
        }
        std::vector<ParsedCallstackFrame> expected;
        try {
            expected = oracle::ParseLine(line);
        } catch (const oracle::Refused&) {
            ++skipped;
            continue;
        }
        ++compared;
        const std::vector<ParsedCallstackFrame> actual = ParseCallstackText(line);
        if (!SameFrames(actual, expected)) {
            const std::string want = Describe(expected);
            const std::string got = Describe(actual);
            CAPTURE(line);
            CAPTURE(want);
            CAPTURE(got);
            CHECK(SameFrames(actual, expected));
            break;
        }
    }
    CHECK(compared > 25000);
    CHECK(skipped < 500);
}

TEST_CASE("ParseCallstackText keeps the leftmost-match choices the regexes made") {
    SUBCASE("MSVC relative form: the one character before the path run is part of the path") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("(foo.cpp(12)");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "(foo.cpp");
        CHECK(frames.front().LineNumber == 12);
    }
    SUBCASE("path:line drive form wins over a later rooted path on the same line") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("C:x y:1 /a/b.cpp:2");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "C:x y");
        CHECK(frames.front().LineNumber == 1);
    }
    SUBCASE("bracket form ends the path at the last :digit before ']'") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("[/x 1:2/y:33]");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "/x 1:2/y");
        CHECK(frames.front().LineNumber == 33);
    }
    SUBCASE("bracket form can give the '[' back to start the path") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("[\\foo:12]");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "[\\foo");
        CHECK(frames.front().LineNumber == 12);
    }
    SUBCASE("a first bracket match that is not a path ends the search") {
        // "a::x" is the bracket group (drive form, run up to the last :digit before ']'); it holds no
        // '.', '/' or '\', so the line yields nothing even though a path follows the '\r'.
        CHECK(ParseCallstackText("a::x:1]\r/q/r.txt:5").empty());
    }
    SUBCASE("\"at\" prefix is matched case-insensitively and needs whitespace after the token") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("#3 AT  Foo::Bar /src/x.cc:9");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().Function == "Foo::Bar");
    }
}

TEST_CASE("ParseCallstackText finds frames after long tokens that the regexes lost or stalled on (#2311)" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("MSVC frame after a 384- and a 4096-character token") {
        for (const size_t length : {static_cast<size_t>(384), static_cast<size_t>(4096)}) {
            const std::vector<ParsedCallstackFrame> frames =
                ParseCallstackText(std::string(length, 'a') + " C:\\real.cpp(7)");
            REQUIRE(frames.size() == 1);
            CHECK(frames.front().FilePath == "C:\\real.cpp");
            CHECK(frames.front().LineNumber == 7);
        }
    }
    SUBCASE("MSVC frame after a 512-character templated symbol") {
        std::string symbol = "ns::Outer<";
        while (symbol.size() < 512) {
            symbol += "std::vector<int>,";
        }
        symbol += "int>::Run()";
        const std::vector<ParsedCallstackFrame> frames =
            ParseCallstackText(symbol + " [C:\\Dev\\Game\\Source\\MyClass.cpp(42)]");
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "C:\\Dev\\Game\\Source\\MyClass.cpp");
        CHECK(frames.front().LineNumber == 42);
    }
    SUBCASE("slash-heavy lines that held the pair are fast and exact") {
        // 500 x "a/" then "(1)" took ~2.4 s on MSVC 14.50; the full 16 KiB cap is used here.
        std::string slashes;
        while (slashes.size() < 16000) {
            slashes += "a/";
        }
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(ParseCallstackText(slashes + "(1)").empty());
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText(slashes + "x.cpp(5)");
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == slashes + "x.cpp");
        CHECK(frames.front().LineNumber == 5);
        // Linear work on 32 KiB is well under a millisecond; the ceiling only catches a return of
        // super-linear scanning (seconds), with room for sanitizer and coverage instrumentation.
        CHECK(elapsedMs < 2000);
    }
    SUBCASE("full-cap whitespace runs are skipped once, not once per start") {
        // "a" + 16K blanks + ":1" has no "(digit" pair and no path start, so only the bracket scan
        // sees it; before the run-end was carried across starts each start re-skipped the run.
        std::string paste;
        for (int i = 0; i < 128; ++i) {
            paste += "a" + std::string(16000, (i % 2) ? '\t' : ' ') + ":1\n";
        }
        paste += std::string(16000, ' ') + "[x/y.h:4]\n";
        const auto t0 = std::chrono::steady_clock::now();
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText(paste);
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().FilePath == "x/y.h");
        CHECK(frames.front().LineNumber == 4);
        // Quadratic took ~60 ms per line at -O2 (~7.5 s here); linear is ~0.5 ms per line.
        CHECK(elapsedMs < 2000);
    }
    SUBCASE("a full-cap line of path characters with every anchor kind") {
        std::string line(16000, 'b');
        line += ".cpp:3 [x/y.h:4] z.cpp(5)";
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText(line);
        REQUIRE(frames.size() == 1);
        CHECK(frames.front().LineNumber == 5);
    }
}
