#include "LinearQueryFromJql.h"

#include <cctype>
#include <string>
#include <vector>

namespace smatchet {
namespace linear {

namespace {

std::string Trim(const std::string& s) {
    std::string::size_type b = 0;
    std::string::size_type e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) {
        ++b;
    }
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) {
        --e;
    }
    return s.substr(b, e - b);
}

std::string ToLower(const std::string& s) {
    std::string out = s;
    for (std::string::size_type i = 0; i < out.size(); ++i) {
        out[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[i])));
    }
    return out;
}

std::string StripQuotes(const std::string& s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

void AddWarning(JqlToLinearResult& r, const std::string& w) {
    if (!r.Warning.empty()) {
        r.Warning += "; ";
    }
    r.Warning += w;
}

// Find the first occurrence of `needle` in `hay` at index >= `from` that lies
// OUTSIDE any quoted span. Quote spans open on `'`/`"` and close on the matching
// quote (no escape handling — mirrors StripQuotes). Quote state is tracked from
// the start of `hay` so a `from` past a balanced split point is safe. Returns
// npos when no top-level match exists. This keeps connector/operator substrings
// that live inside quoted values (e.g. `"work in progress"`) from being treated
// as structural tokens.
std::string::size_type FindTopLevel(const std::string& hay, const std::string& needle,
                                    std::string::size_type from = 0) {
    if (needle.empty() || needle.size() > hay.size()) {
        return std::string::npos;
    }
    char quote = 0;
    for (std::string::size_type i = 0; i < hay.size(); ++i) {
        const char c = hay[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote = c;
            continue;
        }
        if (i >= from && i + needle.size() <= hay.size() && hay.compare(i, needle.size(), needle) == 0) {
            return i;
        }
    }
    return std::string::npos;
}

// Split on a whitespace-delimited, case-insensitive `AND` connector. Quote-aware:
// an ` and ` inside a quoted value (e.g. `summary ~ "cut and paste"`) is treated
// atomically and does not split the clause.
void SplitTopLevelAnd(const std::string& s, std::vector<std::string>& out) {
    const std::string lower = ToLower(s);
    const std::string token = " and ";
    std::string::size_type start = 0;
    std::string::size_type pos = FindTopLevel(lower, token, start);
    while (pos != std::string::npos) {
        out.push_back(s.substr(start, pos - start));
        start = pos + token.size();
        pos = FindTopLevel(lower, token, start);
    }
    out.push_back(s.substr(start));
}

void ParseInList(const std::string& raw, std::vector<std::string>& out) {
    std::string inner = Trim(raw);
    if (!inner.empty() && inner.front() == '(') {
        inner.erase(inner.begin());
    }
    if (!inner.empty() && inner.back() == ')') {
        inner.pop_back();
    }
    std::string::size_type start = 0;
    for (std::string::size_type i = 0; i <= inner.size(); ++i) {
        if (i == inner.size() || inner[i] == ',') {
            const std::string item = StripQuotes(Trim(inner.substr(start, i - start)));
            if (!item.empty()) {
                out.push_back(item);
            }
            start = i + 1;
        }
    }
}

bool PriorityNameToInt(const std::string& nameLower, int& out) {
    if (nameLower == "urgent") {
        out = 1;
    } else if (nameLower == "high") {
        out = 2;
    } else if (nameLower == "medium") {
        out = 3;
    } else if (nameLower == "low") {
        out = 4;
    } else if (nameLower == "no priority" || nameLower == "none" || nameLower == "no_priority") {
        out = 0;
    } else {
        return false;
    }
    return true;
}

enum ClauseOp { kEq, kNeq, kIn, kNotIn, kContains };

// Detect the operator in a clause and split it into field + raw value. Returns
// false when no recognized operator is present.
bool DetectOperator(const std::string& clause, const std::string& lower, ClauseOp& op, std::string& field,
                    std::string& valueRaw) {
    // All operator scans are quote-aware (FindTopLevel) so an operator substring
    // inside a quoted value (e.g. the ` in ` of `summary ~ "x in y"`, or the `=`
    // of `text ~ "a = b"`) is never mistaken for the clause operator — which
    // previously corrupted the split and silently dropped the text search.
    std::string::size_type p;
    if ((p = FindTopLevel(lower, " not in ")) != std::string::npos) {
        op = kNotIn;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 8);
    } else if ((p = FindTopLevel(lower, " in ")) != std::string::npos) {
        op = kIn;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 4);
    } else if ((p = FindTopLevel(clause, "!=")) != std::string::npos) {
        op = kNeq;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 2);
    } else if ((p = FindTopLevel(clause, "~")) != std::string::npos) {
        op = kContains;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 1);
    } else if ((p = FindTopLevel(clause, "=")) != std::string::npos) {
        op = kEq;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 1);
    } else {
        return false;
    }
    return true;
}

void HandleAssignee(ClauseOp op, const std::string& value, const std::string& valueLower, const char* relation,
                    JqlToLinearResult& r) {
    if (op == kEq && valueLower == "currentuser()") {
        r.Filter[relation]["isMe"]["eq"] = true;
    } else if (op == kEq) {
        r.Filter[relation]["name"]["containsIgnoreCase"] = value;
    } else {
        AddWarning(r, std::string("unsupported ") + relation + " operator");
    }
}

