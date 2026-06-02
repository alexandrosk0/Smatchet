// CodeColorView slice-1 doctest — pins the public surface that subsequent
// slices depend on:
//   - FromTag alias coverage (case-insensitive, alias-rich)
//   - CanonicalName per enum value
//   - Tokenize palette-histogram round-trip for each hand-rolled LD
//   - Cache hit/miss accounting via the SMATCHET_BUILD_TESTS-only accessors
//
// Pure-logic — pulls TextEditor.h transitively via CodeColorView.cpp but no
// ImGui context is needed (rendering is in DrawColoredCode, not covered here).

#include "CodeColorView.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace smatchet::code_color;

namespace {

std::unordered_map<TokenPalette, int> Histogram(const std::vector<Token>& tokens) {
    std::unordered_map<TokenPalette, int> h;
    for (const Token& t : tokens) {
        ++h[t.palette];
    }
    return h;
}

} // namespace

TEST_CASE("CodeColorView::FromTag — cpp / c++ family routes to CPlusPlus") {
    CHECK(FromTag("cpp") == CodeLang::CPlusPlus);
    CHECK(FromTag("c++") == CodeLang::CPlusPlus);
    CHECK(FromTag("cxx") == CodeLang::CPlusPlus);
    CHECK(FromTag("cc") == CodeLang::CPlusPlus);
    CHECK(FromTag("hpp") == CodeLang::CPlusPlus);
    CHECK(FromTag("h") == CodeLang::CPlusPlus);
    CHECK(FromTag("hh") == CodeLang::CPlusPlus);
    CHECK(FromTag("hxx") == CodeLang::CPlusPlus);
}

TEST_CASE("CodeColorView::FromTag — bare 'c' routes to C, not CPlusPlus") { CHECK(FromTag("c") == CodeLang::C); }

TEST_CASE("CodeColorView::FromTag — case-insensitive") {
    CHECK(FromTag("CPP") == CodeLang::CPlusPlus);
    CHECK(FromTag("Cpp") == CodeLang::CPlusPlus);
    CHECK(FromTag("PYTHON") == CodeLang::Python);
    CHECK(FromTag("JSON") == CodeLang::Json);
}

TEST_CASE("CodeColorView::FromTag — scripting / shading / data aliases") {
    CHECK(FromTag("py") == CodeLang::Python);
    CHECK(FromTag("python") == CodeLang::Python);
    CHECK(FromTag("python3") == CodeLang::Python);
    CHECK(FromTag("lua") == CodeLang::Lua);
    CHECK(FromTag("glsl") == CodeLang::Glsl);
    CHECK(FromTag("vert") == CodeLang::Glsl);
    CHECK(FromTag("frag") == CodeLang::Glsl);
    CHECK(FromTag("hlsl") == CodeLang::Hlsl);
    CHECK(FromTag("sql") == CodeLang::Sql);
    CHECK(FromTag("as") == CodeLang::AngelScript);
    CHECK(FromTag("angelscript") == CodeLang::AngelScript);
    CHECK(FromTag("md") == CodeLang::Markdown);
    CHECK(FromTag("markdown") == CodeLang::Markdown);
    CHECK(FromTag("json") == CodeLang::Json);
    CHECK(FromTag("jsonc") == CodeLang::Json);
    CHECK(FromTag("bash") == CodeLang::Bash);
    CHECK(FromTag("sh") == CodeLang::Bash);
    CHECK(FromTag("zsh") == CodeLang::Bash);
    CHECK(FromTag("shell") == CodeLang::Bash);
}

TEST_CASE("CodeColorView::FromTag — empty / txt / unknown → Plain") {
    CHECK(FromTag("") == CodeLang::Plain);
    CHECK(FromTag("txt") == CodeLang::Plain);
    CHECK(FromTag("text") == CodeLang::Plain);
    CHECK(FromTag("plaintext") == CodeLang::Plain);
    CHECK(FromTag("bogus-language-name") == CodeLang::Plain);
    CHECK(FromTag("a-tag-that-doesnt-exist") == CodeLang::Plain);
}

TEST_CASE("CodeColorView::CanonicalName — every enum value resolves") {
    CHECK(std::string(CanonicalName(CodeLang::Plain)) == "Plain text");
    CHECK(std::string(CanonicalName(CodeLang::CPlusPlus)) == "C++");
    CHECK(std::string(CanonicalName(CodeLang::C)) == "C");
    CHECK(std::string(CanonicalName(CodeLang::Lua)) == "Lua");
    CHECK(std::string(CanonicalName(CodeLang::Glsl)) == "GLSL");
    CHECK(std::string(CanonicalName(CodeLang::Hlsl)) == "HLSL");
    CHECK(std::string(CanonicalName(CodeLang::Sql)) == "SQL");
    CHECK(std::string(CanonicalName(CodeLang::AngelScript)) == "AngelScript");
    CHECK(std::string(CanonicalName(CodeLang::Markdown)) == "Markdown");
    CHECK(std::string(CanonicalName(CodeLang::Json)) == "JSON");
    CHECK(std::string(CanonicalName(CodeLang::Bash)) == "Bash");
    CHECK(std::string(CanonicalName(CodeLang::Python)) == "Python");
}

