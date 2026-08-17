#include "AnnotateAnalysisUi_Modals_detail.h"

#include "ConfigManager.h"
#include "P4Annotate.h"
#include "StringUtil.h"
#include "Tracker/TrackerFieldSchema.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace AnnotateUiPure {

std::string CsvEscape(const std::string& s) {
    bool needsQuotes =
        std::any_of(s.begin(), s.end(), [](char c) { return c == ',' || c == '"' || c == '\n' || c == '\r'; });
    if (!needsQuotes) {
        return s;
    }
    std::string out;
    out.reserve(s.size() + 4);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string NormalizeDateDisplay(const std::string& raw) {
    if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') {
        return raw.substr(0, 4) + "/" + raw.substr(5, 2) + "/" + raw.substr(8, 2);
    }
    return raw;
}

std::string SanitizeTsvCell(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

std::string ReplaceStringPlaceholder(std::string str, const std::string& placeholder, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = str.find(placeholder, pos)) != std::string::npos) {
        str.replace(pos, placeholder.length(), replacement);
        pos += replacement.length();
    }
    return str;
}

std::string BuildQuickCommentText(const std::string& issueKey, const std::string& templateId,
                                  const AnnotateRowView& row, const std::vector<CommentTemplate>& templates) {
    std::string text;
    bool found = false;
    auto it =
        std::find_if(templates.begin(), templates.end(), [&templateId](const auto& t) { return t.Id == templateId; });
    if (it != templates.end()) {
        text = it->Text;
        found = true;
    }
    if (!found) {
        if (templateId == "need_repro") {
            text = "Need repro details for {key} (annotate context: {path}:{line}, CL {cl}).";
        } else if (templateId == "need_logs") {
            text =
                "Please attach logs/diagnostics for {key} to continue triage.\nReference: {function} @ {path}:{line}.";
        } else {
            text = "Triage handoff for {key}:\n- Suggested owner: {user}\n- Suspect location: {function} "
                   "({path}:{line})\n- CL: {cl}";
        }
    }

    text = ReplaceStringPlaceholder(std::move(text), "{key}", issueKey);
    text = ReplaceStringPlaceholder(std::move(text), "{issueKey}", issueKey);
    text = ReplaceStringPlaceholder(std::move(text), "{path}", row.Path);
    text = ReplaceStringPlaceholder(std::move(text), "{line}", std::to_string(row.Line));
    text = ReplaceStringPlaceholder(std::move(text), "{cl}", row.Changelist);
    text = ReplaceStringPlaceholder(std::move(text), "{function}", row.Function);
    text = ReplaceStringPlaceholder(std::move(text), "{user}", row.User);

    return text;
}

std::string BuildCallstackRowTsv(const AnnotateRowView& row, std::size_t displayIndex) {
    std::ostringstream o;
    o << (displayIndex + 1) << '\t' << row.Function << '\t' << row.Path << ':' << row.Line << '\t' << row.User << '\t'
      << row.Changelist << '\t' << row.Date;
    return o.str();
}

std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln) {
    std::ostringstream o;
    o << ln.SourceLine << '\t' << SanitizeTsvCell(ln.Changelist) << '\t' << SanitizeTsvCell(ln.User) << '\t'
      << SanitizeTsvCell(ln.Date) << '\t' << SanitizeTsvCell(ln.Code);
    return o.str();
}

bool PickJiraAccountForP4User(const std::vector<TrackerUser>& users, const std::string& p4User,
                              std::string& outAccountId, std::string& outError) {
    outAccountId.clear();
    outError.clear();
    const std::string pl = ToLowerAsciiCopy(p4User);
    for (const auto& u : users) {
        size_t at = u.EmailAddress.find('@');
        const std::string local = at == std::string::npos ? u.EmailAddress : u.EmailAddress.substr(0, at);
        if (!local.empty() && ToLowerAsciiCopy(local) == pl) {
            outAccountId = u.AccountId;
            return true;
        }
    }
    if (!users.empty()) {
        outAccountId = users.front().AccountId;
        return true;
    }
    outError = "No Jira user match.";
    return false;
}

std::string GroupLookupErrorMessage(const std::string& detail) {
    // Same fallback wording the sibling sites use (SmatchetUserInfoUi.cpp group/member fetch),
    // so an empty-Detail backend failure is still visible instead of rendering as "no groups".
    return detail.empty() ? std::string("Group lookup failed.") : detail;
}

} // namespace AnnotateUiPure
