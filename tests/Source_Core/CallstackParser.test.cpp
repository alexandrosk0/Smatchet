#include <doctest/doctest.h>

#include "CallstackParser.h"

#include <string>
#include <vector>

namespace {

PathRemapRule MakeRule(const std::string& from, const std::string& to) {
    PathRemapRule r;
    r.FromPrefix = from;
    r.ToPrefix = to;
    return r;
}

ParsedCallstackFrame MakeFrame(const std::string& raw, const std::string& fn, const std::string& path) {
    ParsedCallstackFrame f;
    f.RawLine = raw;
    f.Function = fn;
    f.FilePath = path;
    return f;
}

ParsedCallstackFrame ParseSingleLine(const std::string& line) {
    const std::vector<ParsedCallstackFrame> frames = ParseCallstackText(line);
    if (frames.empty()) {
        return ParsedCallstackFrame();
    }
    return frames.front();
}

} // namespace

TEST_CASE("CallstackParser::ParseCallstackText handles MSVC path(line) form" * doctest::test_suite("[high-risk]")) {
    SUBCASE("plain MSVC path with absolute drive") {
        const ParsedCallstackFrame f = ParseSingleLine(R"(C:\Dev\Smatchet\Source_Core\src\AppController.cpp(123))");
        CHECK(f.FilePath == R"(C:\Dev\Smatchet\Source_Core\src\AppController.cpp)");
        CHECK(f.LineNumber == 123);
    }

    SUBCASE("MSVC line 1 (off-by-one mutation sentinel)") {
        const ParsedCallstackFrame f = ParseSingleLine(R"(C:\foo.cpp(1))");
        CHECK(f.LineNumber == 1);
        CHECK(f.FilePath == R"(C:\foo.cpp)");
    }

    SUBCASE("MSVC large line number") {
        const ParsedCallstackFrame f = ParseSingleLine(R"(C:\big.cpp(987654))");
        CHECK(f.LineNumber == 987654);
    }

    SUBCASE("Unreal Module!Class::Method() pattern carries function") {
        const std::string raw = R"(MyModule!UMyClass::Tick() [C:\UE5\MyModule\Private\MyClass.cpp(42)])";
        const ParsedCallstackFrame f = ParseSingleLine(raw);
        CHECK(f.LineNumber == 42);
        CHECK(f.FilePath == R"(C:\UE5\MyModule\Private\MyClass.cpp)");
        // Function prefix includes the trailing bracket lead-in because the MSVC branch fast-path
        // captures everything before the path(line) match, then strips only the module-bang split.
        CHECK(f.Function == "UMyClass::Tick() [");
    }

    SUBCASE("bare-relative path(line) is recognized when extension is known") {
        const ParsedCallstackFrame f = ParseSingleLine("Source/Foo.cpp(7)");
        CHECK(f.FilePath == "Source/Foo.cpp");
        CHECK(f.LineNumber == 7);
    }
}

TEST_CASE("CallstackParser::ParseCallstackText handles Clang/GDB path:line[:col] form" *
          doctest::test_suite("[high-risk]")) {
    SUBCASE("clang colon line only") {
        const ParsedCallstackFrame f = ParseSingleLine("/home/dev/project/src/foo.cpp:42");
        CHECK(f.FilePath == "/home/dev/project/src/foo.cpp");
        CHECK(f.LineNumber == 42);
    }

    SUBCASE("clang colon line and column") {
        const ParsedCallstackFrame f = ParseSingleLine("/usr/src/bar.cc:99:14");
        CHECK(f.FilePath == "/usr/src/bar.cc");
        CHECK(f.LineNumber == 99);
    }

    SUBCASE("GDB 'at FUNC PATH:LINE' line is recognized") {
        const ParsedCallstackFrame f = ParseSingleLine("#3 0x004 in main at /tmp/main.cpp:17");
        CHECK(f.FilePath == "/tmp/main.cpp");
        CHECK(f.LineNumber == 17);
        // Function discovery in this branch is best-effort — current behaviour preserves the
        // leading text up to the path match, not just the 'at FUNC' capture.
        CHECK_FALSE(f.Function.empty());
    }

    SUBCASE("windows drive path:line") {
        const ParsedCallstackFrame f = ParseSingleLine("C:/Dev/Foo/bar.hpp:10");
        CHECK(f.FilePath == "C:/Dev/Foo/bar.hpp");
        CHECK(f.LineNumber == 10);
    }
}

TEST_CASE("CallstackParser::ParseCallstackText skips lines without a path:line marker") {
    SUBCASE("plain prose ignored") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("this is not a stack frame at all\n");
        CHECK(frames.size() == 0);
    }

    SUBCASE("empty input yields no frames") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("");
        CHECK(frames.size() == 0);
    }

    SUBCASE("whitespace-only input yields no frames") {
        const std::vector<ParsedCallstackFrame> frames = ParseCallstackText("   \n\n\t\r\n");
        CHECK(frames.size() == 0);
    }

    SUBCASE("function name defaults to <unknown> when path-only line lacks prefix") {
        const ParsedCallstackFrame f = ParseSingleLine("/x/y/z.cpp:5");
        CHECK(f.Function == "<unknown>");
    }
}

TEST_CASE("CallstackParser::ParseCallstackText accumulates multi-line mixed-format text") {
    const std::string text = "Some preamble line\n"
                             "MyModule!Foo::Bar() [C:\\proj\\src\\foo.cpp(10)]\n"
                             "skipping this prose\n"
                             "/home/dev/baz.cpp:20:5\n"
                             "C:\\proj\\src\\qux.cpp(30)\n";
    const std::vector<ParsedCallstackFrame> frames = ParseCallstackText(text);
    REQUIRE(frames.size() == 3);

    CHECK(frames[0].LineNumber == 10);
    CHECK(frames[0].FilePath == R"(C:\proj\src\foo.cpp)");
    CHECK(frames[0].Function == "Foo::Bar() [");

    CHECK(frames[1].LineNumber == 20);
    CHECK(frames[1].FilePath == "/home/dev/baz.cpp");

    CHECK(frames[2].LineNumber == 30);
    CHECK(frames[2].FilePath == R"(C:\proj\src\qux.cpp)");
}