TEST_CASE("CodeColorView::Tokenize — Plain emits a single Default token") {
    ResetCacheForTest();
    const char src[] = "anything at all here";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::Plain, toks);
    REQUIRE(toks.size() == 1u);
    CHECK(toks[0].start == 0);
    CHECK(toks[0].length == static_cast<int>(std::strlen(src)));
    CHECK(toks[0].palette == TokenPalette::Default);
}

TEST_CASE("CodeColorView::Tokenize — Python emits Keyword + Identifier per `def foo():`") {
    ResetCacheForTest();
    const char src[] = "def foo(): pass";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::Python, toks);
    REQUIRE(!toks.empty());
    const auto h = Histogram(toks);
    // `def` + `pass` → 2 keyword tokens (Python LD lookup re-tags Identifier).
    CHECK(h.count(TokenPalette::Keyword) >= 1);
    // `foo` → 1 Identifier.
    CHECK(h.count(TokenPalette::Identifier) >= 1);
    // `(`, `)`, `:` → ≥ 1 Punctuation token.
    CHECK(h.count(TokenPalette::Punctuation) >= 1);
}

TEST_CASE("CodeColorView::Tokenize — Python `#` line-comment routes to Comment") {
    ResetCacheForTest();
    const char src[] = "# this is a comment\nx = 1";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::Python, toks);
    const auto h = Histogram(toks);
    CHECK(h.count(TokenPalette::Comment) >= 1);
    CHECK(h.count(TokenPalette::Number) >= 1); // `1`
}

TEST_CASE("CodeColorView::Tokenize — Bash recognises `if`/`then`/`fi` + `$VAR`") {
    ResetCacheForTest();
    const char src[] = "if [ -z \"$NAME\" ]; then echo $NAME; fi";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::Bash, toks);
    const auto h = Histogram(toks);
    // `if`, `then`, `fi` → 3 Keyword.
    CHECK(h.count(TokenPalette::Keyword) >= 1);
    // `$NAME` ×2 → Preprocessor palette per the Bash LD.
    CHECK(h.count(TokenPalette::Preprocessor) >= 1);
    // The string literal "$NAME" content.
    CHECK(h.count(TokenPalette::String) >= 1);
}

TEST_CASE("CodeColorView::Tokenize — JSON recognises true / false / null + numbers + strings") {
    ResetCacheForTest();
    const char src[] = "{\"a\": 1, \"b\": true, \"c\": null}";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::Json, toks);
    const auto h = Histogram(toks);
    // `true` and `null` are keywords.
    CHECK(h.count(TokenPalette::Keyword) >= 1);
    // Strings "a", "b", "c".
    CHECK(h.count(TokenPalette::String) >= 1);
    // Number 1.
    CHECK(h.count(TokenPalette::Number) >= 1);
    // Punctuation {} : , .
    CHECK(h.count(TokenPalette::Punctuation) >= 1);
}

TEST_CASE("CodeColorView::Tokenize — C++ block comment spans to its terminator (ScanBlockComment seam)") {
    ResetCacheForTest();
    const char src[] = "int x; /* multi\nline comment */ int y;";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::CPlusPlus, toks);
    const auto h = Histogram(toks);
    // The /* ... */ run collapses into a single MultiLineComment token.
    CHECK(h.count(TokenPalette::MultiLineComment) >= 1);
    // Keywords `int` survive on both sides of the comment.
    CHECK(h.count(TokenPalette::Keyword) >= 1);
}

TEST_CASE("CodeColorView::Tokenize — keyword reclassification re-tags Identifier (ReclassifyIdentifier seam)") {
    ResetCacheForTest();
    // `return` is a C++ keyword; `myVar` is a bare identifier. The shared
    // ReclassifyIdentifier helper must re-tag the former and leave the latter.
    const char src[] = "return myVar;";
    std::vector<Token> toks;
    Tokenize(src, std::strlen(src), CodeLang::CPlusPlus, toks);
    const auto h = Histogram(toks);
    CHECK(h.count(TokenPalette::Keyword) >= 1);    // `return`
    CHECK(h.count(TokenPalette::Identifier) >= 1); // `myVar`
}

