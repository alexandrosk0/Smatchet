#include "Tracker/QueryFacetClausesPure.h"

#include "StringUtil.h"
#include "Tracker/GitHubQueryFromJql.h"
#include "Tracker/LinearQueryFromJql.h"
#include "Tracker/TrackerQuerySuggestCommon.h"

#include <algorithm>
#include <cctype>

namespace query_facets {

namespace {

enum class TokKind : unsigned char { Word, Quoted, Operator, Open, Close, Comma };

struct Tok {
    TokKind Kind = TokKind::Word;
    std::string Text; ///< a Quoted token holds its value without the quotes
    std::size_t At = 0;
    std::size_t Len = 0;
    bool OddEscape = false; ///< a Jql string with an escape other than \" \' and \\ (kept as typed in Text)

    std::size_t End() const { return At + Len; }
};

/// The tokens of a query, and what stops it from being read. `Broken`: a quote that is never closed or a
/// parenthesis with no partner, so not even a wrap around the filter would be well formed. `Foreign`: a byte
/// the GitHub translator skips without a word, so it reads the query differently from what was typed.
struct TokenList {
    std::vector<Tok> Tokens;
    bool Broken = false;
    bool Foreign = false;
};

bool IsSpaceByte(char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; }

bool IsQuoteByte(char c) { return c == '"' || c == '\''; }

bool IsOperatorByte(char c) { return c == '=' || c == '!' || c == '<' || c == '>' || c == '~'; }

/// GitHubQueryFromJql.cpp ScanBareword.
bool IsGitHubWordByte(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '-' || c == '.' || c == '@';
}

bool IsWordByte(char c, FacetDialect d) {
    if (d == FacetDialect::GitHubSubset) {
        return IsGitHubWordByte(c);
    }
    return !IsSpaceByte(c) && !IsQuoteByte(c) && !IsOperatorByte(c) && c != '(' && c != ')' && c != ',';
}

/// JQL reads a backslash escape inside a string; the two subset translators do not.
bool ScanQuoted(const std::string& q, std::size_t& pos, bool escapes, Tok& tok) {
    const char quote = q[pos];
    std::size_t i = pos + 1;
    while (i < q.size()) {
        if (escapes && q[i] == '\\' && i + 1 < q.size()) {
            const char next = q[i + 1];
            if (!IsQuoteByte(next) && next != '\\') {
                tok.Text.push_back('\\'); // "\n" and friends stay as typed
                tok.OddEscape = true;
            }
            tok.Text.push_back(next);
            i += 2;
            continue;
        }
        if (q[i] == quote) {
            tok.Kind = TokKind::Quoted;
            tok.Len = i + 1 - pos;
            pos = i + 1;
            return true;
        }
        tok.Text.push_back(q[i]);
        ++i;
    }
    return false;
}

/// An operator run: "=", "!=", and any other mix of operator bytes as one token. GitHub also reads ":" as an
/// operator, and splits its operators the way its tokenizer does.
void ScanOperator(const std::string& q, std::size_t& pos, FacetDialect d, Tok& tok) {
    std::size_t end = pos + 1;
    if (d == FacetDialect::GitHubSubset) {
        if (q[pos] == '!' && end < q.size() && q[end] == '=') {
            ++end;
        }
    } else {
        while (end < q.size() && IsOperatorByte(q[end])) {
            ++end;
        }
    }
    tok.Kind = TokKind::Operator;
    tok.Text = q.substr(pos, end - pos);
    tok.Len = end - pos;
    pos = end;
}

TokenList Tokenise(const std::string& q, FacetDialect d) {
    TokenList out;
    const bool github = d == FacetDialect::GitHubSubset;
    int depth = 0;
    std::size_t pos = 0;
    while (pos < q.size()) {
        const char c = q[pos];
        if (IsSpaceByte(c)) {
            ++pos;
            continue;
        }
        Tok tok;
        tok.At = pos;
        if (IsQuoteByte(c)) {
            if (!ScanQuoted(q, pos, d == FacetDialect::Jql, tok)) {
                out.Broken = true;
                return out;
            }
        } else if (c == '(' || c == ')' || c == ',') {
            tok.Kind = c == '(' ? TokKind::Open : (c == ')' ? TokKind::Close : TokKind::Comma);
            tok.Len = 1;
            ++pos;
            depth += c == '(' ? 1 : (c == ')' ? -1 : 0);
            out.Broken = out.Broken || depth < 0;
        } else if (IsOperatorByte(c) || (github && c == ':')) {
            ScanOperator(q, pos, d, tok);
        } else if (IsWordByte(c, d)) {
            std::size_t end = pos;
            while (end < q.size() && IsWordByte(q[end], d)) {
                ++end;
            }
            tok.Text = q.substr(pos, end - pos);
            tok.Len = end - pos;
            pos = end;
        } else {
            out.Foreign = true;
            ++pos;
            continue;
        }
        out.Tokens.push_back(std::move(tok));
    }
    out.Broken = out.Broken || depth != 0;
    return out;
}

bool IsKeyword(const Tok& t, const char* keyword) {
    return t.Kind == TokKind::Word && EqualsIgnoreCaseAscii(t.Text, keyword);
}

/// Index of the top-level "ORDER" token that starts the tail, or the token count when there is none.
std::size_t FindOrderByToken(const std::vector<Tok>& tokens) {
    int depth = 0;
    for (std::size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].Kind == TokKind::Open) {
            ++depth;
        } else if (tokens[i].Kind == TokKind::Close) {
            --depth;
        } else if (depth == 0 && IsKeyword(tokens[i], "order") && IsKeyword(tokens[i + 1], "by")) {
            return i;
        }
    }
    return tokens.size();
}

