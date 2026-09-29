#pragma once

// Pure (ImGui-free, AppController-free, State()-free) formatting/matching helpers extracted
// from AnnotateAnalysisUi_Modals.cpp (gap-map Tier 5 `_detail` pattern; sibling of
// AnnotateAnalysisUi_Window_detail.h). Covers the export/clipboard cell escaping, the
// quick-comment template expansion, and the P4-user -> Jira-account selection that backs
// the assign modal. Element types are forward-declared so this header stays light; the
// .cpp includes the full definitions.

#include <cstddef>
#include <string>
#include <vector>

struct CommentTemplate; // Config/ConfigManager.h
struct P4AnnotatedLine; // P4Annotate.h
struct TrackerUser;     // Tracker/TrackerFieldSchema.h

namespace AnnotateUiPure {

/** The per-row fields the modal formatters consume, decoupled from the private AnnotateRow
 *  (which lives in the internal header behind the ImGui alias define). */
struct AnnotateRowView {
    std::string Function;
    std::string Path;
    int Line = 0;
    std::string User;
    std::string Changelist;
    std::string Date;
};

/** RFC-4180-style CSV cell: quoted (with doubled inner quotes) only when the value carries
 *  a comma, quote, or newline. */
std::string CsvEscape(const std::string& s);

/** "YYYY-MM-DD..." -> "YYYY/MM/DD"; anything else passes through verbatim. */
std::string NormalizeDateDisplay(const std::string& raw);

/** Tabs/newlines flattened to spaces so a code snippet cannot break the TSV row shape. */
std::string SanitizeTsvCell(std::string s);

/** Every occurrence of `placeholder` replaced with `replacement` (no recursion into the
 *  replacement text). */
std::string ReplaceStringPlaceholder(std::string str, const std::string& placeholder, const std::string& replacement);

/** Quick-comment text for the annotate triage modal: the user-configured template with
 *  matching `templateId` wins, else the built-in need_repro / need_logs / handoff fallback;
 *  {key}/{issueKey}/{path}/{line}/{cl}/{function}/{user} placeholders are then expanded. */
std::string BuildQuickCommentText(const std::string& issueKey, const std::string& templateId,
                                  const AnnotateRowView& row, const std::vector<CommentTemplate>& templates);

/** One clipboard TSV row for a callstack entry (1-based display index first). */
std::string BuildCallstackRowTsv(const AnnotateRowView& row, std::size_t displayIndex);

/** One clipboard TSV row for an annotated source line (cells sanitized). */
std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln);

/** Jira account for a Perforce user from a user-search result: prefer the exact
 *  email-local-part match (case-insensitive), else the first result; false with
 *  "No Jira user match." when the search returned nothing. */
bool PickJiraAccountForP4User(const std::vector<TrackerUser>& users, const std::string& p4User,
                              std::string& outAccountId, std::string& outError);

/** How a Perforce-user lookup ended, for the profile and assign dialogs (Quality Pillar 6). */
enum class UserLookupOutcome : unsigned char {
    Found,          ///< the tracker's user search matched
    FoundInRoster,  ///< the search did not answer; the saved user list matched
    UnknownOffline, ///< the tracker was unreachable and the saved list has no match — never "Past Employee"
    LookupFailed,   ///< the search was refused or never ran (auth, local failure) and the saved list has no
                    ///< match: unknown, with the error — never "Past Employee"
    NotFound,       ///< the tracker answered the search with no match: "Past Employee"
};

/** `searchOk`: the user search answered; `liveMatch`: it returned users; `trackerUnreachable`: the search was
 *  skipped offline or the tracker could not answer (transport failure or service outage); `rosterMatch`:
 *  the saved user list matched. A live answer always wins over the saved list, and only a live answer
 *  with no match reads as NotFound. */
UserLookupOutcome ClassifyUserLookupOutcome(bool searchOk, bool liveMatch, bool trackerUnreachable, bool rosterMatch);

/** The saved-roster user for a Perforce login: an exact, case-insensitive match of the email's local part,
 *  else of the display name. Null when neither matches — never a guess (PickJiraAccountForP4User, by
 *  contrast, falls back to the first search result, which on a whole roster would be a stranger). */
const TrackerUser* FindRosterUserForP4User(const std::vector<TrackerUser>& roster, const std::string& p4User);

/** User-facing message for a failed group lookup. A backend `Result` error can legitimately
 *  carry an empty Detail (CollaborationPreconditionPure maps an empty-Detail failure to
 *  `Err("")`), and the profile modal only shows a message when it is non-empty — so passing the
 *  raw detail through renders the failure as an empty group list with no error at all
 *  (Issue #2064). Never returns an empty string. */
std::string GroupLookupErrorMessage(const std::string& detail);

} // namespace AnnotateUiPure
