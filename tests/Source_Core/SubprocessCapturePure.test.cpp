#include "SubprocessCapturePure.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

namespace {

// Helper for inspecting the null-separated env block. Returns the
// sequence of KEY=VALUE entries split on '\0' (excluding the final
// double-null terminator).
std::vector<std::string> SplitEnvBlock(const std::string& block) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < block.size(); ++i) {
        if (block[i] == '\0') {
            if (i > start) {
                out.push_back(block.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return out;
}

} // namespace

TEST_SUITE("SubprocessCapturePure::QuoteArgvWindows") {

    TEST_CASE("empty argv element produces literal empty quoted string") {
        CHECK(SubprocessCapturePure::QuoteArgvWindows("") == "\"\"");
    }

    TEST_CASE("bare token with no whitespace or quotes is unchanged") {
        CHECK(SubprocessCapturePure::QuoteArgvWindows("p4") == "p4");
        CHECK(SubprocessCapturePure::QuoteArgvWindows("--flag=value") == "--flag=value");
        CHECK(SubprocessCapturePure::QuoteArgvWindows("C:\\path\\with\\backslashes") == "C:\\path\\with\\backslashes");
    }

    TEST_CASE("embedded space wraps in double quotes") {
        CHECK(SubprocessCapturePure::QuoteArgvWindows("hello world") == "\"hello world\"");
    }

    TEST_CASE("embedded tab triggers quoting") { CHECK(SubprocessCapturePure::QuoteArgvWindows("a\tb") == "\"a\tb\""); }

    TEST_CASE("embedded quote is escaped with backslash") {
        // Input:  a"b   →   Output: "a\"b"
        CHECK(SubprocessCapturePure::QuoteArgvWindows("a\"b") == "\"a\\\"b\"");
    }

    TEST_CASE("backslashes before an embedded quote are doubled") {
        // Input:  a\"b  (one backslash, one quote, one b)
        // Output: "a\\\"b" — the backslash is doubled because it
        // precedes a literal quote that itself must be escaped.
        CHECK(SubprocessCapturePure::QuoteArgvWindows("a\\\"b") == "\"a\\\\\\\"b\"");
    }

    TEST_CASE("trailing backslashes before closing wrap are doubled") {
        // Input:  C:\path\  (note the trailing backslash) inside a
        // quoted span — must double to avoid pairing with the close.
        // The token contains a space to force quoting.
        CHECK(SubprocessCapturePure::QuoteArgvWindows("C:\\Program Files\\") == "\"C:\\Program Files\\\\\"");
    }
}

TEST_SUITE("SubprocessCapturePure::QuoteArgvPosix") {

    TEST_CASE("empty argv element produces ''") { CHECK(SubprocessCapturePure::QuoteArgvPosix("") == "''"); }

    TEST_CASE("bare safe token is unchanged") {
        CHECK(SubprocessCapturePure::QuoteArgvPosix("p4") == "p4");
        CHECK(SubprocessCapturePure::QuoteArgvPosix("--flag=value") == "--flag=value");
        CHECK(SubprocessCapturePure::QuoteArgvPosix("/usr/bin/cat") == "/usr/bin/cat");
    }

    TEST_CASE("space triggers single-quote wrapping") {
        CHECK(SubprocessCapturePure::QuoteArgvPosix("hello world") == "'hello world'");
    }

    TEST_CASE("single-quote escape uses '\\'' trick") {
        // Input:  it's
        // Output: 'it'\''s'
        CHECK(SubprocessCapturePure::QuoteArgvPosix("it's") == "'it'\\''s'");
    }

    TEST_CASE("special chars trigger wrapping") {
        CHECK(SubprocessCapturePure::QuoteArgvPosix("$HOME") == "'$HOME'");
        CHECK(SubprocessCapturePure::QuoteArgvPosix("a&b") == "'a&b'");
        CHECK(SubprocessCapturePure::QuoteArgvPosix("a;b") == "'a;b'");
    }
}

TEST_SUITE("SubprocessCapturePure::BuildEnvBlockWindows") {

    TEST_CASE("empty env produces double-null terminator only") {
        const std::string block = SubprocessCapturePure::BuildEnvBlockWindows({});
        CHECK(block.size() == 2);
        CHECK(block[0] == '\0');
        CHECK(block[1] == '\0');
    }

    TEST_CASE("single var produces KEY=VALUE\\0\\0") {
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back(std::make_pair("FOO", "bar"));
        const std::string block = SubprocessCapturePure::BuildEnvBlockWindows(env);
        const auto entries = SplitEnvBlock(block);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0] == "FOO=bar");
        // Final byte must be the second null of the double-null
        // terminator.
        REQUIRE(block.size() >= 2);
        CHECK(block[block.size() - 1] == '\0');
        CHECK(block[block.size() - 2] == '\0');
    }

    TEST_CASE("multiple vars preserve insertion order") {
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back(std::make_pair("PATH", "/usr/bin"));
        env.push_back(std::make_pair("LANG", "en_US.UTF-8"));
        env.push_back(std::make_pair("EMPTY", ""));
        const std::string block = SubprocessCapturePure::BuildEnvBlockWindows(env);
        const auto entries = SplitEnvBlock(block);
        REQUIRE(entries.size() == 3);
        CHECK(entries[0] == "PATH=/usr/bin");
        CHECK(entries[1] == "LANG=en_US.UTF-8");
        CHECK(entries[2] == "EMPTY=");
    }
}

TEST_SUITE("SubprocessCapturePure::RemainingTimeoutMs") {

    TEST_CASE("zero total timeout returns zero (no timeout sentinel)") {
        const auto now = std::chrono::steady_clock::now();
        CHECK(SubprocessCapturePure::RemainingTimeoutMs(now, 0) == 0);
    }

    TEST_CASE("negative total timeout is passed through") {
        const auto now = std::chrono::steady_clock::now();
        CHECK(SubprocessCapturePure::RemainingTimeoutMs(now, -1) == -1);
    }

    TEST_CASE("zero elapsed returns full budget") {
        const auto now = std::chrono::steady_clock::now();
        // Use a budget large enough that no real clock drift between
        // the two now() calls in the function vs the test eats it.
        CHECK(SubprocessCapturePure::RemainingTimeoutMs(now, 1000000) > 999000);
    }

    TEST_CASE("expired timeout returns zero") {
        const auto pastStart = std::chrono::steady_clock::now() - std::chrono::seconds(10);
        CHECK(SubprocessCapturePure::RemainingTimeoutMs(pastStart, 1000) == 0);
    }

    TEST_CASE("partial elapse returns positive remaining") {
        const auto pastStart = std::chrono::steady_clock::now() - std::chrono::milliseconds(100);
        const int64_t remaining = SubprocessCapturePure::RemainingTimeoutMs(pastStart, 5000);
        CHECK(remaining > 4000);
        CHECK(remaining < 5000);
    }
}