/// LinearQueryFromJql.cpp finds its tail as the text " order by ", so any other spacing is not a tail to it.
bool LinearReadsTail(const std::string& q, std::size_t at) {
    static const std::string kTail = "order by ";
    const bool spaceBefore = at == 0 || q[at - 1] == ' ';
    return spaceBefore && ToLowerAsciiCopy(q.substr(at, kTail.size())) == kTail;
}

/// One clause as typed, before it is judged to be a chip or not.
struct RawClause {
    std::string Field; ///< as typed
    FacetOp Op = FacetOp::Eq;
    std::vector<std::string> Values;
    bool CurrentUser = false;
    bool Group = false;            ///< a parenthesized group (Jql only)
    bool ChangesOnRewrite = false; ///< Jql: holds a value that would mean something else once written again
    std::size_t At = 0;
    std::size_t Len = 0;
};

/// True for a Jql value that a rewrite would change: a bare EMPTY or NULL (a keyword bare, a literal once
/// quoted), or a backslash the rewrite would double (an escape such as "\t", or any in a bare word).
bool IsJqlValueChangedByRewrite(const Tok& t) {
    if (t.Kind == TokKind::Quoted) {
        return t.OddEscape;
    }
    return EqualsIgnoreCaseAscii(t.Text, "empty") || EqualsIgnoreCaseAscii(t.Text, "null") ||
           t.Text.find('\\') != std::string::npos;
}

/// Reads clauses off the filter's tokens (those before the ORDER BY tail).
class ClauseReader {
  public:
    ClauseReader(const std::string& query, FacetDialect d, const std::vector<Tok>& tokens, std::size_t count)
        : query_(query), dialect_(d), tokens_(tokens), count_(count) {}

    /// False when the filter is not a list of clauses joined by AND.
    bool ReadAll(std::vector<RawClause>& out) {
        while (next_ < count_) {
            RawClause clause;
            if (!ReadOperand(clause)) {
                return false;
            }
            out.push_back(std::move(clause));
            if (next_ >= count_) {
                return true;
            }
            if (!ReadAnd()) {
                return false;
            }
        }
        return out.empty();
    }

  private:
    bool Has(std::size_t ahead, TokKind kind) const {
        return next_ + ahead < count_ && tokens_[next_ + ahead].Kind == kind;
    }

    /// Consumes the AND between two clauses; something must follow it.
    bool ReadAnd() {
        const Tok& t = tokens_[next_];
        if (!IsKeyword(t, "and")) {
            return false;
        }
        // The Linear translator splits on the text " and ": a tab or a line break beside it hides the split.
        if (dialect_ == FacetDialect::LinearSubset &&
            (t.At == 0 || query_[t.At - 1] != ' ' || t.End() >= query_.size() || query_[t.End()] != ' ')) {
            return false;
        }
        ++next_;
        return next_ < count_;
    }

