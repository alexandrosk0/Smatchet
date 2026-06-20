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

// Split on a whitespace-delimited, case-insensitive `AND` connector. Quoted
// values containing the literal word are an accepted limitation of the subset.
void SplitTopLevelAnd(const std::string& s, std::vector<std::string>& out) {
    const std::string lower = ToLower(s);
    const std::string token = " and ";
    std::string::size_type start = 0;
    std::string::size_type pos = lower.find(token, 0);
    while (pos != std::string::npos) {
        out.push_back(s.substr(start, pos - start));
        start = pos + token.size();
        pos = lower.find(token, start);
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

// Map one already-split `field OP value` clause into the result filter.
void ProcessClause(const std::string& clauseRaw, JqlToLinearResult& r) {
    const std::string clause = Trim(clauseRaw);
    if (clause.empty()) {
        return;
    }
    const std::string lower = ToLower(clause);

    // Operator detection (checked in precedence order).
    std::string field;
    std::string valueRaw;
    enum Op { kEq, kNeq, kIn, kNotIn, kContains } op = kEq;
    std::string::size_type p;
    if ((p = lower.find(" not in ")) != std::string::npos) {
        op = kNotIn;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 8);
    } else if ((p = lower.find(" in ")) != std::string::npos) {
        op = kIn;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 4);
    } else if ((p = clause.find("!=")) != std::string::npos) {
        op = kNeq;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 2);
    } else if ((p = clause.find('~')) != std::string::npos) {
        op = kContains;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 1);
    } else if ((p = clause.find('=')) != std::string::npos) {
        op = kEq;
        field = clause.substr(0, p);
        valueRaw = clause.substr(p + 1);
    } else {
        AddWarning(r, "unsupported clause '" + clause + "'");
        return;
    }

    field = ToLower(Trim(field));
    const std::string value = StripQuotes(Trim(valueRaw));
    const std::string valueLower = ToLower(value);

    // Reject an OR buried inside a single clause rather than guess its meaning.
    if (op != kIn && op != kNotIn && valueLower.find(" or ") != std::string::npos) {
        AddWarning(r, "OR is not supported (clause '" + clause + "' dropped)");
        return;
    }

    if (field == "assignee") {
        if (op == kEq && valueLower == "currentuser()") {
            r.Filter["assignee"]["isMe"]["eq"] = true;
        } else if (op == kEq) {
            r.Filter["assignee"]["name"]["containsIgnoreCase"] = value;
        } else {
            AddWarning(r, "unsupported assignee operator");
        }
    } else if (field == "reporter" || field == "creator") {
        if (op == kEq && valueLower == "currentuser()") {
            r.Filter["creator"]["isMe"]["eq"] = true;
        } else if (op == kEq) {
            r.Filter["creator"]["name"]["containsIgnoreCase"] = value;
        } else {
            AddWarning(r, "unsupported reporter operator");
        }
    } else if (field == "status" || field == "state") {
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
    } else if (field == "labels" || field == "label") {
        if (op == kEq) {
            r.Filter["labels"]["name"]["eq"] = value;
        } else if (op == kIn) {
            std::vector<std::string> items;
            ParseInList(valueRaw, items);
            r.Filter["labels"]["name"]["in"] = items;
        } else {
            AddWarning(r, "unsupported labels operator");
        }
    } else if (field == "priority") {
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
    } else if (op == kContains &&
               (field == "text" || field == "summary" || field == "title" || field == "description")) {
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