void HandleStatus(ClauseOp op, const std::string& value, const std::string& valueRaw, JqlToLinearResult& r) {
    if (op == kEq) {
        r.Filter["state"]["name"]["eq"] = value;
    } else if (op == kNeq) {
        r.Filter["state"]["name"]["neq"] = value;
    } else if (op == kIn) {
        std::vector<std::string> items;
        ParseInList(valueRaw, items);
        r.Filter["state"]["name"]["in"] = items;
    } else {
        AddWarning(r, "unsupported status operator");
    }
}

void HandleLabels(ClauseOp op, const std::string& value, const std::string& valueRaw, JqlToLinearResult& r) {
    if (op == kEq) {
        r.Filter["labels"]["name"]["eq"] = value;
    } else if (op == kIn) {
        std::vector<std::string> items;
        ParseInList(valueRaw, items);
        r.Filter["labels"]["name"]["in"] = items;
    } else {
        AddWarning(r, "unsupported labels operator");
    }
}

void HandlePriority(ClauseOp op, const std::string& value, const std::string& valueLower, const std::string& valueRaw,
                    JqlToLinearResult& r) {
    if (op == kEq) {
        int pr = 0;
        if (PriorityNameToInt(valueLower, pr)) {
            r.Filter["priority"]["eq"] = pr;
        } else {
            AddWarning(r, "unknown priority '" + value + "'");
        }
    } else if (op == kIn) {
        std::vector<std::string> items;
        ParseInList(valueRaw, items);
        std::vector<int> ints;
        for (std::string::size_type i = 0; i < items.size(); ++i) {
            int pr = 0;
            if (PriorityNameToInt(ToLower(items[i]), pr)) {
                ints.push_back(pr);
            } else {
                AddWarning(r, "unknown priority '" + items[i] + "'");
            }
        }
        if (!ints.empty()) {
            r.Filter["priority"]["in"] = ints;
        }
    } else {
        AddWarning(r, "unsupported priority operator");
    }
}

bool IsTextField(const std::string& field) {
    return field == "text" || field == "summary" || field == "title" || field == "description";
}

// Map one already-split clause into the result filter. Operator detection +
// per-field dispatch are kept thin (the heavy lifting lives in the Handle*
// helpers) so this stays under the size/branch caps.
void ProcessClause(const std::string& clauseRaw, JqlToLinearResult& r) {
    const std::string clause = Trim(clauseRaw);
    if (clause.empty()) {
        return;
    }
    const std::string lower = ToLower(clause);

    ClauseOp op = kEq;
    std::string field;
    std::string valueRaw;
    if (!DetectOperator(clause, lower, op, field, valueRaw)) {
        AddWarning(r, "unsupported clause '" + clause + "'");
        return;
    }

    field = ToLower(Trim(field));
    const std::string value = StripQuotes(Trim(valueRaw));
    const std::string valueLower = ToLower(value);

    // Reject an OR buried inside a single clause rather than guess its meaning.
    // Quote-aware (scan the full clause, not the stripped value) so an ` or `
    // inside a quoted text value (e.g. `summary ~ "cats or dogs"`) is treated
    // atomically and the text search survives instead of being dropped.
    if (op != kIn && op != kNotIn && FindTopLevel(lower, " or ") != std::string::npos) {
        AddWarning(r, "OR is not supported (clause '" + clause + "' dropped)");
        return;
    }

    if (field == "assignee") {
        HandleAssignee(op, value, valueLower, "assignee", r);
    } else if (field == "reporter" || field == "creator") {
        HandleAssignee(op, value, valueLower, "creator", r);
    } else if (field == "status" || field == "state") {
        HandleStatus(op, value, valueRaw, r);
    } else if (field == "labels" || field == "label") {
        HandleLabels(op, value, valueRaw, r);
    } else if (field == "priority") {
        HandlePriority(op, value, valueLower, valueRaw, r);
    } else if (field == "project") {
        if (op == kEq) {
            r.Filter["project"]["name"]["eq"] = value;
        } else {
            AddWarning(r, "unsupported project operator");
        }
    } else if (field == "team") {
        if (op == kEq) {
            r.Filter["team"]["key"]["eq"] = value;
        } else {
            AddWarning(r, "unsupported team operator");
        }
    } else if (op == kContains && IsTextField(field)) {
        r.SearchTerm = value;
    } else {
        AddWarning(r, "unsupported field '" + field + "'");
    }
}

} // namespace

JqlToLinearResult TranslateJqlToLinearFilter(const std::string& jql) {
    JqlToLinearResult r;
    r.Ok = true;

    std::string s = Trim(jql);
    if (s.empty()) {
        return r;
    }

    // Strip a trailing ORDER BY (Linear ordering is set elsewhere).
    const std::string lower = ToLower(s);
    const std::string::size_type ob = lower.find(" order by ");
    if (ob != std::string::npos) {
        AddWarning(r, "ORDER BY ignored");
        s = Trim(s.substr(0, ob));
    }

    std::vector<std::string> clauses;
    SplitTopLevelAnd(s, clauses);
    for (std::string::size_type i = 0; i < clauses.size(); ++i) {
        ProcessClause(clauses[i], r);
    }
    return r;
}

} // namespace linear
} // namespace smatchet
