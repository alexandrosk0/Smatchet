#include "CodeColorView.h"

#include "CppSyntaxHighlight.h"
#include "Logger.h"
#include "SmatchetTheme.h"

#include "TextEditor.h"
#include "imgui.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>
#include <unordered_map>

namespace smatchet {
namespace code_color {

namespace {

// Tag → CodeLang alias map
// Case-insensitive, alias-rich per code-syntax-coloring-and-tooltips
// § Slice 1 step 2. Order doesn't matter; linear-scan lookup against a small
// constant table (~30 entries) is faster than std::unordered_map for this
// size and gives compile-time-checkable coverage.

struct TagAlias {
    const char* tag;
    CodeLang lang;
};

constexpr TagAlias kAliases[] = {
    // CPlusPlus — note .h / .hpp / .hh / .hxx are routed to CPlusPlus, not C,
    // for back-compat with the existing IsCppLikeLangTag() coverage and
    // because Smatchet's own `.h` files are C++.
    {"cpp", CodeLang::CPlusPlus},
    {"c++", CodeLang::CPlusPlus},
    {"cxx", CodeLang::CPlusPlus},
    {"cc", CodeLang::CPlusPlus},
    {"hpp", CodeLang::CPlusPlus},
    {"h", CodeLang::CPlusPlus},
    {"hh", CodeLang::CPlusPlus},
    {"hxx", CodeLang::CPlusPlus},
    // C — only the literal "c" tag; not .h headers (per above).
    {"c", CodeLang::C},
    // Shading languages
    {"glsl", CodeLang::Glsl},
    {"vert", CodeLang::Glsl},
    {"frag", CodeLang::Glsl},
    {"hlsl", CodeLang::Hlsl},
    // Scripting
    {"lua", CodeLang::Lua},
    {"py", CodeLang::Python},
    {"python", CodeLang::Python},
    {"python3", CodeLang::Python},
    {"bash", CodeLang::Bash},
    {"sh", CodeLang::Bash},
    {"zsh", CodeLang::Bash},
    {"shell", CodeLang::Bash},
    // Data / misc
    {"sql", CodeLang::Sql},
    {"as", CodeLang::AngelScript},
    {"angelscript", CodeLang::AngelScript},
    {"md", CodeLang::Markdown},
    {"markdown", CodeLang::Markdown},
    {"json", CodeLang::Json},
    {"jsonc", CodeLang::Json},
    // Plain — empty / explicit
    {"", CodeLang::Plain},
    {"txt", CodeLang::Plain},
    {"text", CodeLang::Plain},
    {"plaintext", CodeLang::Plain},
};

std::string ToLower(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

// Hand-rolled LanguageDefinitions for Python / Bash / Json
// Same pattern as TextEditor.cpp's Markdown() factory at line 3630 — populate
// the LD's keyword set + regex strings + comment delimiters, leave mTokenize
// nullptr so the regex-fallback path drives tokenization. Lazy init via
// function-local statics; thread-safe per C++14 ctor semantics.

const TextEditor::LanguageDefinition& PythonLd() {
    static bool inited = false;
    static TextEditor::LanguageDefinition langDef;
    if (!inited) {
        // 35 reserved keywords (Python 3.x). Pinned by the slice-1 doctest's
        // palette-index histogram so drift fails loudly.
        static const char* const kKeywords[] = {
            "False", "None",     "True",  "and",    "as",   "assert", "async",  "await",    "break",
            "class", "continue", "def",   "del",    "elif", "else",   "except", "finally",  "for",
            "from",  "global",   "if",    "import", "in",   "is",     "lambda", "nonlocal", "not",
            "or",    "pass",     "raise", "return", "try",  "while",  "with",   "yield"};
        for (const char* k : kKeywords) {
            langDef.mKeywords.insert(k);
        }
        // 30 common builtins — tagged via mIdentifiers so they render as
        // KnownIdentifier (Identifier palette by default).
        static const char* const kBuiltins[] = {
            "print", "len",  "range",      "list",      "dict", "set",   "tuple",  "int",    "str",      "float",
            "bool",  "type", "isinstance", "enumerate", "zip",  "map",   "filter", "sorted", "reversed", "min",
            "max",   "sum",  "any",        "all",       "open", "super", "iter",   "next",   "repr",     "hash"};
        for (const char* b : kBuiltins) {
            TextEditor::Identifier id;
            id.mDeclaration = "Python builtin";
            langDef.mIdentifiers.insert(std::make_pair(std::string(b), id));
        }
        // Comment + literal regexes. `#`-line-comments; triple-quoted blocks
        // collapsed to single-quote-strings (the vendored regex engine is
        // greedy-aware so `\"\"\"[\\s\\S]*?\"\"\"` matches a triple-string).
        langDef.mTokenRegexStrings.push_back({"\"\"\"[\\s\\S]*?\"\"\"", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back({"\'\'\'[\\s\\S]*?\'\'\'", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back({"\\\"(\\\\.|[^\\\"])*\\\"", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back({"\\\'(\\\\.|[^\\\'])*\\\'", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back(
            {"[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?", TextEditor::PaletteIndex::Number});
        langDef.mTokenRegexStrings.push_back({"@[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Preprocessor});
        langDef.mTokenRegexStrings.push_back({"[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Identifier});
        langDef.mTokenRegexStrings.push_back({"[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]",
                                              TextEditor::PaletteIndex::Punctuation});

        langDef.mCommentStart = "\"\"\""; // approximate — triple-quoted docstring
        langDef.mCommentEnd = "\"\"\"";
        langDef.mSingleLineComment = "#";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;
        langDef.mName = "Python";
        langDef.mTokenize = nullptr; // regex-only path
        inited = true;
    }
    return langDef;
}

const TextEditor::LanguageDefinition& BashLd() {
    static bool inited = false;
    static TextEditor::LanguageDefinition langDef;
    if (!inited) {
        static const char* const kKeywords[] = {"if",     "then",  "else",     "elif", "fi",   "case",     "esac",
                                                "for",    "while", "until",    "do",   "done", "function", "select",
                                                "return", "break", "continue", "in",   "time"};
        for (const char* k : kKeywords) {
            langDef.mKeywords.insert(k);
        }
        static const char* const kBuiltins[] = {"echo",  "cd",       "pwd",     "export", "source", "alias", "unalias",
                                                "local", "readonly", "declare", "unset",  "read",   "set",   "shift",
                                                "trap",  "exec",     "eval",    "exit",   "wait",   "test"};
        for (const char* b : kBuiltins) {
            TextEditor::Identifier id;
            id.mDeclaration = "Bash builtin";
            langDef.mIdentifiers.insert(std::make_pair(std::string(b), id));
        }
        langDef.mTokenRegexStrings.push_back({"\\\"(\\\\.|[^\\\"])*\\\"", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back({"\\\'(\\\\.|[^\\\'])*\\\'", TextEditor::PaletteIndex::String});
        // $VAR + ${VAR} + $1..$9 — preprocessor palette since they're
        // metacharacters that change shell behaviour.
        langDef.mTokenRegexStrings.push_back({"\\$\\{[^\\}]*\\}", TextEditor::PaletteIndex::Preprocessor});
        langDef.mTokenRegexStrings.push_back({"\\$[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Preprocessor});
        langDef.mTokenRegexStrings.push_back({"\\$[0-9]+", TextEditor::PaletteIndex::Preprocessor});
        langDef.mTokenRegexStrings.push_back({"[+-]?[0-9]+", TextEditor::PaletteIndex::Number});
        langDef.mTokenRegexStrings.push_back({"[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Identifier});
        langDef.mTokenRegexStrings.push_back({"[\\[\\]\\{\\}\\!\\%\\^\\&\\*\\(\\)\\-\\+\\=\\~\\|\\<\\>\\?\\/\\;\\,\\.]",
                                              TextEditor::PaletteIndex::Punctuation});

        langDef.mCommentStart = "";
        langDef.mCommentEnd = "";
        langDef.mSingleLineComment = "#";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = true;
        langDef.mName = "Bash";
        langDef.mTokenize = nullptr;
        inited = true;
    }
    return langDef;
}

const TextEditor::LanguageDefinition& JsonLd() {
    static bool inited = false;
    static TextEditor::LanguageDefinition langDef;
    if (!inited) {
        // Strict-JSON keywords. JSONC tag accepted as alias but the comment
        // delimiters below tolerate `//` so the tag handles both modes.
        langDef.mKeywords.insert("true");
        langDef.mKeywords.insert("false");
        langDef.mKeywords.insert("null");
        langDef.mTokenRegexStrings.push_back({"\\\"(\\\\.|[^\\\"])*\\\"", TextEditor::PaletteIndex::String});
        langDef.mTokenRegexStrings.push_back(
            {"[+-]?([0-9]+([.][0-9]*)?|[.][0-9]+)([eE][+-]?[0-9]+)?", TextEditor::PaletteIndex::Number});
        langDef.mTokenRegexStrings.push_back({"[a-zA-Z_][a-zA-Z0-9_]*", TextEditor::PaletteIndex::Identifier});
        langDef.mTokenRegexStrings.push_back({"[\\[\\]\\{\\}:,]", TextEditor::PaletteIndex::Punctuation});

        // JSONC tolerance — `//` line comments + `/* */` block comments. Strict
        // JSON forbids them but viewers commonly accept them; if the input is
        // strict-JSON these regexes never match anything.
        langDef.mCommentStart = "/*";
        langDef.mCommentEnd = "*/";
        langDef.mSingleLineComment = "//";

        langDef.mCaseSensitive = true;
        langDef.mAutoIndentation = false;
        langDef.mName = "JSON";
        langDef.mTokenize = nullptr;
        inited = true;
    }
    return langDef;
}

// LD lookup

const TextEditor::LanguageDefinition* LdForLang(CodeLang lang) {
    switch (lang) {
    case CodeLang::CPlusPlus:
        return &TextEditor::LanguageDefinition::CPlusPlus();
    case CodeLang::C:
        return &TextEditor::LanguageDefinition::C();
    case CodeLang::Lua:
        return &TextEditor::LanguageDefinition::Lua();
    case CodeLang::Glsl:
        return &TextEditor::LanguageDefinition::GLSL();
    case CodeLang::Hlsl:
        return &TextEditor::LanguageDefinition::HLSL();
    case CodeLang::Sql:
        return &TextEditor::LanguageDefinition::SQL();
    case CodeLang::AngelScript:
        return &TextEditor::LanguageDefinition::AngelScript();
    case CodeLang::Markdown:
        return &TextEditor::LanguageDefinition::Markdown();
    case CodeLang::Json:
        return &JsonLd();
    case CodeLang::Bash:
        return &BashLd();
    case CodeLang::Python:
        return &PythonLd();
    case CodeLang::Plain:
    default:
        return nullptr;
    }
}

// Tokenizer — single-pass left-to-right over the LD's tables

constexpr std::size_t kInputCap = 256 * 1024; // 256 KiB

TokenPalette ToTokenPalette(TextEditor::PaletteIndex p) {
    // Slot indices match the first 12 values of TextEditor::PaletteIndex; the
    // tail (Background.. Max) is editor chrome, never emitted by the tokenizer.
    const auto v = static_cast<std::uint8_t>(p);
    if (v <= static_cast<std::uint8_t>(TextEditor::PaletteIndex::MultiLineComment)) {
        return static_cast<TokenPalette>(v);
    }
    return TokenPalette::Default;
}

// Re-tag an Identifier token against the LD's keyword / known-identifier /
// preproc-identifier tables. No-op for any other palette. Shared verbatim by
// the LD-tokenize-callback path and the regex-fallback path (steps 4 + 5).
void ReclassifyIdentifier(const char* tok_begin, const char* tok_end, const TextEditor::LanguageDefinition& ld,
                          Token& t) {
    if (t.palette != TokenPalette::Identifier) {
        return;
    }
    std::string tok(tok_begin, tok_end);
    const std::string lookup = ld.mCaseSensitive ? tok : ToLower(tok);
    if (ld.mKeywords.find(lookup) != ld.mKeywords.end()) {
        t.palette = TokenPalette::Keyword;
    } else if (ld.mIdentifiers.find(lookup) != ld.mIdentifiers.end()) {
        t.palette = TokenPalette::KnownIdentifier;
    } else if (ld.mPreprocIdentifiers.find(lookup) != ld.mPreprocIdentifiers.end()) {
        t.palette = TokenPalette::PreprocIdentifier;
    }
}

// Step 1 — whitespace run as a Default-palette span (no colour, preserves byte
// offsets). Returns true + advances `p` when it consumed a span.
bool ScanWhitespace(const char* begin, const char*& p, const char* end, std::vector<Token>& outTokens) {
    if (!std::isspace(static_cast<unsigned char>(*p))) {
        return false;
    }
    const char* ws_start = p;
    while (p < end && std::isspace(static_cast<unsigned char>(*p))) {
        ++p;
    }
    Token t;
    t.start = static_cast<int>(ws_start - begin);
    t.length = static_cast<int>(p - ws_start);
    t.palette = TokenPalette::Default;
    outTokens.push_back(t);
    return true;
}

// Step 2 — single-line comment shortcut (faster than regex; LDs that need it
// always set mSingleLineComment).
bool ScanSingleLineComment(const char* begin, const char*& p, const char* end, const TextEditor::LanguageDefinition& ld,
                           std::vector<Token>& outTokens) {
    if (ld.mSingleLineComment.empty() || static_cast<std::size_t>(end - p) < ld.mSingleLineComment.size() ||
        std::memcmp(p, ld.mSingleLineComment.c_str(), ld.mSingleLineComment.size()) != 0) {
        return false;
    }
    const char* line_end = p;
    while (line_end < end && *line_end != '\n') {
        ++line_end;
    }
    Token t;
    t.start = static_cast<int>(p - begin);
    t.length = static_cast<int>(line_end - p);
    t.palette = TokenPalette::Comment;
    outTokens.push_back(t);
    p = line_end;
    return true;
}

// Step 3 — block-comment shortcut.
bool ScanBlockComment(const char* begin, const char*& p, const char* end, const TextEditor::LanguageDefinition& ld,
                      std::vector<Token>& outTokens) {
    if (ld.mCommentStart.empty() || ld.mCommentEnd.empty() ||
        static_cast<std::size_t>(end - p) < ld.mCommentStart.size() ||
        std::memcmp(p, ld.mCommentStart.c_str(), ld.mCommentStart.size()) != 0) {
        return false;
    }
    const char* block_end = p + ld.mCommentStart.size();
    while (block_end < end) {
        if (static_cast<std::size_t>(end - block_end) >= ld.mCommentEnd.size() &&
            std::memcmp(block_end, ld.mCommentEnd.c_str(), ld.mCommentEnd.size()) == 0) {
            block_end += ld.mCommentEnd.size();
            break;
        }
        ++block_end;
    }
    Token t;
    t.start = static_cast<int>(p - begin);
    t.length = static_cast<int>(block_end - p);
    t.palette = TokenPalette::MultiLineComment;
    outTokens.push_back(t);
    p = block_end;
    return true;
}

// Step 4 — LD-supplied tokenize callback (set by CPlusPlus / Lua / etc.).
bool ScanLdTokenize(const char* begin, const char*& p, const char* end, const TextEditor::LanguageDefinition& ld,
                    std::vector<Token>& outTokens) {
    if (!ld.mTokenize) {
        return false;
    }
    const char* out_begin = nullptr;
    const char* out_end = nullptr;
    TextEditor::PaletteIndex idx = TextEditor::PaletteIndex::Default;
    if (!ld.mTokenize(p, end, out_begin, out_end, idx) || out_end <= out_begin) {
        return false;
    }
    Token t;
    t.start = static_cast<int>(out_begin - begin);
    t.length = static_cast<int>(out_end - out_begin);
    t.palette = ToTokenPalette(idx);
    ReclassifyIdentifier(out_begin, out_end, ld, t);
    outTokens.push_back(t);
    p = out_end;
    return true;
}

// Step 5 — regex-fallback path. Picks the longest match across all patterns.
bool ScanRegexFallback(const char* begin, const char*& p, const char* end, const TextEditor::LanguageDefinition& ld,
                       std::vector<Token>& outTokens) {
    const char* best_end = nullptr;
    TextEditor::PaletteIndex best_idx = TextEditor::PaletteIndex::Default;
    for (const auto& kv : ld.mTokenRegexStrings) {
        try {
            std::regex re(kv.first);
            std::cmatch m;
            if (std::regex_search(p, end, m, re, std::regex_constants::match_continuous) && m.position(0) == 0) {
                const char* match_end = p + m.length(0);
                if (best_end == nullptr || match_end > best_end) {
                    best_end = match_end;
                    best_idx = kv.second;
                }
            }
        } catch (const std::regex_error&) {
            // Pathological LD regex — log once + skip. Defensive.
            LOG_WARN("CodeColorView: regex compile failed for LD pattern");
        }
    }
    if (best_end == nullptr || best_end <= p) {
        return false;
    }
    Token t;
    t.start = static_cast<int>(p - begin);
    t.length = static_cast<int>(best_end - p);
    t.palette = ToTokenPalette(best_idx);
    ReclassifyIdentifier(p, best_end, ld, t);
    outTokens.push_back(t);
    p = best_end;
    return true;
}

void TokenizeWithLd(const char* begin, const char* end, const TextEditor::LanguageDefinition& ld,
                    std::vector<Token>& outTokens) {
    const char* p = begin;
    while (p < end) {
        if (ScanWhitespace(begin, p, end, outTokens)) {
            continue;
        }
        if (ScanSingleLineComment(begin, p, end, ld, outTokens)) {
            continue;
        }
        if (ScanBlockComment(begin, p, end, ld, outTokens)) {
            continue;
        }
        if (ScanLdTokenize(begin, p, end, ld, outTokens)) {
            continue;
        }
        if (ScanRegexFallback(begin, p, end, ld, outTokens)) {
            continue;
        }

        // 6. Default — advance one byte as Default so the loop terminates.
        Token t;
        t.start = static_cast<int>(p - begin);
        t.length = 1;
        t.palette = TokenPalette::Default;
        outTokens.push_back(t);
        ++p;
    }
}

// Cache — (content_hash, lang) -> vector<Token>
// FNV-1a 64-bit hash; collision risk acceptable at this cache size (256 entries
// FIFO eviction). Slice 3 extends the key with theme_revision.

std::uint64_t Fnv1a64(const char* data, std::size_t len) {
    std::uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= static_cast<unsigned char>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

struct CacheKey {
    std::uint64_t contentHash;
    CodeLang lang;
    // Slice 3 of code-syntax-coloring-and-tooltips.
    // Snapshot of `SmatchetTheme::GetThemeRevision()` at insert time. Theme
    // swap bumps the live revision; lookup miss fires when the live value
    // doesn't equal the snapshot stored in the key, so per-token colour
    // tables stay current automatically. Same shape as the existing per-
    // theme palette invalidation in SmatchetThemeSyntaxColors.
    std::uint64_t themeRevision;
    bool operator==(const CacheKey& o) const {
        return contentHash == o.contentHash && lang == o.lang && themeRevision == o.themeRevision;
    }
};

struct CacheKeyHasher {
    std::size_t operator()(const CacheKey& k) const noexcept {
        return static_cast<std::size_t>(k.contentHash) ^ (static_cast<std::size_t>(k.lang) << 1) ^
               (static_cast<std::size_t>(k.themeRevision) << 17);
    }
};

constexpr std::size_t kCacheCap = 256;

std::mutex g_cacheMutex;
std::unordered_map<CacheKey, std::shared_ptr<const std::vector<Token>>, CacheKeyHasher> g_cache;
std::vector<CacheKey> g_cacheInsertionOrder;

// Cache-rebuild counter — bumped on every first-time tokenize for a (hash, lang)
// key. Production code never reads this; the slice-3 doctest uses it to assert
// theme-revision invalidation works. Always-present per Smatchet's *ForTest
// public-API convention.
std::size_t g_cacheRebuildCount = 0;

// CPP_CODE_AUDIT.md #24: returns a shared_ptr into an immutable, ref-counted entry
// rather than a reference into g_cache — a raw reference would dangle if a later
// concurrent call's FIFO eviction erased this exact key while the caller was still
// iterating the (unlocked) returned tokens. Unlike returning by value, the shared_ptr
// keeps the DrawColoredCode hot path zero-copy on a cache hit (code review flagged the
// by-value version as a per-frame vector<Token> copy against this dir's ≤6.94 ms/144 Hz
// UI budget) while still giving every caller its own independent, never-mutated owner.
std::shared_ptr<const std::vector<Token>> TokenizeCached(const char* data, std::size_t len, CodeLang lang) {
    const CacheKey key{Fnv1a64(data, len), lang, SmatchetTheme::GetThemeRevision()};
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) {
        return it->second;
    }
    auto fresh = std::make_shared<std::vector<Token>>();
    Tokenize(data, len, lang, *fresh);
    auto inserted = g_cache.emplace(key, std::move(fresh));
    g_cacheInsertionOrder.push_back(key);
    ++g_cacheRebuildCount;
    while (g_cacheInsertionOrder.size() > kCacheCap) {
        const CacheKey evict = g_cacheInsertionOrder.front();
        g_cacheInsertionOrder.erase(g_cacheInsertionOrder.begin());
        if (!(evict == key)) {
            g_cache.erase(evict);
        }
    }
    return inserted.first->second;
}

// Render-side palette mapping

ImVec4 PaletteToThemeColor(TokenPalette p) {
    const SmatchetThemeSyntaxColors& s = SmatchetTheme::GetSyntaxColors();
    switch (p) {
    case TokenPalette::Keyword:
        return ImVec4(s.Keyword[0], s.Keyword[1], s.Keyword[2], s.Keyword[3]);
    case TokenPalette::Number:
        return ImVec4(s.Number[0], s.Number[1], s.Number[2], s.Number[3]);
    case TokenPalette::String:
        return ImVec4(s.String[0], s.String[1], s.String[2], s.String[3]);
    case TokenPalette::CharLiteral:
        return ImVec4(s.String[0], s.String[1], s.String[2], s.String[3]);
    case TokenPalette::Preprocessor:
        return ImVec4(s.Preprocessor[0], s.Preprocessor[1], s.Preprocessor[2], s.Preprocessor[3]);
    case TokenPalette::Comment:
        return ImVec4(s.Comment[0], s.Comment[1], s.Comment[2], s.Comment[3]);
    case TokenPalette::MultiLineComment:
        return ImVec4(s.Comment[0], s.Comment[1], s.Comment[2], s.Comment[3]);
    case TokenPalette::Identifier:
    case TokenPalette::KnownIdentifier:
    case TokenPalette::PreprocIdentifier:
        // Slice 6 — per-theme Identifier tint (was: ImGuiCol_Text default,
        // which blended into plain text). Tackles the user-reported "no
        // coloring" on callstack / annotate views where identifiers dominate.
        return ImVec4(s.Identifier[0], s.Identifier[1], s.Identifier[2], s.Identifier[3]);
    case TokenPalette::Punctuation:
    case TokenPalette::Default:
    default:
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    }
}

// Flat-orange fallback for Plain. Matches the pre-existing tint at
// MarkdownPreviewRender.cpp:856 (kept for visual continuity).
constexpr ImVec4 kPlainTint(0.95f, 0.85f, 0.6f, 1.0f);

} // namespace

// Public API

CodeLang FromTag(const std::string& tag) {
    const std::string lower = ToLower(tag);
    const auto it =
        std::find_if(std::begin(kAliases), std::end(kAliases), [&](const TagAlias& a) { return lower == a.tag; });
    return (it != std::end(kAliases)) ? it->lang : CodeLang::Plain;
}

CodeLang LangFromFilePath(const std::string& path) {
    // Strip directories — find the last '/' or '\' separator.
    const std::size_t lastSep = path.find_last_of("/\\");
    const std::string basename = (lastSep == std::string::npos) ? path : path.substr(lastSep + 1);
    // Strip everything before the last '.'. Files with no dot (`Makefile`,
    // `Dockerfile`) fall through to Plain.
    const std::size_t lastDot = basename.find_last_of('.');
    if (lastDot == std::string::npos || lastDot + 1 >= basename.size()) {
        return CodeLang::Plain;
    }
    const std::string ext = basename.substr(lastDot + 1);
    return FromTag(ext);
}

const char* CanonicalName(CodeLang lang) {
    switch (lang) {
    case CodeLang::Plain:
        return "Plain text";
    case CodeLang::CPlusPlus:
        return "C++";
    case CodeLang::C:
        return "C";
    case CodeLang::Lua:
        return "Lua";
    case CodeLang::Glsl:
        return "GLSL";
    case CodeLang::Hlsl:
        return "HLSL";
    case CodeLang::Sql:
        return "SQL";
    case CodeLang::AngelScript:
        return "AngelScript";
    case CodeLang::Markdown:
        return "Markdown";
    case CodeLang::Json:
        return "JSON";
    case CodeLang::Bash:
        return "Bash";
    case CodeLang::Python:
        return "Python";
    }
    return "Unknown";
}

void Tokenize(const char* utf8, std::size_t len, CodeLang lang, std::vector<Token>& outTokens) {
    if (utf8 == nullptr || len == 0) {
        return;
    }
    if (len > kInputCap) {
        len = kInputCap;
    }
    if (lang == CodeLang::Plain) {
        Token t;
        t.start = 0;
        t.length = static_cast<int>(len);
        t.palette = TokenPalette::Default;
        outTokens.push_back(t);
        return;
    }
    const TextEditor::LanguageDefinition* ld = LdForLang(lang);
    if (ld == nullptr) {
        Token t;
        t.start = 0;
        t.length = static_cast<int>(len);
        t.palette = TokenPalette::Default;
        outTokens.push_back(t);
        return;
    }
    TokenizeWithLd(utf8, utf8 + len, *ld, outTokens);
}

void DrawColoredCode(const char* utf8, CodeLang lang) {
    if (utf8 == nullptr || utf8[0] == '\0') {
        ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        return;
    }
    // CPlusPlus / C — delegate to the existing tokenizer to preserve byte-for-byte
    // existing behaviour on Smatchet's most-used path.
    if (lang == CodeLang::CPlusPlus || lang == CodeLang::C) {
        DrawColoredCppText(utf8);
        return;
    }
    if (lang == CodeLang::Plain) {
        ImGui::PushStyleColor(ImGuiCol_Text, kPlainTint);
        ImGui::TextUnformatted(utf8);
        ImGui::PopStyleColor();
        return;
    }

    const std::size_t len = std::strlen(utf8);
    const std::shared_ptr<const std::vector<Token>> tokens = TokenizeCached(utf8, len, lang);

    // Emit tokens with newline-aware ImGui::SameLine layout — same shape as
    // CppSyntaxHighlight's DrawColoredCppText so the rendered output looks
    // identical to the existing C++ path for non-cpp languages.
    bool firstOnLine = true;
    for (const Token& t : *tokens) {
        if (t.length <= 0) {
            continue;
        }
        const char* tok_begin = utf8 + t.start;
        const char* tok_end = tok_begin + t.length;
        // Split on \n so each line emits as its own ImGui row.
        const char* segment_begin = tok_begin;
        const char* p = tok_begin;
        while (p < tok_end) {
            if (*p == '\n') {
                if (p > segment_begin) {
                    if (!firstOnLine) {
                        ImGui::SameLine(0.f, 0.f);
                    }
                    const ImVec4 col = PaletteToThemeColor(t.palette);
                    ImGui::PushStyleColor(ImGuiCol_Text, col);
                    ImGui::TextUnformatted(segment_begin, p);
                    ImGui::PopStyleColor();
                    // No need to set firstOnLine=false here — the line break
                    // below resets to true regardless, making this write dead
                    // (cppcheck redundantAssignment, clang-analyzer dead-store).
                }
                // Newline — drop to next ImGui row.
                firstOnLine = true;
                segment_begin = p + 1;
            }
            ++p;
        }
        if (segment_begin < tok_end) {
            if (!firstOnLine) {
                ImGui::SameLine(0.f, 0.f);
            }
            const ImVec4 col = PaletteToThemeColor(t.palette);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(segment_begin, tok_end);
            ImGui::PopStyleColor();
            firstOnLine = false;
        }
    }
}

void DrawColoredCodeBlock(const char* utf8, CodeLang lang, const std::string& /*langTagOrigin*/) {
    // Slice 1: just calls DrawColoredCode. Slice 4 will overlay the language
    // badge + tooltip in the block's top-right corner using langTagOrigin.
    DrawColoredCode(utf8, lang);
}

std::size_t GetCacheRebuildCountForTest() {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    return g_cacheRebuildCount;
}

void ResetCacheForTest() {
    std::lock_guard<std::mutex> lk(g_cacheMutex);
    g_cache.clear();
    g_cacheInsertionOrder.clear();
    g_cacheRebuildCount = 0;
}

std::shared_ptr<const std::vector<Token>> TokenizeCachedForTest(const char* utf8, std::size_t len, CodeLang lang) {
    return TokenizeCached(utf8, len, lang);
}

} // namespace code_color
} // namespace smatchet
