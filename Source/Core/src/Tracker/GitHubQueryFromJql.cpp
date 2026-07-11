#include "GitHubQueryFromJql.h"

#include "GitHubClientHelpers.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

// Pure helper. Tokenizer + small pattern matcher; no regex, no I/O,
// no globals. C++14 compatible.

namespace smatchet {
namespace github {

namespace {

enum class TokKind {
    Word,   // bareword (identifier or keyword)
    String, // quoted string (quotes stripped)
    Op,     // =, !=, ~
    LParen,
    RParen,
    Comma,
    End,
};

struct Tok {
    TokKind Kind = TokKind::End;
    std::string Text; // for Word, String, Op, Comma; empty for parens
};

std::string ToLower(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

bool EqIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// Scan a quoted string starting just past the opening quote at `i` (which is
// advanced past the closing quote on success). Returns false + sets error on
// an unterminated string. No escape handling — JQL strings in our view editor
// don't use backslash escapes.
bool ScanQuotedString(const std::string& src, std::size_t& i, char quote, std::string& outStr, std::string& error) {
    bool closed = false;
    while (i < src.size()) {
        const char ch = src[i];
        if (ch == quote) {
            closed = true;
            ++i;
            break;
        }
        outStr.push_back(ch);
        ++i;
    }
    if (!closed) {
        error = "Unterminated quoted string in JQL";
        return false;
    }
    return true;
}

// Scan a bareword (identifier chars + dots/dashes/at) starting at `i`, advancing
// `i` past the word. Returns the matched text. `@` is preserved so GitHub-native
// user tokens like `@me` survive tokenization intact (dropping it collapsed
// `@me` to a literal `me`, mis-scoping the qualifier).
std::string ScanBareword(const std::string& src, std::size_t& i) {
    const std::size_t start = i;
    while (i < src.size()) {
        const unsigned char cc = static_cast<unsigned char>(src[i]);
        if (std::isalnum(cc) == 0 && cc != '_' && cc != '-' && cc != '.' && cc != '@') {
            break;
        }
        ++i;
    }
    return src.substr(start, i - start);
}

// Tokenize the input. Returns false + sets error on unterminated quoted string.
bool Tokenize(const std::string& src, std::vector<Tok>& out, std::string& error) {
    std::size_t i = 0;
    while (i < src.size()) {
        const unsigned char c = static_cast<unsigned char>(src[i]);
        if (std::isspace(c) != 0) {
            ++i;
            continue;
        }
        if (c == '(') {
            Tok t;
            t.Kind = TokKind::LParen;
            out.push_back(t);
            ++i;
            continue;
        }
        if (c == ')') {
            Tok t;
            t.Kind = TokKind::RParen;
            out.push_back(t);
            ++i;
            continue;
        }
        if (c == ',') {
            Tok t;
            t.Kind = TokKind::Comma;
            t.Text = ",";
            out.push_back(t);
            ++i;
            continue;
        }
        if (c == '"' || c == '\'') {
            const char quote = static_cast<char>(c);
            ++i;
            std::string s;
            if (!ScanQuotedString(src, i, quote, s, error)) {
                return false;
            }
            Tok t;
            t.Kind = TokKind::String;
            t.Text = s;
            out.push_back(t);
            continue;
        }
        if (c == '!' && i + 1 < src.size() && src[i + 1] == '=') {
            Tok t;
            t.Kind = TokKind::Op;
            t.Text = "!=";
            out.push_back(t);
            i += 2;
            continue;
        }
        if (c == '=' || c == '~' || c == ':') {
            // Accept `:` as an operator so the JQL-shorthand `type:pr`
            // tokenizes as Word("type") + Op(":") + Word("pr"). Only the
            // `type` field handler treats `:` as equivalent to `=`; all other
            // handlers reject `:` as an unsupported operator.
            Tok t;
            t.Kind = TokKind::Op;
            t.Text = std::string(1, static_cast<char>(c));
            out.push_back(t);
            ++i;
            continue;
        }
        // Bareword: identifier chars + dots/dashes (for fields), plus `@` so a
        // leading-`@` value such as `@me` starts (and stays inside) one token.
        if (std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.' || c == '@') {
            Tok t;
            t.Kind = TokKind::Word;
            t.Text = ScanBareword(src, i);
            out.push_back(t);
            continue;
        }
        // Unknown byte — skip; tokenizer is permissive on stray punctuation.
        ++i;
    }
    Tok end;
    end.Kind = TokKind::End;
    out.push_back(end);
    return true;
}

// Detect `currentUser()` starting at tokens[i] (Word "currentUser" then LParen
// then RParen). On match consumes through ')' and returns true; otherwise i is
// untouched.
bool MatchCurrentUser(const std::vector<Tok>& tokens, std::size_t& i) {
    if (i + 2 < tokens.size() && tokens[i].Kind == TokKind::Word && EqIgnoreCase(tokens[i].Text, "currentUser") &&
        tokens[i + 1].Kind == TokKind::LParen && tokens[i + 2].Kind == TokKind::RParen) {
        i += 3;
        return true;
    }
    return false;
}

// Read the RHS of a comparison: either a quoted string, a bareword, or
// `currentUser()`. Writes the resolved value + a flag indicating whether the
// value was a `currentUser()` call. Returns false if no value found.
bool ReadValue(const std::vector<Tok>& tokens, std::size_t& i, std::string& outValue, bool& outIsCurrentUser) {
    outValue.clear();
    outIsCurrentUser = false;
    if (i >= tokens.size()) {
        return false;
    }
    if (tokens[i].Kind == TokKind::Word && EqIgnoreCase(tokens[i].Text, "currentUser")) {
        if (MatchCurrentUser(tokens, i)) {
            outIsCurrentUser = true;
            return true;
        }
    }
    if (tokens[i].Kind == TokKind::String || tokens[i].Kind == TokKind::Word) {
        outValue = tokens[i].Text;
        ++i;
        return true;
    }
    return false;
}

void AppendQueryTerm(std::string& q, const std::string& term) {
    if (term.empty()) {
        return;
    }
    if (!q.empty()) {
        q.push_back(' ');
    }
    q.append(term);
}

void AppendWarning(std::string& w, const std::string& msg) {
    if (msg.empty()) {
        return;
    }
    if (!w.empty()) {
        w.append("; ");
    }
    w.append(msg);
}

// Quote a value if it contains whitespace, otherwise emit bare. GitHub's
// search-qualifier syntax accepts both `label:bug` and `label:"needs review"`.
// When wrapping, embedded `"` and `\` are backslash-escaped so a value carrying
// a quote can't terminate the qualifier early (and can't inject a second one).
std::string MaybeQuote(const std::string& v) {
    const bool needs =
        std::any_of(v.begin(), v.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; });
    if (!needs) {
        return v;
    }
    std::string out;
    out.reserve(v.size() + 2);
    out.push_back('"');
    for (char c : v) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// Token-aware ORDER BY clause stripping. JQL allows `ORDER BY <field> <dir>`
// at the end of the query; GitHub `/search/issues` uses a separate `&sort=`
// parameter so we drop the clause + emit a warning.
// The prior pre-tokenize `lower.find("order by")`
// was context-blind and corrupted any quoted string containing the
// substring (e.g. `text ~ "work order by priority"`). Walking the token
// stream and matching only on bare `Word` tokens fixes that — quoted-
// string tokens carry a different Kind and never match.
void StripOrderByClause(std::vector<Tok>& tokens, std::string& warning) {
    for (std::size_t j = 0; j + 1 < tokens.size(); ++j) {
        if (tokens[j].Kind == TokKind::Word && EqIgnoreCase(tokens[j].Text, "ORDER") &&
            tokens[j + 1].Kind == TokKind::Word && EqIgnoreCase(tokens[j + 1].Text, "BY")) {
            AppendWarning(warning, "ORDER BY clause dropped (GitHub search uses separate sort parameter)");
            tokens.resize(j); // drop everything from `ORDER` onward
            return;
        }
    }
}

// status = "Open" → is:open, status != "Closed" → is:open, etc. Maps a small
// set of common JQL statuses to GitHub's binary open/closed state.
void HandleStatusClause(const std::string& op, const std::string& value, std::string& body, std::string& warning) {
    const std::string lowerVal = ToLower(value);
    bool isOpenLike =
        (lowerVal == "open" || lowerVal == "to do" || lowerVal == "in progress" || lowerVal == "reopened");
    bool isClosedLike = (lowerVal == "closed" || lowerVal == "done" || lowerVal == "resolved");
    if (op == "=" && isOpenLike) {
        AppendQueryTerm(body, "is:open");
    } else if (op == "=" && isClosedLike) {
        AppendQueryTerm(body, "is:closed");
    } else if (op == "!=" && isClosedLike) {
        AppendQueryTerm(body, "is:open");
    } else if (op == "!=" && isOpenLike) {
        AppendQueryTerm(body, "is:closed");
    } else {
        AppendWarning(warning, "status '" + value + "' has no direct GitHub equivalent (dropped)");
    }
}

// github-commit-tracker-rows — `type:` handling. op is already
// normalized from ":" to "=" by the caller, so only "=" reaches here for the
// shorthand + full-JQL forms alike.
void HandleTypeClause(const std::string& op, const std::string& value, std::string& body, JqlToGitHubResult& result) {
    if (op != "=") {
        AppendWarning(result.Warning, "Unsupported operator '" + op + "' on type");
        return;
    }
    const std::string lowerVal = ToLower(value);
    if (lowerVal == "pr" || lowerVal == "pullrequest" || lowerVal == "pull-request") {
        AppendQueryTerm(body, "is:pr");
        result.IsPullRequestQuery = true;
        result.IncludePullRequests = true;
    } else if (lowerVal == "issue") {
        AppendQueryTerm(body, "is:issue");
    } else if (lowerVal == "commit") {
        // github-commit-tracker-rows — commits only. No GitHub search-qualifier
        // term (commits aren't a /search/issues kind); the REST commit path is
        // selected via the flags.
        result.IncludeCommits = true;
        result.IncludeIssuesOrPullRequests = false;
    } else if (lowerVal == "all" || lowerVal == "any") {
        // github-commit-tracker-rows — issues + PRs + commits. Keep PR rows (no
        // is:pr injection so plain issues stay) and additionally fetch commits.
        result.IncludePullRequests = true;
        result.IncludeCommits = true;
    } else {
        AppendWarning(result.Warning, std::string("Unknown 'type:' value '") + value +
                                          "' — ignored (use 'pr', 'issue', 'commit', 'all', or 'any')");
    }
}

// Native GitHub-search `is:` qualifier (#984). A raw `is:pr` / `is:issue` /
// `is:open` / `is:closed` token is GitHub-native authoritative syntax: re-emit
// the qualifier verbatim AND set the matching include-flags so the row-mapper
// keeps the rows the GraphQL search returns. Without this, `is:pr` returns PRs
// from GraphQL but the mapper drops them all (IncludePullRequests stayed false).
void HandleIsClause(const std::string& op, const std::string& value, std::string& body, JqlToGitHubResult& result) {
    if (op != "=") {
        AppendWarning(result.Warning, "Unsupported operator '" + op + "' on is");
        return;
    }
    const std::string lowerVal = ToLower(value);
    if (lowerVal == "pr" || lowerVal == "pull-request" || lowerVal == "pullrequest") {
        AppendQueryTerm(body, "is:pr");
        result.IsPullRequestQuery = true;
        result.IncludePullRequests = true;
    } else if (lowerVal == "issue") {
        AppendQueryTerm(body, "is:issue");
    } else if (lowerVal == "open") {
        AppendQueryTerm(body, "is:open");
    } else if (lowerVal == "closed") {
        AppendQueryTerm(body, "is:closed");
    } else if (lowerVal == "merged" || lowerVal == "unmerged" || lowerVal == "draft" || lowerVal == "public" ||
               lowerVal == "private") {
        // Other GitHub-native is: qualifiers are PR/repo scoped — these imply PRs,
        // so keep PR rows. Re-emit verbatim; GraphQL applies the actual filter.
        AppendQueryTerm(body, std::string("is:") + lowerVal);
        if (lowerVal == "merged" || lowerVal == "unmerged" || lowerVal == "draft") {
            result.IncludePullRequests = true;
        }
    } else {
        AppendWarning(result.Warning,
                      std::string("Unknown 'is:' qualifier '") + value +
                          "' — ignored (use 'pr', 'issue', 'open', 'closed', 'merged', 'unmerged', 'draft', "
                          "'public', or 'private')");
    }
}

// assignee = currentUser() → assignee:@me, assignee = "login" → assignee:login.
// `!=` and other operators are unsupported (GitHub search has no assignee
// negation qualifier).
void HandleAssigneeClause(const std::string& op, const std::string& value, bool isCurrentUser, std::string& body,
                          std::string& warning) {
    if (op != "=" && op != "!=") {
        AppendWarning(warning, "Unsupported operator '" + op + "' on assignee");
    } else if (op == "!=") {
        AppendWarning(warning, "Unsupported negation 'assignee != ...' dropped");
    } else {
        AppendQueryTerm(body, std::string("assignee:") + (isCurrentUser ? "@me" : MaybeQuote(value)));
    }
}

// reporter / author = currentUser() → author:@me, = "login" → author:login.
void HandleReporterClause(const std::string& op, const std::string& value, bool isCurrentUser, std::string& body,
                          std::string& warning) {
    if (op != "=") {
        AppendWarning(warning, "Unsupported operator '" + op + "' on reporter");
    } else {
        AppendQueryTerm(body, std::string("author:") + (isCurrentUser ? "@me" : MaybeQuote(value)));
    }
}

// labels / label = "value" → label:"value" (quoted when multi-word). Empty
// value and non-`=` operators drop with a warning.
void HandleLabelsClause(const std::string& op, const std::string& value, std::string& body, std::string& warning) {
    if (op != "=") {
        AppendWarning(warning, "Unsupported operator '" + op + "' on labels");
    } else if (value.empty()) {
        AppendWarning(warning, "Empty label value dropped");
    } else {
        AppendQueryTerm(body, std::string("label:") + MaybeQuote(value));
    }
}

// text / summary / description ~ "phrase" → bare quoted phrase. `=` is accepted
// as an alias for `~`.
void HandleTextClause(const std::string& field, const std::string& op, const std::string& value, std::string& body,
                      std::string& warning) {
    if (op != "~" && op != "=") {
        AppendWarning(warning, "Unsupported operator '" + op + "' on " + field);
    } else {
        AppendQueryTerm(body, MaybeQuote(value));
    }
}

// Dispatch a single `<field> <op> <value>` clause to its per-field handler,
// appending to `body` or `result.Warning`. `isCurrentUser` is true when the
// value was a `currentUser()` call.
void HandleFieldClause(const std::string& field, const std::string& op, const std::string& value, bool isCurrentUser,
                       std::string& body, JqlToGitHubResult& result) {
    if (EqIgnoreCase(field, "project")) {
        AppendWarning(result.Warning, "project key '" + value + "' ignored (GitHub has no project-key concept)");
    } else if (EqIgnoreCase(field, "assignee")) {
        HandleAssigneeClause(op, value, isCurrentUser, body, result.Warning);
    } else if (EqIgnoreCase(field, "reporter") || EqIgnoreCase(field, "author")) {
        HandleReporterClause(op, value, isCurrentUser, body, result.Warning);
    } else if (EqIgnoreCase(field, "status")) {
        HandleStatusClause(op, value, body, result.Warning);
    } else if (EqIgnoreCase(field, "labels") || EqIgnoreCase(field, "label")) {
        HandleLabelsClause(op, value, body, result.Warning);
    } else if (EqIgnoreCase(field, "type")) {
        HandleTypeClause(op, value, body, result);
    } else if (EqIgnoreCase(field, "is")) {
        HandleIsClause(op, value, body, result);
    } else if (EqIgnoreCase(field, "text") || EqIgnoreCase(field, "summary") || EqIgnoreCase(field, "description")) {
        HandleTextClause(field, op, value, body, result.Warning);
    } else if (EqIgnoreCase(field, "key") || EqIgnoreCase(field, "issuekey")) {
        // Item 18c (user-info-window) — single-issue narrowing. GitHub search has
        // no exact issue-key qualifier, so the canonical key is carried out-of-band
        // in result.KeyFilter and the fetch path post-filters client-side.
        ParsedIssueKey parsed;
        if (op != "=") {
            AppendWarning(result.Warning, "Unsupported operator '" + op + "' on key");
        } else if (!ParseGitHubIssueKey(value, parsed)) {
            AppendWarning(result.Warning,
                          "key value '" + value + "' is not a canonical owner/repo#N GitHub issue key — dropped");
        } else {
            result.KeyFilter = FormatGitHubIssueKey(parsed.Owner, parsed.Repo, parsed.Number);
        }
    } else {
        AppendWarning(result.Warning, std::string("Unsupported JQL field '") + field + "' dropped");
    }
}

// Walk the token stream, translating each clause into `body` / `result`. Tokens
// must already have had the ORDER BY clause stripped.
void TranslateClauses(const std::vector<Tok>& tokens, std::string& body, JqlToGitHubResult& result) {
    std::size_t i = 0;
    while (i < tokens.size() && tokens[i].Kind != TokKind::End) {
        const Tok& t = tokens[i];

        // AND / OR connectors at top level. AND becomes implicit space (skip);
        // OR is unsupported — warn and treat as AND so we don't drop following
        // clauses entirely.
        if (t.Kind == TokKind::Word && EqIgnoreCase(t.Text, "AND")) {
            ++i;
            continue;
        }
        if (t.Kind == TokKind::Word && EqIgnoreCase(t.Text, "OR")) {
            AppendWarning(result.Warning, "OR connector unsupported (GitHub search is AND-only); treating as AND");
            ++i;
            continue;
        }

        // Field-comparison clauses: <field> <op> <value>.
        if (t.Kind == TokKind::Word && i + 1 < tokens.size() && tokens[i + 1].Kind == TokKind::Op) {
            const std::string field = t.Text;
            // Normalize `:` shorthand (e.g. `status:open`, `assignee:@me`) to `=`
            // so every field handler treats them uniformly.
            std::string op = tokens[i + 1].Text;
            if (op == ":") {
                op = "=";
            }
            std::size_t valIdx = i + 2;
            std::string value;
            bool isCurrentUser = false;
            if (!ReadValue(tokens, valIdx, value, isCurrentUser)) {
                AppendWarning(result.Warning, std::string("Missing value after '") + field + " " + op + "'");
                ++i;
                continue;
            }
            HandleFieldClause(field, op, value, isCurrentUser, body, result);
            i = valIdx;
            continue;
        }

        // Bare token we can't classify (stray identifier, paren without
        // currentUser, etc.) — skip with no warning for stray symbols, warning
        // for unrecognised words so downstream debugging has a breadcrumb.
        if (t.Kind == TokKind::Word) {
            AppendWarning(result.Warning, std::string("Unrecognised token '") + t.Text + "' dropped");
        }
        ++i;
    }
}

// Strict `owner/repo` shape check: exactly one '/', non-empty on both sides, and
// every other char drawn from the GitHub owner/repo alphabet (alnum + '-' '_'
// '.'). A token carrying qualifier punctuation (`:` etc.) or a second '/' is
// rejected so a stray slash inside a value (e.g. a `ui/ux` label) can't be
// mistaken for a repo anchor.
bool IsOwnerRepoToken(const std::string& token) {
    std::size_t slashCount = 0;
    std::size_t slashPos = std::string::npos;
    for (std::size_t i = 0; i < token.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(token[i]);
        if (c == '/') {
            ++slashCount;
            slashPos = i;
            continue;
        }
        const bool allowed = std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
        if (!allowed) {
            return false;
        }
    }
    return slashCount == 1 && slashPos > 0 && slashPos + 1 < token.size();
}

// Compose the final query: optional `repo:<owner>/<repo>` prefix, then body.
std::string ComposeQuery(const std::string& owner, const std::string& repo, const std::string& body) {
    std::string out;
    if (!owner.empty() && !repo.empty()) {
        out = std::string("repo:") + owner + "/" + repo;
    }
    if (!body.empty()) {
        if (!out.empty()) {
            out.push_back(' ');
        }
        out.append(body);
    }
    return out;
}

} // namespace

JqlToGitHubResult TranslateJqlToGitHubSearch(const std::string& jql, const std::string& owner,
                                             const std::string& repo) {
    JqlToGitHubResult result;
    result.Ok = true;

    std::vector<Tok> tokens;
    std::string tokError;
    if (!Tokenize(jql, tokens, tokError)) {
        result.Ok = false;
        result.Error = tokError;
        return result;
    }

    StripOrderByClause(tokens, result.Warning);

    std::string body; // accumulator for the translated terms (excluding repo: prefix)
    TranslateClauses(tokens, body, result);

    result.Query = ComposeQuery(owner, repo, body);
    return result;
}

std::string ExtractGitHubProjectAnchor(const std::string& query) {
    // The GitHub "project" anchor is a leading `owner/repo` token. Anchor on the
    // FIRST whitespace-delimited token and accept it only when that token is
    // itself shaped `owner/repo`; a bare '/' elsewhere in the query (e.g. a
    // `ui/ux` label value) must not be mistaken for a repo anchor.
    const std::size_t firstSpace = query.find(' ');
    const std::string token = (firstSpace == std::string::npos) ? query : query.substr(0, firstSpace);
    if (!IsOwnerRepoToken(token)) {
        return std::string();
    }
    return token;
}

} // namespace github
} // namespace smatchet
