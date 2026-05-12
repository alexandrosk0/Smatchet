#include "JqlProjectScope.h"

#include <cctype>
#include <string>
#include <vector>

namespace JqlProjectScope {

namespace {

bool IsIdentChar(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isalnum(uc) != 0 || c == '_' || c == '-';
}

bool IsAsciiSpace(char c) {
    const unsigned char uc = static_cast<unsigned char>(c);
    return std::isspace(uc) != 0;
}

void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && IsAsciiSpace(s[i])) {
        ++i;
    }
}

bool MatchKeywordCI(const std::string& s, size_t pos, const char* kw, size_t& outEnd) {
    size_t i = pos;
    size_t k = 0;
    while (kw[k] != '\0') {
        if (i >= s.size()) {
            return false;
        }
        const char a = static_cast<char>(
            std::tolower(static_cast<unsigned char>(s[i])));
        const char b = static_cast<char>(
            std::tolower(static_cast<unsigned char>(kw[k])));
        if (a != b) {
            return false;
        }
        ++i;
        ++k;
    }
    // Must not be followed by another identifier char (word-boundary).
    if (i < s.size() && IsIdentChar(s[i])) {
        return false;
    }
    outEnd = i;
    return true;
}

// Read a project token from position `i`. Handles "PROJ", "FOO-BAR",
// double-quoted "PROJ", and numeric ids. Advances `i` past the token.
// Returns "" if no valid token is at `i`.
std::string ReadProjectToken(const std::string& s, size_t& i) {
    SkipWs(s, i);
    if (i >= s.size()) {
        return "";
    }
    if (s[i] == '"') {
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '"') {
            out.push_back(s[i]);
            ++i;
        }
        if (i < s.size()) {
            ++i; // closing quote
        }
        return out;
    }
    if (s[i] == '\'') {
        ++i;
        std::string out;
        while (i < s.size() && s[i] != '\'') {
            out.push_back(s[i]);
            ++i;
        }
        if (i < s.size()) {
            ++i;
        }
        return out;
    }
    std::string out;
    while (i < s.size() && IsIdentChar(s[i])) {
        out.push_back(s[i]);
        ++i;
    }
    return out;
}

// Try to parse a project clause starting at the "project" keyword position.
// On success, sets `outProjects` (1+ entries for `in (...)`, 1 for `=`) and
// advances `i` past the clause. Returns true if it consumed a clause.
bool TryParseProjectClause(const std::string& s, size_t& i, std::vector<std::string>& outProjects) {
    size_t after = 0;
    if (!MatchKeywordCI(s, i, "project", after)) {
        return false;
    }
    size_t j = after;
    SkipWs(s, j);
    if (j >= s.size()) {
        return false;
    }
    if (s[j] == '=') {
        ++j;
        SkipWs(s, j);
        const std::string tok = ReadProjectToken(s, j);
        if (tok.empty()) {
            return false;
        }
        outProjects.push_back(tok);
        i = j;
        return true;
    }
    // Try "in" / "IN"
    size_t afterIn = 0;
    if (MatchKeywordCI(s, j, "in", afterIn)) {
        j = afterIn;
        SkipWs(s, j);
        if (j >= s.size() || s[j] != '(') {
            return false;
        }
        ++j;
        while (j < s.size()) {
            SkipWs(s, j);
            const std::string tok = ReadProjectToken(s, j);
            if (!tok.empty()) {
                outProjects.push_back(tok);
            }
            SkipWs(s, j);
            if (j >= s.size()) {
                return false;
            }
            if (s[j] == ',') {
                ++j;
                continue;
            }
            if (s[j] == ')') {
                ++j;
                i = j;
                return true;
            }
            return false;
        }
        return false;
    }
    return false;
}

