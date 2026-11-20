#include "JqlQueryValidatePure.h"

#include "StringUtil.h"

#include <cstddef>
#include <string>
#include <vector>

namespace JqlQueryValidate {

namespace {

using TrackerParseExplain::ParseMessage;
using TrackerParseExplain::ParseMessageId;

/// Queries longer than this are not scanned: the box takes a few hundred bytes of typing, and a pasted
/// novel should not cost a frame. The tracker still judges it.
constexpr std::size_t kMaxScanBytes = 8192;

enum class TokenKind : unsigned char { Word, Operator, Open, Close, Comma, String };

struct Token {
    TokenKind Kind = TokenKind::Word;
    std::string Text; ///< as typed (strings include no quotes)
    std::size_t At = 0;
    std::size_t Len = 0;
};

using TrackerParseExplain::MakeParseMessage;

bool IsOperatorChar(char c) { return c == '=' || c == '!' || c == '<' || c == '>' || c == '~'; }

/// A word runs until something that structures the query. Everything else belongs to it: "cf[10010]", an e-mail
/// address, "status:done". An apostrophe is a quote only in JQL; in the field:value dialects it is an
/// ordinary character ("can't login").
bool IsWordChar(char c, bool apostropheQuotes) {
    const bool space = c == ' ' || c == '\t' || c == '\n' || c == '\r';
    const bool structure = c == '(' || c == ')' || c == ',' || c == '"' || (c == '\'' && apostropheQuotes);
    return !space && !structure && !IsOperatorChar(c);
}

/// A quote that is not closed is reported and stops the scan.
bool ScanString(const std::string& s, std::size_t& pos, Token& tok, ParseMessage& error) {
    const char quote = s[pos];
    const std::size_t start = pos++;
    while (pos < s.size()) {
        if (s[pos] == '\\' && pos + 1 < s.size()) {
            pos += 2; // an escaped character, including an escaped quote
            continue;
        }
        if (s[pos] == quote) {
            tok.Kind = TokenKind::String;
            tok.Text = s.substr(start + 1, pos - start - 1);
            tok.At = start;
            tok.Len = pos - start + 1;
            ++pos;
            return true;
        }
        ++pos;
    }
    error = MakeParseMessage(ParseMessageId::QueryUnclosedQuote, std::string(1, quote), start, s.size() - start);
    return false;
}

bool Tokenise(const std::string& s, bool apostropheQuotes, std::vector<Token>& out, ParseMessage& error) {
    std::size_t pos = 0;
    while (pos < s.size()) {
        const char c = s[pos];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++pos;
            continue;
        }
        Token tok;
        tok.At = pos;
        if (c == '"' || (c == '\'' && apostropheQuotes)) {
            if (!ScanString(s, pos, tok, error)) {
                return false;
            }
        } else if (c == '(') {
            tok.Kind = TokenKind::Open;
            tok.Len = 1;
            ++pos;
        } else if (c == ')') {
            tok.Kind = TokenKind::Close;
            tok.Len = 1;
            ++pos;
        } else if (c == ',') {
            tok.Kind = TokenKind::Comma;
            tok.Len = 1;
            ++pos;
        } else if (IsOperatorChar(c)) {
            const std::size_t start = pos;
            while (pos < s.size() && IsOperatorChar(s[pos])) {
                ++pos;
            }
            tok.Kind = TokenKind::Operator;
            tok.Text = s.substr(start, pos - start);
            tok.Len = pos - start;
        } else {
            const std::size_t start = pos;
            while (pos < s.size() && IsWordChar(s[pos], apostropheQuotes)) {
                ++pos;
            }
            tok.Kind = TokenKind::Word;
            tok.Text = s.substr(start, pos - start);
            tok.Len = pos - start;
        }
        out.push_back(tok);
    }
    return true;
}

bool IsKeywordWord(const Token& t, const char* keyword) {
    return t.Kind == TokenKind::Word && EqualsIgnoreCaseAscii(t.Text, keyword);
}

/// Words that need something after them.
bool IsJoiningKeyword(const Token& t) {
    static const char* const kKeywords[] = {"and", "or", "not",   "in",     "was",    "by", "from",
                                            "to",  "on", "after", "before", "during", "is", "order"};
    if (t.Kind != TokenKind::Word) {
        return false;
    }
    for (const char* k : kKeywords) {
        if (EqualsIgnoreCaseAscii(t.Text, k)) {
            return true; // ("changed" is not listed: "status changed" is a complete clause)
        }
    }
    return false;
}

ParseMessage CheckParens(const std::vector<Token>& tokens) {
    std::vector<std::size_t> open;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].Kind == TokenKind::Open) {
            open.push_back(i);
        } else if (tokens[i].Kind == TokenKind::Close) {
            if (open.empty()) {
                return MakeParseMessage(ParseMessageId::QueryUnbalancedClose, std::string(), tokens[i].At,
                                        tokens[i].Len);
            }
            open.pop_back();
        }
    }
    if (!open.empty()) {
        const Token& t = tokens[open.back()];
        return MakeParseMessage(ParseMessageId::QueryUnbalancedOpen, std::string(), t.At, t.Len);
    }
    return ParseMessage();
}

