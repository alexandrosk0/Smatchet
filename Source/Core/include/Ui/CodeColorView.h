#ifndef SMATCHET_CODE_COLOR_VIEW_H
#define SMATCHET_CODE_COLOR_VIEW_H

// CodeColorView — read-only syntax-colored code rendering for Smatchet.
// Per the slice-1 deliverable of code-syntax-coloring-and-tooltips.
// Extends Smatchet's existing C++-only `DrawColoredCppText` (CppSyntaxHighlight.h)
// to a polymorphic multi-language API. Vendored LDs (CPlusPlus / C / Lua / Glsl /
// Hlsl / Sql / AngelScript / Markdown) come from Source/Core/ThirdParty/
// ImGuiColorTextEdit/TextEditor.h; Python / Bash / Json are hand-rolled in
// CodeColorView.cpp (same shape as the in-tree Markdown() factory at
// TextEditor.cpp:3630).
// Lifecycle: tokenize-once cached by (content_hash, lang, theme_revision) —
// first render parses, subsequent frames replay; a theme switch bumps
// SmatchetTheme::GetThemeRevision() so the key misses and the palette is
// re-resolved (slice 3). Cache shape mirrors the existing s_messageHeightCache
// in SmatchetAiAssistantUi.cpp.
// This header is intentionally ImGui-free in its public surface so pure-logic
// doctests can include it without pulling ImGui. The .cpp implementation
// includes ImGui + TextEditor.h.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace smatchet {
namespace code_color {

/// Languages CodeColorView can render. Vendored-first then hand-rolled per
/// the LD-availability tier (TextEditor.h:148-159 for the vendored set).
/// `Plain` is the always-available no-LD fallback — flat-orange tint.
enum class CodeLang : std::uint8_t {
    Plain = 0,
    CPlusPlus,
    C,
    Lua,
    Glsl,
    Hlsl,
    Sql,
    AngelScript,
    Markdown,
    Json,
    Bash,
    Python,
};

/// Token-palette enum mirrors the first 12 values of
/// TextEditor::PaletteIndex (Default..MultiLineComment) so pure-logic tests
/// can name palette buckets without including TextEditor.h. The render layer
/// maps each value onto a SmatchetThemeSyntaxColors field — see
/// CodeColorView.cpp PaletteToThemeColor for the canonical map.
enum class TokenPalette : std::uint8_t {
    Default = 0,
    Keyword = 1,
    Number = 2,
    String = 3,
    CharLiteral = 4,
    Punctuation = 5,
    Preprocessor = 6,
    Identifier = 7,
    KnownIdentifier = 8,
    PreprocIdentifier = 9,
    Comment = 10,
    MultiLineComment = 11,
};

/// One tokenization span. `start` is the byte offset in the source input;
/// `length` is in bytes (UTF-8 aware via the LD's own regex coverage).
struct Token {
    int start = 0;
    int length = 0;
    TokenPalette palette = TokenPalette::Default;
};

/// Map a markdown fenced-code language tag to a CodeLang. Case-insensitive.
/// Empty / unknown / "txt" / "text" / "plaintext" map to Plain.
/// Full alias coverage per code-syntax-coloring-and-tooltips
/// § Slice 1 step 2.
CodeLang FromTag(const std::string& tag);

/// Infer language from a file path's extension (case-insensitive). Path may
/// be a depot path (`//depot/foo/bar.cpp`), absolute (`C:/foo/bar.lua`), or
/// bare basename (`script.py`). Strips everything before the last `/`, `\`,
/// or `.` separator + delegates the extension to `FromTag`. Unknown
/// extensions / paths with no extension map to Plain. Used by Slice 5 of
/// code-syntax-coloring-and-tooltips — `AnnotateAnalysisUi`
/// annotate view + future `SmatchetFieldRender` callstack tooltips that
/// carry a file-path hint.
CodeLang LangFromFilePath(const std::string& path);

/// Human-readable name for the language — used by slice 4's hover tooltip.
/// Returns a static string; never null.
const char* CanonicalName(CodeLang lang);

/// Tokenize `utf8` (length `len`) using `lang`'s LanguageDefinition. Output is
/// appended to `outTokens` (caller may clear() first). Bounded: input above
/// 256 KiB is truncated with a single trailing `Default` token covering the
/// remainder so the caller sees a complete span list. Plain returns one
/// `Default` token covering the whole input — render layer paints it
/// flat-orange.
void Tokenize(const char* utf8, std::size_t len, CodeLang lang, std::vector<Token>& outTokens);

/// Draw `utf8` syntax-coloured for `lang`. Calls Tokenize internally (cached by
/// content hash + lang); per-token emits ImGui::Text* with theme-coloured
/// PushStyleColor. For CPlusPlus / C, delegates to the existing
/// DrawColoredCppText (CppSyntaxHighlight.h) so the existing test coverage
/// stays asserted on that path. Requires an active ImGui context.
void DrawColoredCode(const char* utf8, CodeLang lang);

/// As DrawColoredCode but for a multi-line block. Pre-render the block as one
/// `ImGui::TextUnformatted`-equivalent — slice 1 just calls DrawColoredCode;
/// slice 4 will overlay the language badge in the block's top-right corner
/// using `langTagOrigin` as the tooltip-body's "detected from" hint.
void DrawColoredCodeBlock(const char* utf8, CodeLang lang, const std::string& langTagOrigin = std::string());

/// Test-only accessor — returns the running count of cache-rebuild events
/// (first-time tokenize for a (hash, lang, theme_revision) key). Slice-3
/// doctest drives this through `TokenizeCachedForTest` (below) + a theme
/// switch + asserts the count increments. Production callers MUST NOT depend
/// on this counter; named with `ForTest` suffix per Smatchet convention
/// (mirrors `DictationInsertionRouter::RegisteredCountForTest`).
std::size_t GetCacheRebuildCountForTest();
/// Test-only — clear the cache so tests start from a known state.
void ResetCacheForTest();
/// Test-only — drives the production `TokenizeCached` path so slice-3 doctest
/// can exercise the (content_hash, lang, theme_revision) cache key + invalidation
/// without an ImGui context (DrawColoredCode is the production caller; needs ImGui).
/// Returns a shared, immutable handle to the tokenized span vector for the input
/// (CPP_CODE_AUDIT.md #24 — TokenizeCached itself returns this shape now, not a
/// reference into the cache, so a concurrent eviction can't dangle it); caller may
/// discard.
std::shared_ptr<const std::vector<Token>> TokenizeCachedForTest(const char* utf8, std::size_t len, CodeLang lang);

} // namespace code_color
} // namespace smatchet

#endif // SMATCHET_CODE_COLOR_VIEW_H