    bool ReadOperand(RawClause& out) {
        const Tok& first = tokens_[next_];
        out.At = first.At;
        if (first.Kind == TokKind::Open) {
            return dialect_ == FacetDialect::Jql && ReadGroup(out);
        }
        // Only JQL lets a field name be quoted.
        const bool fieldToken =
            first.Kind == TokKind::Word || (first.Kind == TokKind::Quoted && dialect_ == FacetDialect::Jql);
        if (!fieldToken || IsReservedWord(first)) {
            return false;
        }
        out.Field = first.Text;
        ++next_;
        if (!ReadOperator(out)) {
            return false;
        }
        const bool isList = out.Op == FacetOp::In || out.Op == FacetOp::NotIn;
        if (isList ? !ReadList(out) : !ReadValue(out)) {
            return false;
        }
        out.Len = tokens_[next_ - 1].End() - out.At;
        return true;
    }

    static bool IsReservedWord(const Tok& t) {
        return IsKeyword(t, "and") || IsKeyword(t, "or") || IsKeyword(t, "not") || IsKeyword(t, "order");
    }

    bool ReadGroup(RawClause& out) {
        int depth = 0;
        while (next_ < count_) {
            const TokKind kind = tokens_[next_].Kind;
            depth += kind == TokKind::Open ? 1 : (kind == TokKind::Close ? -1 : 0);
            ++next_;
            if (depth == 0) {
                out.Group = true;
                out.Len = tokens_[next_ - 1].End() - out.At;
                return true;
            }
        }
        return false;
    }

    bool ReadOperator(RawClause& out) {
        if (next_ >= count_) {
            return false;
        }
        const Tok& t = tokens_[next_];
        if (t.Kind == TokKind::Operator && (t.Text == "=" || t.Text == "!=" || t.Text == ":")) {
            out.Op = t.Text == "!=" ? FacetOp::NotEq : FacetOp::Eq;
        } else if (IsKeyword(t, "in")) {
            out.Op = FacetOp::In;
        } else if (IsKeyword(t, "not") && next_ + 1 < count_ && IsKeyword(tokens_[next_ + 1], "in")) {
            out.Op = FacetOp::NotIn;
            ++next_;
        } else {
            return false;
        }
        ++next_;
        return true;
    }

    /// One value or currentUser().
    bool ReadValue(RawClause& out) {
        if (next_ >= count_) {
            return false;
        }
        const Tok& t = tokens_[next_];
        if (IsKeyword(t, "currentUser") && Has(1, TokKind::Open) && Has(2, TokKind::Close)) {
            // Linear compares the value's text with "currentuser()": a space inside makes it a person's name.
            const std::size_t end = tokens_[next_ + 2].End();
            if (dialect_ != FacetDialect::LinearSubset || end - t.At == t.Len + 2) {
                out.CurrentUser = true;
            } else {
                out.Values.push_back(query_.substr(t.At, end - t.At));
            }
            next_ += 3;
            return true;
        }
        if (t.Kind == TokKind::Quoted) {
            if (dialect_ == FacetDialect::LinearSubset && EqualsIgnoreCaseAscii(t.Text, "currentUser()")) {
                out.CurrentUser = true; // Linear strips the quotes before it looks
            } else {
                out.Values.push_back(t.Text);
            }
        } else if (t.Kind == TokKind::Word && !IsReservedWord(t)) {
            out.Values.push_back(t.Text);
        } else {
            return false;
        }
        out.ChangesOnRewrite = out.ChangesOnRewrite || (dialect_ == FacetDialect::Jql && IsJqlValueChangedByRewrite(t));
        ++next_;
        return true;
    }

    bool ReadList(RawClause& out) {
        if (!Has(0, TokKind::Open)) {
            return false;
        }
        ++next_;
        while (ReadValue(out)) {
            if (Has(0, TokKind::Close)) {
                ++next_;
                return true;
            }
            if (!Has(0, TokKind::Comma)) {
                return false;
            }
            ++next_;
        }
        return false;
    }

    const std::string& query_;
    const FacetDialect dialect_;
    const std::vector<Tok>& tokens_;
    const std::size_t count_;
    std::size_t next_ = 0;
};