/// The query must not end on an operator or a keyword that needs a right-hand side.
ParseMessage CheckDangling(const std::vector<Token>& tokens, bool jql) {
    if (tokens.empty()) {
        return ParseMessage();
    }
    const Token& last = tokens.back();
    if (jql && IsKeywordWord(last, "by") && tokens.size() >= 2 && IsKeywordWord(tokens[tokens.size() - 2], "order")) {
        return MakeParseMessage(ParseMessageId::QueryDanglingOperator, "ORDER BY", tokens[tokens.size() - 2].At,
                                last.At + last.Len - tokens[tokens.size() - 2].At);
    }
    // A trailing "!" or "~" is plain text in a field:value search ("is:open crash!"); only a comparison that
    // needs a right-hand side leaves the query unfinished there.
    const bool plainPunctuation = !jql && (last.Text == "!" || last.Text == "~");
    if ((last.Kind == TokenKind::Operator && !plainPunctuation) || last.Kind == TokenKind::Comma) {
        return MakeParseMessage(ParseMessageId::QueryDanglingOperator, last.Text.empty() ? "," : last.Text, last.At,
                                last.Len);
    }
    // Only JQL gives these words a meaning. In a field:value search ("is:open sign in") they are plain text.
    if (jql && IsJoiningKeyword(last)) {
        // "order by" is only complete with a field; "ORDER" alone is caught here as well.
        return MakeParseMessage(ParseMessageId::QueryDanglingOperator, ToUpperAsciiCopy(last.Text), last.At, last.Len);
    }
    return ParseMessage();
}

bool FieldKnown(const std::string& word, const std::vector<TrackerField>& fields) {
    for (const TrackerField& f : fields) {
        if (EqualsIgnoreCaseAscii(f.Id, word) || EqualsIgnoreCaseAscii(f.Name, word)) {
            return true;
        }
    }
    // "cf[10010]" style custom-field references and a few JQL system names the catalog may not list.
    static const char* const kBuiltins[] = {
        "key",         "issuekey", "id",       "text",     "project",    "summary",   "status",
        "created",     "updated",  "resolved", "assignee", "reporter",   "labels",    "priority",
        "issuetype",   "type",     "parent",   "sprint",   "fixversion", "component", "resolution",
        "description", "comment",  "duedate",  "creator",  "watcher",    "voter",     "worklogdate"};
    for (const char* b : kBuiltins) {
        if (EqualsIgnoreCaseAscii(word, b)) {
            return true;
        }
    }
    return word.size() > 3 && (word[0] == 'c' || word[0] == 'C') && (word[1] == 'f' || word[1] == 'F') &&
           word[2] == '[';
}

bool IsClauseKeyword(const Token& t) {
    static const char* const kKeywords[] = {"and",   "or",   "not",   "order",  "by",     "asc",     "desc",
                                            "empty", "null", "is",    "in",     "was",    "changed", "from",
                                            "to",    "on",   "after", "before", "during", "true",    "false"};
    for (const char* k : kKeywords) {
        if (EqualsIgnoreCaseAscii(t.Text, k)) {
            return true;
        }
    }
    return false;
}

/// A word directly followed by an operator or "in"/"is"/"was"/"changed" is a field name. Only those are
/// checked: a bare word is a value or a function and the tracker knows better.
ParseMessage CheckFields(const std::vector<Token>& tokens, const std::vector<TrackerField>& fields) {
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        const Token& t = tokens[i];
        const Token& next = tokens[i + 1];
        if (t.Kind != TokenKind::Word || IsClauseKeyword(t) || t.Text.find(':') != std::string::npos) {
            continue;
        }
        const bool before = i == 0 || tokens[i - 1].Kind == TokenKind::Open || IsKeywordWord(tokens[i - 1], "and") ||
                            IsKeywordWord(tokens[i - 1], "or") || IsKeywordWord(tokens[i - 1], "not");
        const bool leadsClause =
            next.Kind == TokenKind::Operator || IsKeywordWord(next, "in") || IsKeywordWord(next, "is") ||
            IsKeywordWord(next, "was") || IsKeywordWord(next, "changed") ||
            (IsKeywordWord(next, "not") && i + 2 < tokens.size() && IsKeywordWord(tokens[i + 2], "in"));
        if (before && leadsClause && !FieldKnown(t.Text, fields)) {
            return MakeParseMessage(ParseMessageId::QueryUnknownField, t.Text, t.At, t.Len);
        }
    }
    return ParseMessage();
}

} // namespace

bool IsBlockingQueryError(const ParseMessage& msg) {
    switch (msg.Id) {
    case ParseMessageId::QueryUnbalancedOpen:
    case ParseMessageId::QueryUnbalancedClose:
    case ParseMessageId::QueryUnclosedQuote:
    case ParseMessageId::QueryDanglingOperator:
        return true;
    default:
        return false;
    }
}

ParseMessage ValidateQuery(const std::string& text, QueryDialect dialect, const std::vector<TrackerField>* fields) {
    if (text.size() > kMaxScanBytes) {
        return ParseMessage();
    }
    std::vector<Token> tokens;
    ParseMessage error;
    const bool jql = dialect == QueryDialect::Jql;
    if (!Tokenise(text, jql, tokens, error)) {
        return error;
    }
    ParseMessage parens = CheckParens(tokens);
    if (parens.IsError()) {
        return parens;
    }
    ParseMessage dangling = CheckDangling(tokens, jql);
    if (dangling.IsError()) {
        return dangling;
    }
    if (jql && fields != nullptr && !fields->empty()) {
        return CheckFields(tokens, *fields);
    }
    return ParseMessage();
}

} // namespace JqlQueryValidate
