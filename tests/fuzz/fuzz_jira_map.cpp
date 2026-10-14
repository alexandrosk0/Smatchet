// libFuzzer driver for the Jira tracker-response mapping layer —
// smatchet::jira::AppendCachedTicketFromJiraSearchIssue and the /transitions
// parsers FindJiraTransitionId / ParseAvailableTransitionTargets
// (JiraIssueMappingPure.cpp). These consume an ALREADY-PARSED nlohmann::json from
// a Jira /search or /transitions HTTP response and walk it with structural
// assumptions: `fields.<id>` values that may be scalars, objects, arrays or ADF
// documents; the `comment` object's inline `comments[]` vs `total`; the
// `changelog.histories[]` history column; duration-seconds and timetracking
// fields; `issuelinks[]` as a parent fallback; transition `id` / `to.id` as
// string-or-number.
//
// The raw parse is already depth/node-bounded by json_safe::ParseBounded, so this
// driver fuzzes the *consuming* layer: it feeds fuzz bytes through ParseBounded
// (what the production fetcher sees) and drives the pure entry points on the
// resulting DOM. AppendCachedTicketFromJiraSearchIssue is never-throw by contract
// (it returns false on a malformed row), so it is called WITHOUT a catch — an
// escaping exception is a finding. The transitions parsers throw
// nlohmann::json::exception on a mistyped member, which their production callers
// catch, so those calls are wrapped. The comment fetcher is a stub returning
// false, so a row whose inline thread is empty but whose `total` is positive takes
// the no-fetch fallback; the fetched thread would go through the same
// MapJiraIssueComments the inline thread exercises. Logging stays at its default
// level: the LOG_WARN on a rejected row, and the redaction of the fuzz-controlled
// id it carries, are part of the production path under test.
#include "JiraIssueMappingPure.h"

#include "Json/BoundedJsonParse.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace {

// Every column family the mapper special-cases, plus plain and custom fields.
const std::vector<std::string>& SelectedFields() {
    static const std::vector<std::string> kFields = {"summary",
                                                     "status",
                                                     "priority",
                                                     "assignee",
                                                     "reporter",
                                                     "created",
                                                     "updated",
                                                     "labels",
                                                     "components",
                                                     "fixVersions",
                                                     "description",
                                                     "comment",
                                                     "comments",
                                                     "history",
                                                     "watchers",
                                                     "timetracking",
                                                     "timeoriginalestimate",
                                                     "timespent",
                                                     "aggregatetimespent",
                                                     "attachment",
                                                     "issuelinks",
                                                     "parent",
                                                     "duedate",
                                                     "resolution",
                                                     "customfield_10020",
                                                     "customfield_10016"};
    return kFields;
}

void MapSearchIssues(const nlohmann::json& j) {
    const std::function<bool(const std::string&, nlohmann::json&)> noFetch = [](const std::string&, nlohmann::json&) {
        return false;
    };
    std::vector<CachedTicket> results;
    (void)smatchet::jira::AppendCachedTicketFromJiraSearchIssue(j, SelectedFields(), noFetch, results);
    // A /search page wraps the rows in `issues[]`; map each one as the sync loop does.
    if (j.is_object()) {
        const nlohmann::json::const_iterator issues = j.find("issues");
        if (issues != j.end() && issues->is_array()) {
            for (const nlohmann::json& issue : *issues) {
                (void)smatchet::jira::AppendCachedTicketFromJiraSearchIssue(issue, SelectedFields(), noFetch, results);
            }
        }
    }
}

void ParseTransitions(const nlohmann::json& j) {
    // A /transitions response is {"transitions": [...]}; the parsers take the array.
    const nlohmann::json* transitions = &j;
    if (j.is_object()) {
        const nlohmann::json::const_iterator it = j.find("transitions");
        if (it != j.end()) {
            transitions = &*it;
        }
    }
    try {
        (void)smatchet::jira::FindJiraTransitionId(*transitions, "10001", "Done");
    } catch (const std::exception&) {
        // Contractual throw on a mistyped member — not a crash.
    }
    try {
        (void)smatchet::jira::ParseAvailableTransitionTargets(*transitions);
    } catch (const std::exception&) {
        // Contractual throw on a mistyped member — not a crash.
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string err;
    const std::string bytes(reinterpret_cast<const char*>(data), size);
    const nlohmann::json j = smatchet::json_safe::ParseBounded(bytes, err);
    if (!err.empty()) {
        return 0; // malformed / too-deep / oversized never reaches the mappers in production
    }
    MapSearchIssues(j);
    ParseTransitions(j);
    return 0;
}