// Walk the JQL collecting every project clause we can find.
// Skips quoted strings so e.g. summary ~ "project = X" doesn't count.
std::vector<std::string> CollectProjects(const std::string& s) {
    std::vector<std::string> all;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < s.size() && s[i] != quote) {
                ++i;
            }
            if (i < s.size()) {
                ++i;
            }
            continue;
        }
        // Word-boundary check: previous char must not be an identifier char,
        // otherwise we'd match "myproject" as "project".
        const bool atWordBoundary = (i == 0) || !IsIdentChar(s[i - 1]);
        if (atWordBoundary) {
            size_t save = i;
            if (TryParseProjectClause(s, save, all)) {
                i = save;
                continue;
            }
        }
        ++i;
    }
    return all;
}

} // namespace

std::string ExtractSingleProject(const std::string& jql) {
    const std::vector<std::string> projects = CollectProjects(jql);
    if (projects.size() != 1) {
        return "";
    }
    return projects[0];
}

bool HasProjectScope(const std::string& jql) {
    const std::vector<std::string> projects = CollectProjects(jql);
    return !projects.empty();
}

namespace {

// Locate the first project clause in `s`. On success returns true and sets [outBegin, outEnd)
// to the slice covering the clause (e.g. "project = FOO" / "project in (A, B)"). Mirrors
// TryParseProjectClause's parse, but additionally skips trailing `AND` / `OR` so we can
// rewrite cleanly without leaving stray boolean connectives.
bool FindFirstProjectClauseRange(const std::string& s, size_t& outBegin, size_t& outEnd,
                                 bool& consumedTrailingConnective) {
    consumedTrailingConnective = false;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '"' || c == '\'') {
            const char quote = c;
            ++i;
            while (i < s.size() && s[i] != quote) {
                ++i;
            }
            if (i < s.size()) {
                ++i;
            }
            continue;
        }
        const bool atWordBoundary = (i == 0) || !IsIdentChar(s[i - 1]);
        if (atWordBoundary) {
            const size_t clauseStart = i;
            std::vector<std::string> dummy;
            size_t save = i;
            if (TryParseProjectClause(s, save, dummy)) {
                size_t end = save;
                // Try to absorb a trailing connective ("AND" / "OR") to keep the JQL well-formed
                // when we drop the clause.
                size_t afterWs = end;
                SkipWs(s, afterWs);
                size_t afterKw = 0;
                if (MatchKeywordCI(s, afterWs, "and", afterKw) ||
                    MatchKeywordCI(s, afterWs, "or", afterKw)) {
                    end = afterKw;
                    consumedTrailingConnective = true;
                }
                outBegin = clauseStart;
                outEnd = end;
                return true;
            }
        }
        ++i;
    }
    return false;
}

std::string LTrimAscii(std::string s) {
    while (!s.empty() && IsAsciiSpace(s.front())) {
        s.erase(s.begin());
    }
    return s;
}

std::string RTrimAscii(std::string s) {
    while (!s.empty() && IsAsciiSpace(s.back())) {
        s.pop_back();
    }
    return s;
}

} // namespace

std::string SetProjectClause(const std::string& jql, const std::string& projectKey) {
    if (projectKey.empty()) {
        return jql;
    }
    const std::string replacement = std::string("project = ") + projectKey;

    size_t b = 0;
    size_t e = 0;
    bool consumedConn = false;
    if (FindFirstProjectClauseRange(jql, b, e, consumedConn)) {
        std::string head = jql.substr(0, b);
        std::string tail = jql.substr(e);
        if (consumedConn) {
            // We absorbed the connective; emit replacement + " AND " + remainder.
            std::string out = RTrimAscii(std::move(head));
            std::string tailTrim = LTrimAscii(std::move(tail));
            if (!out.empty()) {
                out += " ";
            }
            out += replacement;
            if (!tailTrim.empty()) {
                out += " AND ";
                out += tailTrim;
            }
            return out;
        }
        // No connective absorbed; literal splice.
        return head + replacement + tail;
    }

    // No existing project clause — prepend.
    const std::string trimmed = LTrimAscii(jql);
    if (trimmed.empty()) {
        return replacement;
    }
    return replacement + " AND " + trimmed;
}

} // namespace JqlProjectScope
