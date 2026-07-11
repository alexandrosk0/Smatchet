#include "SubprocessCapturePure.h"

#include <doctest/doctest.h>

#include <chrono>
#include <string>

namespace {

// (CR Bundle A1) BuildEnvBlockWindows returns std::wstring (UTF-16) since
// `CreateProcessW` consumes a wide env block under CREATE_UNICODE_ENVIRONMENT.
// Split on wide null sentinels and narrow each entry back to UTF-8 for the
// assertions. The narrowing is safe for the ASCII test cases below; the
// Unicode round-trip case asserts on the wide form directly.
std::vector<std::string> SplitEnvBlock(const std::wstring& block) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i < block.size(); ++i) {
        if (block[i] == L'\0') {
            if (i > start) {
                std::string entry;
                entry.reserve(i - start);
                for (size_t j = start; j < i; ++j) {
                    // Narrow ASCII-safe — wide chars in [0, 127] map 1:1 to UTF-8.
                    // Non-ASCII content is asserted at the wide-string level
                    // (see the Ångström round-trip below) so this helper is
                    // only used for ASCII keys/values.
                    entry.push_back(static_cast<char>(block[j] & 0xFF));
                }
                out.push_back(std::move(entry));
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
        const std::wstring block = SubprocessCapturePure::BuildEnvBlockWindows({});
        CHECK(block.size() == 2);
        CHECK(block[0] == L'\0');
        CHECK(block[1] == L'\0');
    }

    TEST_CASE("single var produces KEY=VALUE\\0\\0") {
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back(std::make_pair("FOO", "bar"));
        const std::wstring block = SubprocessCapturePure::BuildEnvBlockWindows(env);
        const auto entries = SplitEnvBlock(block);
        REQUIRE(entries.size() == 1);
        CHECK(entries[0] == "FOO=bar");
        // Final wide-char must be the second null of the double-null
        // terminator.
        REQUIRE(block.size() >= 2);
        CHECK(block[block.size() - 1] == L'\0');
        CHECK(block[block.size() - 2] == L'\0');
    }

    TEST_CASE("multiple vars preserve insertion order") {
        std::vector<std::pair<std::string, std::string>> env;
        env.push_back(std::make_pair("PATH", "/usr/bin"));
        env.push_back(std::make_pair("LANG", "en_US.UTF-8"));
        env.push_back(std::make_pair("EMPTY", ""));
        const std::wstring block = SubprocessCapturePure::BuildEnvBlockWindows(env);
        const auto entries = SplitEnvBlock(block);
        REQUIRE(entries.size() == 3);
        CHECK(entries[0] == "PATH=/usr/bin");
        CHECK(entries[1] == "LANG=en_US.UTF-8");
        CHECK(entries[2] == "EMPTY=");
    }

    TEST_CASE("UTF-8 value round-trips to UTF-16 (CR Bundle A1)") {
        // The CodeRabbit critical was: BuildEnvBlockWindows returned 8-bit
        // bytes and CreateProcessW (without CREATE_UNICODE_ENVIRONMENT)
        // interpreted them as ANSI, corrupting non-ASCII bytes. The fix
        // returns UTF-16; this case asserts the conversion is byte-perfect
        // for a non-ASCII value containing the å (U+00E5) and ö (U+00F6)
        // code points that ANSI cp1252 would have round-tripped but cp437
        // / shift-jis hosts would have corrupted.
        std::vector<std::pair<std::string, std::string>> env;
        // "KEY=Ångström" — UTF-8: K E Y = 0xC3 0x85 n g s t r 0xC3 0xB6 m
        env.push_back(std::make_pair("KEY", "\xC3\x85ngstr\xC3\xB6m"));
        const std::wstring block = SubprocessCapturePure::BuildEnvBlockWindows(env);
        // Expected wide form: 'K','E','Y','=', U+00C5, 'n','g','s','t','r', U+00F6, 'm'
        REQUIRE(block.size() >= 14);
        CHECK(block[0] == L'K');
        CHECK(block[1] == L'E');
        CHECK(block[2] == L'Y');
        CHECK(block[3] == L'=');
        CHECK(block[4] == static_cast<wchar_t>(0x00C5));
        CHECK(block[5] == L'n');
        CHECK(block[6] == L'g');
        CHECK(block[7] == L's');
        CHECK(block[8] == L't');
        CHECK(block[9] == L'r');
        CHECK(block[10] == static_cast<wchar_t>(0x00F6));
        CHECK(block[11] == L'm');
        CHECK(block[12] == L'\0'); // entry terminator
        CHECK(block[13] == L'\0'); // block terminator
    }
}

TEST_SUITE("SubprocessCapturePure::IsSensitiveEnvName") {

    TEST_CASE("secret-bearing names are flagged (case-insensitive)") {
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("GH_TOKEN"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("gh_token"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("AWS_SECRET_ACCESS_KEY"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("OPENAI_API_KEY"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("MY_PASSWORD"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("DB_PASSWD"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("JIRA_PAT"));   // _PAT suffix
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("GITHUB_PAT")); // _PAT suffix (#1711)
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("gh_pat"));     // _PAT suffix, case-insensitive
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("GITHUB_TOKEN"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("SSH_PRIVATE_KEY"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("HTTP_AUTHORIZATION"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("AWS_SESSION_TOKEN"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("SOME_CREDENTIAL"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("HTTP_COOKIE"));
        CHECK(SubprocessCapturePure::IsSensitiveEnvName("KEY_PASSPHRASE"));
    }

    TEST_CASE("tool-essential names survive the scrub") {
        // The vars p4 / git / file-pickers actually need must NOT be flagged.
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("PATH"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("SYSTEMROOT"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("windir"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("TEMP"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("TMP"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("HOME"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("LANG"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("LC_ALL"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("P4PORT"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("P4USER"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("P4CLIENT"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("P4CONFIG"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("GIT_DIR"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("DISPLAY"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("XDG_RUNTIME_DIR"));
        // #1711 — *_PATH vars must survive: the "_PAT" token used to substring-match
        // "_PATH", scrubbing these from p4/git children so they failed to load shared
        // libraries on Linux/macOS. "_PAT" is now a suffix match, so "..._PATH" is safe.
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("LD_LIBRARY_PATH"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("DYLD_LIBRARY_PATH"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("GIT_EXEC_PATH"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("PKG_CONFIG_PATH"));
        CHECK_FALSE(SubprocessCapturePure::IsSensitiveEnvName("ld_library_path")); // case-insensitive
    }
}

TEST_SUITE("SubprocessCapturePure::ScrubSensitiveEnv") {

    TEST_CASE("drops sensitive entries, preserves order of the rest") {
        std::vector<std::pair<std::string, std::string>> parent;
        parent.push_back(std::make_pair("PATH", "/usr/bin"));
        parent.push_back(std::make_pair("GH_TOKEN", "ghp_xxx"));
        parent.push_back(std::make_pair("P4PORT", "ssl:host:1666"));
        parent.push_back(std::make_pair("AWS_SECRET_ACCESS_KEY", "abc"));
        parent.push_back(std::make_pair("LANG", "en_US.UTF-8"));
        const auto kept = SubprocessCapturePure::ScrubSensitiveEnv(parent);
        REQUIRE(kept.size() == 3);
        CHECK(kept[0].first == "PATH");
        CHECK(kept[1].first == "P4PORT");
        CHECK(kept[2].first == "LANG");
    }

    TEST_CASE("empty input yields empty output") {
        const auto kept = SubprocessCapturePure::ScrubSensitiveEnv({});
        CHECK(kept.empty());
    }

    TEST_CASE("all-sensitive input yields empty output") {
        std::vector<std::pair<std::string, std::string>> parent;
        parent.push_back(std::make_pair("API_KEY", "1"));
        parent.push_back(std::make_pair("SECRET", "2"));
        const auto kept = SubprocessCapturePure::ScrubSensitiveEnv(parent);
        CHECK(kept.empty());
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