/// The field's name as the dialect's translator reads it: lower case, aliases resolved.
std::string FoldFieldName(FacetDialect d, const std::string& typedField) {
    std::string name = ToLowerAsciiCopy(typedField);
    if (d == FacetDialect::Jql && name == "type") {
        return "issuetype";
    }
    if (d != FacetDialect::Jql && name == "label") {
        return "labels";
    }
    if (d == FacetDialect::GitHubSubset && name == "author") {
        return "reporter";
    }
    if (d == FacetDialect::LinearSubset && name == "state") {
        return "status";
    }
    if (d == FacetDialect::LinearSubset && name == "creator") {
        return "reporter";
    }
    return name;
}

/// GitHub's native "is:open" / "is:closed" say what `status = "Open"` / `"Closed"` translate to.
void ReadGitHubStateToken(RawClause& c) {
    if (!EqualsIgnoreCaseAscii(c.Field, "is") || c.Values.size() != 1) {
        return;
    }
    const bool open = EqualsIgnoreCaseAscii(c.Values.front(), "open");
    if (!open && !EqualsIgnoreCaseAscii(c.Values.front(), "closed")) {
        return;
    }
    c.Field = "status";
    c.Values.front() = open ? "Open" : "Closed";
}

bool Contains(const std::vector<std::string>& list, const std::string& value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

bool IsUserFacet(const std::string& fieldId) { return fieldId == "assignee" || fieldId == "reporter"; }

std::size_t ValueCount(const std::vector<std::string>& values, bool currentUser) {
    return values.size() + (currentUser ? 1 : 0);
}

bool IsListOp(FacetOp op) { return op == FacetOp::In || op == FacetOp::NotIn; }

/// True when a chip may stand for this facet clause: the tracker applies it as written, and a rewrite keeps its
/// meaning. Jira applies all JQL, so only the rewrite test counts there. A subset dialect must both have a
/// picker that could have written the clause and translate it whole.
bool IsChipClause(const std::string& query, FacetDialect d, const std::string& fieldId, const RawClause& c) {
    if (d == FacetDialect::Jql) {
        return !c.ChangesOnRewrite;
    }
    if (c.CurrentUser && !IsUserFacet(fieldId)) {
        return false;
    }
    if (!DialectSupports(d, fieldId, c.Op, ValueCount(c.Values, c.CurrentUser))) {
        return false;
    }
    const bool inList = IsListOp(c.Op);
    const bool writable = std::all_of(c.Values.begin(), c.Values.end(),
                                      [d, inList](const std::string& v) { return ValueSupported(d, v, inList); });
    return writable && IgnoredClauseNote(query.substr(c.At, c.Len), d).empty();
}

/// Linear writes one filter key per field and operator, so a second clause on a field replaces the first.
bool HasRepeatedField(FacetDialect d, const std::vector<RawClause>& clauses) {
    std::vector<std::string> seen;
    for (const RawClause& c : clauses) {
        const std::string name = FoldFieldName(d, c.Field);
        if (Contains(seen, name)) {
            return true;
        }
        seen.push_back(name);
    }
    return false;
}

void SortClauses(const std::string& query, FacetDialect d, const std::vector<std::string>& facetFieldIds,
                 std::vector<RawClause>& raw, FacetParse& out) {
    for (RawClause& c : raw) {
        if (d == FacetDialect::GitHubSubset) {
            ReadGitHubStateToken(c);
        }
        const std::string id = c.Group ? std::string() : CanonicalFacetField(d, c.Field);
        const bool facet = !id.empty() && Contains(facetFieldIds, id);
        if (facet && IsChipClause(query, d, id, c)) {
            FacetClause clause;
            clause.FieldId = id;
            clause.Op = c.Op;
            clause.Values = std::move(c.Values);
            clause.CurrentUser = c.CurrentUser;
            clause.At = c.At;
            clause.Len = c.Len;
            out.Clauses.push_back(std::move(clause));
            continue;
        }
        out.OtherSpans.emplace_back(c.At, c.Len);
        if (facet && d != FacetDialect::Jql && !Contains(out.LockedFieldIds, id)) {
            out.LockedFieldIds.push_back(id);
        }
    }
}

std::string QuoteValue(FacetDialect d, const std::string& value) {
    if (d == FacetDialect::Jql) {
        return tracker_query_suggest::QueryQuotedValue(value);
    }
    return "\"" + value + "\"";
}

/// The clause's text, or empty when the dialect cannot write it or its translator would not apply it whole. A
/// single-value operator with several values is written as the list operator of the same sense.
std::string RenderClause(FacetDialect d, const FacetClause& clause) {
    const std::size_t count = ValueCount(clause.Values, clause.CurrentUser);
    if (count == 0) {
        return std::string();
    }
    FacetOp op = clause.Op;
    if (count > 1 && !IsListOp(op)) {
        op = op == FacetOp::Eq ? FacetOp::In : FacetOp::NotIn;
    }
    if (!DialectSupports(d, clause.FieldId, op, count) ||
        (clause.CurrentUser && d != FacetDialect::Jql && !IsUserFacet(clause.FieldId))) {
        return std::string();
    }
    const bool inList = IsListOp(op);
    std::string values = clause.CurrentUser ? "currentUser()" : "";
    for (const std::string& v : clause.Values) {
        if (!ValueSupported(d, v, inList)) {
            return std::string();
        }
        if (!values.empty()) {
            values += ", ";
        }
        values += QuoteValue(d, v);
    }
    static const char* const kOps[] = {" = ", " != ", " in (", " not in ("};
    const std::string text = clause.FieldId + kOps[static_cast<int>(op)] + values + (inList ? ")" : "");
    // A value outside the field's own names (a GitHub status "Blocked") is writable but ignored: refuse it.
    return IgnoredClauseNote(text, d).empty() ? text : std::string();
}

/// End of the filter part of `query`: the text before the tail, without its trailing white space.
std::size_t FilterEnd(const std::string& query, std::size_t orderByAt) {
    std::size_t end = orderByAt == std::string::npos ? query.size() : orderByAt;
    while (end > 0 && IsSpaceByte(query[end - 1])) {
        --end;
    }
    return end;
}

std::size_t SkipSpacesForward(const std::string& q, std::size_t pos) {
    while (pos < q.size() && IsSpaceByte(q[pos])) {
        ++pos;
    }
    return pos;
}

/// True when the three bytes at `pos` are the word AND.
bool IsAndAt(const std::string& q, std::size_t pos) {
    if (pos + 3 > q.size() || !EqualsIgnoreCaseAscii(q.substr(pos, 3), "and")) {
        return false;
    }
    const auto boundary = [](char c) { return IsSpaceByte(c) || IsQuoteByte(c) || c == '(' || c == ')'; };
    return (pos == 0 || boundary(q[pos - 1])) && (pos + 3 == q.size() || boundary(q[pos + 3]));
}

} // namespace