TEST_CASE("CallstackParser::ParseCallstackText extracts Unreal module prefix as function name") {
    SUBCASE("Module!Function form with bang strips the module prefix") {
        const ParsedCallstackFrame f = ParseSingleLine("MyMod!Foo::Bar() [C:\\foo.cpp(1)]");
        // Bracket lead-in is preserved by current MSVC fast-path; the Module! part is stripped.
        CHECK(f.Function == "Foo::Bar() [");
    }

    SUBCASE("no module bang keeps full prefix as function") {
        const ParsedCallstackFrame f = ParseSingleLine("Foo::Bar() [C:\\foo.cpp(1)]");
        CHECK(f.Function == "Foo::Bar() [");
    }

    SUBCASE("line that starts with path has no function prefix") {
        const ParsedCallstackFrame f = ParseSingleLine("C:\\foo.cpp(1)");
        CHECK(f.Function == "<unknown>");
    }
}

TEST_CASE("CallstackParser::ApplyPathRemaps prefers longest matching prefix" * doctest::test_suite("[high-risk]")) {
    SUBCASE("single rule replaces matching prefix") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/depot/foo/", "C:/local/foo/"));
        CHECK(ApplyPathRemaps("/depot/foo/src/bar.cpp", rules) == "C:/local/foo/src/bar.cpp");
    }

    SUBCASE("no matching rule leaves path unchanged") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/depot/x/", "C:/y/"));
        CHECK(ApplyPathRemaps("/other/path.cpp", rules) == "/other/path.cpp");
    }

    SUBCASE("empty remap list leaves path unchanged") {
        std::vector<PathRemapRule> empty;
        CHECK(ApplyPathRemaps("/a/b/c.cpp", empty) == "/a/b/c.cpp");
    }

    SUBCASE("longest-prefix wins over shorter overlapping prefix") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/depot/", "/SHORT/"));
        rules.push_back(MakeRule("/depot/sub/", "/LONG/"));
        CHECK(ApplyPathRemaps("/depot/sub/file.cpp", rules) == "/LONG/file.cpp");
    }

    SUBCASE("longest-prefix wins regardless of rule order (shorter listed last)") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/depot/sub/deeper/", "/DEEPEST/"));
        rules.push_back(MakeRule("/depot/sub/", "/MID/"));
        rules.push_back(MakeRule("/depot/", "/SHORT/"));
        CHECK(ApplyPathRemaps("/depot/sub/deeper/leaf.cpp", rules) == "/DEEPEST/leaf.cpp");
        CHECK(ApplyPathRemaps("/depot/sub/other.cpp", rules) == "/MID/other.cpp");
        CHECK(ApplyPathRemaps("/depot/top.cpp", rules) == "/SHORT/top.cpp");
    }

    SUBCASE("empty FromPrefix rule is ignored") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("", "/IGNORED/"));
        rules.push_back(MakeRule("/depot/", "/OK/"));
        CHECK(ApplyPathRemaps("/depot/x.cpp", rules) == "/OK/x.cpp");
    }

    SUBCASE("prefix equal to the entire path replaces fully") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/a/b.cpp", "/X/Y.cpp"));
        CHECK(ApplyPathRemaps("/a/b.cpp", rules) == "/X/Y.cpp");
    }

    SUBCASE("prefix longer than path does not match") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/aaa/bbb/", "/X/"));
        CHECK(ApplyPathRemaps("/aaa", rules) == "/aaa");
    }

    SUBCASE("later rule with equal-length prefix wins (ties broken by index)") {
        std::vector<PathRemapRule> rules;
        rules.push_back(MakeRule("/d/", "/FIRST/"));
        rules.push_back(MakeRule("/d/", "/LAST/"));
        CHECK(ApplyPathRemaps("/d/x.cpp", rules) == "/LAST/x.cpp");
    }
}

TEST_CASE("CallstackParser::FrameMatchesIgnoreKeywords matches case-insensitively across raw/function/path") {
    const ParsedCallstackFrame frame = MakeFrame("RawLine has Marker", "Function::Foo", "C:/Path/Bar.cpp");

    SUBCASE("empty keyword list does not match") {
        const std::vector<std::string> none;
        CHECK_FALSE(FrameMatchesIgnoreKeywords(frame, none));
    }

    SUBCASE("empty individual keyword is skipped") {
        std::vector<std::string> kws;
        kws.push_back("");
        CHECK_FALSE(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("matches keyword in raw line") {
        std::vector<std::string> kws;
        kws.push_back("marker");
        CHECK(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("matches keyword in function") {
        std::vector<std::string> kws;
        kws.push_back("function::foo");
        CHECK(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("matches keyword in path") {
        std::vector<std::string> kws;
        kws.push_back("path/bar");
        CHECK(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("case-insensitive match on uppercase keyword") {
        std::vector<std::string> kws;
        kws.push_back("MARKER");
        CHECK(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("no match across all fields returns false") {
        std::vector<std::string> kws;
        kws.push_back("absent");
        CHECK_FALSE(FrameMatchesIgnoreKeywords(frame, kws));
    }

    SUBCASE("multiple keywords - any match wins") {
        std::vector<std::string> kws;
        kws.push_back("absent");
        kws.push_back("MARKER");
        CHECK(FrameMatchesIgnoreKeywords(frame, kws));
    }
}
