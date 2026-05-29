#include <doctest/doctest.h>

#include "MarkdownPreviewRender.h"

#include <string>

// Pure-logic round-trip on the fenced-code lang-tag classifier. The body of `MD_BLOCK_CODE`
// leave handler in `MarkdownPreviewRender.cpp` routes only through `IsCppLikeLangTag`, so this
// is the deterministic seam for "does ```cpp colorize?" without needing an ImGui context or
// md4c integration test.

TEST_CASE("IsCppLikeLangTag accepts C / C++ canonical spellings") {
    using MarkdownPreviewRender::IsCppLikeLangTag;
    CHECK(IsCppLikeLangTag("cpp"));
    CHECK(IsCppLikeLangTag("c++"));
    CHECK(IsCppLikeLangTag("cxx"));
    CHECK(IsCppLikeLangTag("cc"));
    CHECK(IsCppLikeLangTag("c"));
    CHECK(IsCppLikeLangTag("hpp"));
    CHECK(IsCppLikeLangTag("h"));
}

TEST_CASE("IsCppLikeLangTag is case-insensitive") {
    using MarkdownPreviewRender::IsCppLikeLangTag;
    CHECK(IsCppLikeLangTag("CPP"));
    CHECK(IsCppLikeLangTag("C++"));
    CHECK(IsCppLikeLangTag("Cpp"));
    CHECK(IsCppLikeLangTag("HPP"));
    CHECK(IsCppLikeLangTag("H"));
}

TEST_CASE("IsCppLikeLangTag rejects non-C/C++ languages") {
    using MarkdownPreviewRender::IsCppLikeLangTag;
    CHECK_FALSE(IsCppLikeLangTag(""));
    CHECK_FALSE(IsCppLikeLangTag("python"));
    CHECK_FALSE(IsCppLikeLangTag("py"));
    CHECK_FALSE(IsCppLikeLangTag("javascript"));
    CHECK_FALSE(IsCppLikeLangTag("js"));
    CHECK_FALSE(IsCppLikeLangTag("rust"));
    CHECK_FALSE(IsCppLikeLangTag("go"));
    CHECK_FALSE(IsCppLikeLangTag("java"));
    CHECK_FALSE(IsCppLikeLangTag("kotlin"));
    CHECK_FALSE(IsCppLikeLangTag("typescript"));
    CHECK_FALSE(IsCppLikeLangTag("html"));
    CHECK_FALSE(IsCppLikeLangTag("css"));
    CHECK_FALSE(IsCppLikeLangTag("json"));
    CHECK_FALSE(IsCppLikeLangTag("yaml"));
    CHECK_FALSE(IsCppLikeLangTag("bash"));
    CHECK_FALSE(IsCppLikeLangTag("shell"));
    CHECK_FALSE(IsCppLikeLangTag("sh"));
}

TEST_CASE("IsCppLikeLangTag does not prefix-match") {
    using MarkdownPreviewRender::IsCppLikeLangTag;
    // Real-world md4c lang strings are the entire info-string token before the first space,
    // so substrings like "cppreference" or "ccache" must not falsely trigger.
    CHECK_FALSE(IsCppLikeLangTag("cppreference"));
    CHECK_FALSE(IsCppLikeLangTag("ccache"));
    CHECK_FALSE(IsCppLikeLangTag("cxxabi"));
    CHECK_FALSE(IsCppLikeLangTag("hpp-version"));
    CHECK_FALSE(IsCppLikeLangTag("c-sharp"));
    CHECK_FALSE(IsCppLikeLangTag("c#"));
    CHECK_FALSE(IsCppLikeLangTag("objective-c"));
}

TEST_CASE("IsCppLikeLangTag treats whitespace-only and bare-fence tags as non-cpp") {
    using MarkdownPreviewRender::IsCppLikeLangTag;
    // md4c hands the lang string verbatim; if the author wrote ``` with no info, the lang is
    // empty. Anything other than a recognised token must fall through to plain rendering.
    CHECK_FALSE(IsCppLikeLangTag(""));
    CHECK_FALSE(IsCppLikeLangTag(" "));
    CHECK_FALSE(IsCppLikeLangTag("  "));
    CHECK_FALSE(IsCppLikeLangTag("\t"));
}