FacetParse ParseFacetClauses(const std::string& query, FacetDialect d, const std::vector<std::string>& facetFieldIds) {
    FacetParse out;
    const TokenList list = Tokenise(query, d);
    const std::size_t tail = FindOrderByToken(list.Tokens);
    if (tail < list.Tokens.size()) {
        out.OrderByAt = list.Tokens[tail].At;
    }
    if (list.Broken || list.Foreign) {
        return out;
    }
    if (d == FacetDialect::LinearSubset && out.OrderByAt != std::string::npos &&
        !LinearReadsTail(query, out.OrderByAt)) {
        return out;
    }
    std::vector<RawClause> raw;
    ClauseReader reader(query, d, list.Tokens, tail);
    if (!reader.ReadAll(raw)) {
        return out;
    }
    if (d == FacetDialect::LinearSubset && HasRepeatedField(d, raw)) {
        return out;
    }
    out.Chippable = true;
    SortClauses(query, d, facetFieldIds, raw, out);
    return out;
}

std::string AddFacetClause(const std::string& query, FacetDialect d, const FacetClause& clause) {
    const std::string text = RenderClause(d, clause);
    if (text.empty() || Tokenise(query, d).Broken) {
        return query;
    }
    const FacetParse parsed = ParseFacetClauses(query, d, FacetFieldIds(d));
    if (!parsed.Chippable && d != FacetDialect::Jql) {
        return query;
    }
    const std::size_t end = FilterEnd(query, parsed.OrderByAt);
    const std::string rest = query.substr(end);
    if (SkipSpacesForward(query, 0) >= end) { // no filter: the clause stands alone, ahead of any tail
        if (parsed.OrderByAt == std::string::npos) {
            return text;
        }
        return text + " " + query.substr(parsed.OrderByAt);
    }
    std::string filter = query.substr(0, end);
    if (!parsed.Chippable) {
        filter = "(" + filter + ")";
    }
    return filter + " AND " + text + rest;
}