TEST_CASE("CodeColorView::Tokenize — input cap silently truncates at 256 KiB") {
    ResetCacheForTest();
    // 300 KiB of `x`-padding — more than the cap.
    std::string huge(300 * 1024, 'x');
    std::vector<Token> toks;
    Tokenize(huge.data(), huge.size(), CodeLang::Plain, toks);
    REQUIRE(!toks.empty());
    // Plain emits a single Default token covering only the capped portion.
    CHECK(toks[0].length == 256 * 1024);
}

TEST_CASE("CodeColorView cache — repeated Tokenize on same (hash, lang) does NOT bump rebuild count") {
    ResetCacheForTest();
    const char src[] = "def foo(): pass";
    std::vector<Token> first;
    Tokenize(src, std::strlen(src), CodeLang::Python, first);
    // Direct Tokenize() doesn't touch the cache (cache lives behind
    // TokenizeCached which DrawColoredCode uses). Cache invariants tested
    // via TokenizeCachedForTest in the slice-3 cases below.
    CHECK(GetCacheRebuildCountForTest() == 0u);
}

// =============================================================================
// Slice 3 — theme-revision cache invalidation
// =============================================================================

#include "SmatchetTheme.h"
#include "SmatchetThemeIds.h"

namespace {

// Slice-3 cases need an ImGui context because SmatchetTheme::ApplyStyle
// touches ImGui::GetStyle(). Same shape as SmatchetThemeAiColors.test.cpp's
// ImGuiCtxFixture; local clone so this test TU stays self-contained.
struct ImGuiCtxFixture {
    ImGuiCtxFixture() : ctx_(ImGui::CreateContext()) {}
    ~ImGuiCtxFixture() {
        if (ctx_ != nullptr) {
            ImGui::DestroyContext(ctx_);
        }
    }
    ImGuiContext* ctx_;
};

} // namespace

TEST_CASE_FIXTURE(ImGuiCtxFixture,
                  "CodeColorView cache — TokenizeCachedForTest on same input bumps rebuild count exactly once") {
    ResetCacheForTest();
    const char src[] = "def foo(): pass";
    // Pin theme so subsequent Apply doesn't perturb the rev unexpectedly.
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const std::size_t before = GetCacheRebuildCountForTest();
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == before + 1u);
    // Second call against same key — cache hit, no rebuild.
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == before + 1u);
    // Third call — same input, different lang → different cache key, rebuild.
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Bash);
    CHECK(GetCacheRebuildCountForTest() == before + 2u);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "CodeColorView cache — theme switch invalidates the per-key entry") {
    ResetCacheForTest();
    const char src[] = "x = 1";
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const std::uint64_t rev0 = SmatchetTheme::GetThemeRevision();
    const std::size_t before = GetCacheRebuildCountForTest();
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == before + 1u);
    // Same input + lang against same theme — cache hit.
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == before + 1u);
    // Theme switch bumps GetThemeRevision; subsequent lookup misses the
    // stale-rev entry + rebuilds.
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    CHECK(SmatchetTheme::GetThemeRevision() > rev0);
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == before + 2u);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "CodeColorView cache — theme bounce returns to a fresh key (no carry-over hit)") {
    ResetCacheForTest();
    const char src[] = "y = 2";
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    const std::size_t afterDark1 = GetCacheRebuildCountForTest();
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    const std::size_t afterLight = GetCacheRebuildCountForTest();
    CHECK(afterLight == afterDark1 + 1u);
    // Bouncing back to SmatchetDark — theme revision is now further-ahead
    // monotonically, so the original SmatchetDark@rev0 entry is unreachable.
    // Cache miss → rebuild (the SmatchetDark@rev2 key is new).
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    TokenizeCachedForTest(src, std::strlen(src), CodeLang::Python);
    CHECK(GetCacheRebuildCountForTest() == afterLight + 1u);
}

TEST_CASE_FIXTURE(ImGuiCtxFixture, "SmatchetTheme::GetThemeRevision — monotonic per ApplyStyle call") {
    const std::uint64_t r0 = SmatchetTheme::GetThemeRevision();
    SmatchetTheme::ApplyStyle(ThemeId::SmatchetDark);
    const std::uint64_t r1 = SmatchetTheme::GetThemeRevision();
    CHECK(r1 > r0);
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const std::uint64_t r2 = SmatchetTheme::GetThemeRevision();
    CHECK(r2 > r1);
    // Re-apply same theme — still bumps (every ApplyStyle is a write).
    SmatchetTheme::ApplyStyle(ThemeId::Vs2022Light);
    const std::uint64_t r3 = SmatchetTheme::GetThemeRevision();
    CHECK(r3 > r2);
}
