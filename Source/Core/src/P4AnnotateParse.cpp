#include "P4AnnotateParse.h"

#include "StringUtil.h"

#include <regex>
#include <string>
#include <vector>

namespace P4AnnotateParse {

std::vector<std::string> SplitLines(const std::string& s) {
    std::vector<std::string> lines;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) {
            lines.push_back(TrimCopy(s.substr(i)));
            break;
        }
        lines.push_back(TrimCopy(s.substr(i, j - i)));
        i = j + 1;
    }
    return lines;
}

void StripP4UserDomain(std::string& user) {
    const size_t at = user.find('@');
    if (at != std::string::npos) {
        user.resize(at);
    }
}

bool ParseAnnotateTextLine(const std::string& line, std::string& outCl, std::string& outUser, std::string& outAnnotDate,
                           std::string& outCode) {
    outCl.clear();
    outUser.clear();
    outAnnotDate.clear();
    outCode = line;
    static const std::regex reUserColonCode(R"(^\s*(\d+)\s+(\S+)\s*:\s*(.*)$)");
    // Code may be empty (blank source line): "rev: user YYYY/MM/DD" with nothing after the date.
    static const std::regex reRevUserDateCode(R"(^\s*(\d+)\s*:\s*(\S+)\s+(\S+)(?:\s+(.*))?$)");
    static const std::regex reRevUserDateCodeNoColon(R"(^\s*(\d+)\s+(\S+)\s+(\S+)(?:\s+(.*))?$)");
    static const std::regex revOnlyColonCode(R"(^\s*(\d+)\s*:\s*(.*)$)");
    std::smatch m;
    if (std::regex_match(line, m, reUserColonCode)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outCode = m[3].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, reRevUserDateCode)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outAnnotDate = m[3].str();
        outCode = m[4].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, reRevUserDateCodeNoColon)) {
        outCl = m[1].str();
        outUser = m[2].str();
        outAnnotDate = m[3].str();
        outCode = m[4].str();
        StripP4UserDomain(outUser);
        return true;
    }
    if (std::regex_match(line, m, revOnlyColonCode)) {
        outCl = m[1].str();
        outCode = m[2].str();
        return true;
    }
    return false;
}

P4LineAnnotate ParseLatestChangeFromChangesOutput(const std::string& stdoutText, const std::string& stderrText) {
    P4LineAnnotate b;
    b.Approximate = true;
    static const std::regex re(R"(Change\s+(\d+)\s+on\s+[^\s]+\s+by\s+(\S+))");
    std::smatch m;
    if (std::regex_search(stdoutText, m, re)) {
        b.Changelist = m[1].str();
        std::string who = m[2].str();
        const size_t at = who.find('@');
        if (at != std::string::npos) {
            who.resize(at);
        }
        b.User = who;
        return b;
    }
    if (!stderrText.empty()) {
        b.Error = stderrText;
    } else {
        b.Error = "p4 changes produced no match";
    }
    return b;
}

std::vector<P4ChangeSummary> ParseChangesForUserOutput(const std::string& stdoutText) {
    std::vector<P4ChangeSummary> changes;
    // Trailing quote optional: long descriptions can be truncated mid-quote by p4.
    static const std::regex re(R"(^Change\s+(\d+)\s+on\s+(\S+)\s+by\s+(\S+)\s+'(.*?)'?\s*$)");
    const std::vector<std::string> lines = SplitLines(stdoutText);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, re)) {
            continue;
        }
        P4ChangeSummary c;
        c.Changelist = m[1].str();
        c.Date = m[2].str();
        c.User = m[3].str();
        StripP4UserDomain(c.User);
        c.Description = m[4].str();
        changes.push_back(std::move(c));
    }
    return changes;
}

} // namespace P4AnnotateParse