std::string ReplaceFacetClause(const std::string& query, FacetDialect d, std::size_t at, std::size_t len,
                               const FacetClause& clause) {
    const std::string text = RenderClause(d, clause);
    if (text.empty() || len == 0 || at > query.size() || len > query.size() - at) {
        return query;
    }
    return query.substr(0, at) + text + query.substr(at + len);
}

std::string RemoveFacetClause(const std::string& query, std::size_t at, std::size_t len) {
    if (len == 0 || at > query.size() || len > query.size() - at) {
        return query;
    }
    const std::size_t before = FilterEnd(query, at); // end of whatever precedes the clause
    if (before >= 3 && IsAndAt(query, before - 3)) {
        return query.substr(0, FilterEnd(query, before - 3)) + query.substr(at + len);
    }
    std::size_t after = SkipSpacesForward(query, at + len);
    if (IsAndAt(query, after)) {
        after = SkipSpacesForward(query, after + 3);
    }
    const std::string out = query.substr(0, at) + query.substr(after);
    return SkipSpacesForward(out, 0) >= out.size() ? std::string() : out;
}

bool DialectSupports(FacetDialect d, const std::string& fieldId, FacetOp op, std::size_t valueCount) {
    if (valueCount == 0 || !Contains(FacetFieldIds(d), fieldId)) {
        return false;
    }
    if (!IsListOp(op) && valueCount != 1) {
        return false;
    }
    switch (d) {
    case FacetDialect::Jql:
        return true;
    case FacetDialect::GitHubSubset:
        return op == FacetOp::Eq;
    case FacetDialect::LinearSubset:
        if (op == FacetOp::In) {
            return fieldId == "status" || fieldId == "labels" || fieldId == "priority";
        }
        return op == FacetOp::Eq;
    }
    return false;
}

std::string CanonicalFacetField(FacetDialect d, const std::string& typedField) {
    const std::string name = FoldFieldName(d, typedField);
    const std::vector<std::string>& ids = FacetFieldIds(d);
    const auto it = std::find_if(ids.begin(), ids.end(),
                                 [&name](const std::string& id) { return EqualsIgnoreCaseAscii(id, name); });
    return it == ids.end() ? std::string() : *it;
}

bool FieldRepeats(FacetDialect d, const std::string& fieldId) {
    return d == FacetDialect::GitHubSubset && fieldId == "labels";
}

bool ValueSupported(FacetDialect d, const std::string& value, bool inList) {
    if (d == FacetDialect::Jql) {
        return true;
    }
    if (value.empty() || value.find('"') != std::string::npos) {
        return false;
    }
    return !(d == FacetDialect::LinearSubset && inList && value.find(',') != std::string::npos);
}

const std::vector<std::string>& FacetFieldIds(FacetDialect d) {
    static const std::vector<std::string> kJql = {"status",    "assignee",   "labels",     "priority", "issuetype",
                                                  "component", "fixVersion", "resolution", "reporter"};
    static const std::vector<std::string> kGitHub = {"status", "assignee", "labels", "reporter"};
    static const std::vector<std::string> kLinear = {"status",  "assignee", "labels",  "priority",
                                                     "project", "team",     "reporter"};
    switch (d) {
    case FacetDialect::GitHubSubset:
        return kGitHub;
    case FacetDialect::LinearSubset:
        return kLinear;
    case FacetDialect::Jql:
        break;
    }
    return kJql;
}

std::string IgnoredClauseNote(const std::string& query, FacetDialect d) {
    if (d == FacetDialect::Jql) {
        return std::string();
    }
    const TokenList list = Tokenise(query, d);
    const std::size_t tail = FindOrderByToken(list.Tokens);
    std::string filter = query;
    if (tail < list.Tokens.size()) {
        const std::size_t at = list.Tokens[tail].At;
        // A tail the Linear translator would not strip itself stays in: it reads it as part of the filter.
        if (d == FacetDialect::GitHubSubset || LinearReadsTail(query, at)) {
            filter = query.substr(0, at);
        }
    }
    if (d == FacetDialect::GitHubSubset) {
        const smatchet::github::JqlToGitHubResult r =
            smatchet::github::TranslateJqlToGitHubSearch(filter, std::string(), std::string());
        return r.Ok ? r.Warning : r.Error;
    }
    const smatchet::linear::JqlToLinearResult r = smatchet::linear::TranslateJqlToLinearFilter(filter);
    return r.Ok ? r.Warning : r.Error;
}

} // namespace query_facets
