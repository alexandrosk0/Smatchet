#ifndef CPP_SYNTAX_LEX_H
#define CPP_SYNTAX_LEX_H

#include <cstddef>
#include <string>
#include <vector>

// Pure (ImGui-free) lexers extracted from CppSyntaxHighlight.cpp's draw
// helpers. Each lexer turns a single line of text into an ordered list of
// coloured spans. A span carries an offset plus length into the original
// buffer and a semantic kind; the draw side maps each kind onto a theme
// colour and emits it. The lexers allocate nothing into ImGui and have no UI
// side effects, so they are bucket-A unit-testable: feed a line, assert on
// the returned span list.

// Semantic token kind for a C/C++-like source line. The draw layer maps each
// to a theme syntax colour (or, for Default, to the current ImGui text colour).
enum class CppLexKind {
    Default,      // punctuation / unclassified single char
    Comment,      // // ... or /* ... */
    String,       // "..." or '...'
    Preprocessor, // # directive run
    Number,       // numeric literal
    Keyword,      // reserved word
    Identifier    // non-keyword identifier
};

// Semantic token kind for a debugger/Unreal callstack frame line. The draw
// layer maps each to a theme colour; Default is the current ImGui text colour.
enum class CallstackLexKind {
    Default,  // module, punctuation (! :: (...) [ ] whitespace), trailing text
    Class,    // class identifier in the Module!Class::Class::method chain
    Method,   // final segment before '(' — the method name
    Path,     // directory portion inside [ ... ] (dimmed)
    Filename, // filename + extension inside [ ... ] (bright)
    LineNum   // ':lineNumber' inside [ ... ]
};

struct CppLexToken {
    std::size_t offset;
    std::size_t length;
    CppLexKind kind;
};

struct CallstackLexToken {
    std::size_t offset;
    std::size_t length;
    CallstackLexKind kind;
};

// True iff `w` is a recognised C/C++ keyword / built-in type alias.
bool CppLexIsKeyword(const std::string& w);

// True iff a callstack line matches the canonical frame shape — has a `!`
// separating module from symbol AND a `(` or `[` somewhere after the `!`.
bool CppLexLooksLikeCallstack(const char* line, std::size_t n);

// Lex one C/C++-like source line into ordered coloured spans. `line` need not
// be NUL-terminated within [line, line+n). Returns an empty vector for n == 0.
std::vector<CppLexToken> CppLexLine(const char* line, std::size_t n);

// Lex one canonical callstack frame line into ordered coloured spans. The
// caller is expected to have already confirmed CppLexLooksLikeCallstack; this
// function does not re-check and assumes the frame shape. Returns an empty
// vector for n == 0.
std::vector<CallstackLexToken> CppLexCallstackLine(const char* line, std::size_t n);

#endif
