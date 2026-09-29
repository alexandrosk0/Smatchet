#include "AnnotateAnalysisUi_Internal.h"

#include "AppController.h"
#include "DataFreshnessCue.h"
#include "Logger.h"
#include "OfflineFirstPure.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"
#include "Tracker/TrackerError.h"
#include "TrackerFieldSchema.h"
#include "Ui/AnnotateAnalysisUi_Modals_detail.h"
#include "Ui/P4ClPreview.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <sstream>
#include <string>
#include <utility>

namespace {

// Pure formatting/matching half — extracted to AnnotateAnalysisUi_Modals_detail.cpp
// (gap-map Tier 5 `_detail` pattern) so it is doctest-covered without this TU's
// ImGui/AppController/State() closure. This shell keeps the I/O: user search, worker
// dispatch, State() locking, ImGui styling.
AnnotateUiPure::AnnotateRowView RowView(const AnnotateRow& row) {
    AnnotateUiPure::AnnotateRowView v;
    v.Function = row.Parsed.Function;
    v.Path = row.PathForP4;
    v.Line = row.Parsed.LineNumber;
    v.User = row.Annotate.User;
    v.Changelist = row.Annotate.Changelist;
    v.Date = row.Annotate.Date;
    return v;
}

// One Perforce-user search, run on a worker and carried to the UI thread by the post-back.
struct P4UserSearch {
    bool Answered = false;    // the tracker answered the search
    bool Unreachable = false; // skipped offline, or the tracker could not answer (transport or service down)
    std::vector<TrackerUser> Users;
    std::string Error;
};

// Worker: search the tracker for `p4User`, keeping the error kind (Quality Pillar 6: an unreachable
// tracker is not "no such user"). Never throws, so the caller's post-back always clears its flag.
P4UserSearch SearchTrackerForP4User(const AppController& app, const std::string& p4User) {
    P4UserSearch out;
    try {
        Result<std::vector<TrackerUser>, TrackerError> found = app.SearchUsersByQueryTyped(p4User);
        if (found.has_value()) {
            out.Answered = true;
            out.Users = std::move(found.value());
        } else {
            // The connectivity probe treats a service outage as offline too (IsOfflineState).
            out.Unreachable = found.error().Kind == TrackerErrorKind::Transport ||
                              found.error().Kind == TrackerErrorKind::ServerError;
            out.Error = found.error().Detail;
        }
    } catch (const std::exception& ex) {
        out.Error = ex.what();
    } catch (...) { // catch-all-ok: reported as the lookup's error
        out.Error = "The user search failed.";
    }
    return out;
}

// Offline no search is sent: the lookup answers from the saved user list right away.
P4UserSearch SkippedBecauseOffline() {
    P4UserSearch out;
    out.Unreachable = true;
    out.Error = "The tracker is offline.";
    return out;
}

// UI thread only (GetAvailableUsers is a UI-thread reference): the saved-roster user for `p4User` when
// the search did not answer.
const TrackerUser* SavedRosterMatch(const AppController& app, const P4UserSearch& search, const std::string& p4User) {
    return search.Answered ? nullptr : AnnotateUiPure::FindRosterUserForP4User(app.GetAvailableUsers(), p4User);
}

AnnotateUiPure::UserLookupOutcome ClassifyLookup(const P4UserSearch& search, const TrackerUser* rosterUser) {
    return AnnotateUiPure::ClassifyUserLookupOutcome(search.Answered, !search.Users.empty(), search.Unreachable,
                                                     rosterUser != nullptr);
}

const char* UnknownOfflineLabel() {
    return SmatchetLocalization::T("annotate.user_unknown_offline", "Unknown (offline)");
}

const char* UnknownLabel() { return SmatchetLocalization::T("annotate.user_unknown", "Unknown"); }

// What the profile worker found: the search, and for a live match the best user and its groups.
struct ProfileLookup {
    P4UserSearch Search;
    std::string Name;
    std::string Email;
    std::vector<std::string> Groups;
    std::string GroupsError;
};

// UI thread: fill the profile dialog from a finished lookup.
void ApplyProfileLookup(const AppController& app, const std::string& p4User, const ProfileLookup& lookup) {
    State().profileInFlight = false;
    const TrackerUser* rosterUser = SavedRosterMatch(app, lookup.Search, p4User);
    switch (ClassifyLookup(lookup.Search, rosterUser)) {
    case AnnotateUiPure::UserLookupOutcome::Found:
        State().profileName = lookup.Name;
        State().profileEmail = lookup.Email;
        State().profileGroups = lookup.Groups;
        if (State().profileGroups.empty() && !lookup.GroupsError.empty()) {
            State().profileErr = lookup.GroupsError;
        }
        return;
    case AnnotateUiPure::UserLookupOutcome::FoundInRoster:
        State().profileName = rosterUser->DisplayName;
        State().profileEmail = rosterUser->EmailAddress;
        State().profileErr =
            DataFreshnessCue::CueText(lookup.Search.Unreachable ? smatchet::offline::DataFreshness::CachedOffline
                                                                : smatchet::offline::DataFreshness::CachedStale);
        return;
    case AnnotateUiPure::UserLookupOutcome::UnknownOffline:
        State().profileName = UnknownOfflineLabel();
        State().profileEmail = p4User;
        State().profileErr = lookup.Search.Error;
        return;
    case AnnotateUiPure::UserLookupOutcome::LookupFailed:
        State().profileName = UnknownLabel();
        State().profileEmail = p4User;
        State().profileErr = lookup.Search.Error;
        return;
    case AnnotateUiPure::UserLookupOutcome::NotFound:
        State().profileName = "Past Employee";
        State().profileEmail = p4User;
        State().profileErr = lookup.Search.Error;
        return;
    }
}

// UI thread: fill the assign dialog from a finished lookup.
void ApplyAssignLookup(const AppController& app, const std::string& p4User, const P4UserSearch& search) {
    State().assignInFlight = false;
    const TrackerUser* rosterUser = SavedRosterMatch(app, search, p4User);
    std::string accountId;
    std::string displayName;
    switch (ClassifyLookup(search, rosterUser)) {
    case AnnotateUiPure::UserLookupOutcome::Found: {
        std::string pickError;
        if (AnnotateUiPure::PickJiraAccountForP4User(search.Users, p4User, accountId, pickError)) {
            const auto it = std::find_if(search.Users.begin(), search.Users.end(),
                                         [&accountId](const TrackerUser& u) { return u.AccountId == accountId; });
            displayName = (it != search.Users.end()) ? it->DisplayName : search.Users.front().DisplayName;
        }
        break;
    }
    case AnnotateUiPure::UserLookupOutcome::FoundInRoster:
        accountId = rosterUser->AccountId;
        displayName = rosterUser->DisplayName + " " + SmatchetLocalization::T("freshness.badge_cached", "(saved)");
        break;
    case AnnotateUiPure::UserLookupOutcome::UnknownOffline:
        State().assignNoAccountReason = SmatchetLocalization::T(
            "annotate.assign_unknown_offline", "The tracker is offline and the saved user list has no match for this "
                                               "Perforce user.");
        State().assignTitle = std::string(UnknownOfflineLabel()) + " (" + p4User + ")";
        return;
    case AnnotateUiPure::UserLookupOutcome::LookupFailed:
        State().assignNoAccountReason = search.Error.empty() ? std::string("The user search failed.") : search.Error;
        State().assignTitle = std::string(UnknownLabel()) + " (" + p4User + ")";
        return;
    case AnnotateUiPure::UserLookupOutcome::NotFound:
        break;
    }
    if (accountId.empty()) {
        State().assignTitle = std::string("Past Employee (") + p4User + ")";
        return;
    }
    State().assignAccountId = accountId;
    State().assignHasJiraAccount = true;
    State().assignTitle = displayName + " (" + p4User + ")";
}

} // namespace

namespace AnnotateInternal {

std::string BuildAiExport() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const AnnotateRow& r = State().displayRows[i];
        oss << "#" << (i + 1) << " " << r.Parsed.Function << "\n  " << r.PathForP4 << ":" << r.Parsed.LineNumber
            << "\n  User=" << r.Annotate.User << " CL=" << r.Annotate.Changelist << " Date=" << r.Annotate.Date;
        if (r.Annotate.Approximate) {
            oss << " [approximate]";
        }
        oss << "\n";
        if (!r.Annotate.LineSnippet.empty()) {
            oss << "  " << r.Annotate.LineSnippet << "\n";
        }
        if (i < State().detailData.size() && !State().detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : State().detailData[i].Lines) {
                if (std::abs(ln.SourceLine - target) <= 3 && !ln.Code.empty()) {
                    oss << "  L" << ln.SourceLine << " [" << ln.Changelist << "] " << ln.User << ": " << ln.Code
                        << "\n";
                }
            }
        }
        oss << "\n";
    }
    return oss.str();
}

std::string CsvEscape(const std::string& s) { return AnnotateUiPure::CsvEscape(s); }

std::string BuildAnnotateExportCsv() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    oss << "entry,function,path,line,user,changelist,date,approximate,line_snippet\n";
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const AnnotateRow& r = State().displayRows[i];
        oss << (i + 1) << "," << CsvEscape(r.Parsed.Function) << "," << CsvEscape(r.PathForP4) << ","
            << r.Parsed.LineNumber << "," << CsvEscape(r.Annotate.User) << "," << CsvEscape(r.Annotate.Changelist)
            << "," << CsvEscape(r.Annotate.Date) << "," << (r.Annotate.Approximate ? "true" : "false") << ","
            << CsvEscape(r.Annotate.LineSnippet) << "\n";
    }
    return oss.str();
}

std::string BuildAnnotateExportJson() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    nlohmann::json root = nlohmann::json::object();
    root["entries"] = nlohmann::json::array();
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const AnnotateRow& r = State().displayRows[i];
        nlohmann::json entry = nlohmann::json::object();
        entry["entry"] = static_cast<int>(i + 1);
        entry["function"] = r.Parsed.Function;
        entry["path"] = r.PathForP4;
        entry["line"] = r.Parsed.LineNumber;
        entry["user"] = r.Annotate.User;
        entry["changelist"] = r.Annotate.Changelist;
        entry["date"] = r.Annotate.Date;
        entry["approximate"] = r.Annotate.Approximate;
        entry["line_snippet"] = r.Annotate.LineSnippet;
        entry["nearby_lines"] = nlohmann::json::array();
        if (i < State().detailData.size() && !State().detailData[i].Lines.empty()) {
            const int target = r.Parsed.LineNumber;
            for (const auto& ln : State().detailData[i].Lines) {
                if (std::abs(ln.SourceLine - target) > 3 || ln.Code.empty()) {
                    continue;
                }
                entry["nearby_lines"].push_back(nlohmann::json{
                    {"line", ln.SourceLine}, {"changelist", ln.Changelist}, {"user", ln.User}, {"code", ln.Code}});
            }
        }
        root["entries"].push_back(std::move(entry));
    }
    return root.dump(2);
}

std::string BuildAnnotateQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                              const AnnotateRow& row, const std::vector<CommentTemplate>& templates) {
    return AnnotateUiPure::BuildQuickCommentText(issueKey, templateId, RowView(row), templates);
}

ImVec4 ThCol(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

ImVec4 AnnotateLinkText(const AnnotateUiThemeColors& theme) { return ThCol(theme.ImportExisting); }

void PushAnnotateLinkButtonColors(const AnnotateUiThemeColors& theme) {
    const ImVec4 link = AnnotateLinkText(theme);
    ImGui::PushStyleColor(ImGuiCol_Text, link);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(link.x * 0.22f, link.y * 0.28f, link.z * 0.42f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(link.x * 0.34f, link.y * 0.42f, link.z * 0.58f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(link.x * 0.48f, link.y * 0.54f, link.z * 0.72f, 1.f));
}

void PopAnnotateLinkButtonColors() { ImGui::PopStyleColor(4); }

void PushAnnotateLinkTextOnly(const AnnotateUiThemeColors& theme) {
    ImGui::PushStyleColor(ImGuiCol_Text, AnnotateLinkText(theme));
}

void PopAnnotateLinkTextOnly() { ImGui::PopStyleColor(1); }

std::string NormalizeDateDisplay(const std::string& raw) { return AnnotateUiPure::NormalizeDateDisplay(raw); }

std::string ShortenPathForDisplay(const std::string& path, float maxWidthPx) {
    if (path.empty() || maxWidthPx <= 8.f) {
        return path;
    }
    if (ImGui::CalcTextSize(path.c_str()).x <= maxWidthPx) {
        return path;
    }
    const std::string ell = "...";
    const float ellW = ImGui::CalcTextSize(ell.c_str()).x;
    if (maxWidthPx <= ellW + 4.f) {
        return ell;
    }
    const int n = static_cast<int>(path.size());
    std::string best = ell;
    for (int use = n; use >= 2; --use) {
        for (int pre = 1; pre < use; ++pre) {
            const int suf = use - pre;
            std::string trial =
                path.substr(0, static_cast<size_t>(pre)) + ell + path.substr(static_cast<size_t>(n - suf));
            if (ImGui::CalcTextSize(trial.c_str()).x <= maxWidthPx) {
                return trial;
            }
        }
    }
    return best;
}

void CloseAnnotateModal(bool* pOpen) {
    if (!pOpen) {
        return;
    }
    State().worker.Cancel = true;
    if (State().worker.Thread.joinable()) {
        State().worker.Thread.join();
    }
    {
        std::lock_guard<std::mutex> lk(State().worker.Mutex);
        State().worker.Rows.clear();
        State().worker.Progress = 0;
        State().worker.Total = 0;
    }
    {
        std::lock_guard<std::mutex> lk(State().displayMutex);
        for (auto& fut : State().detailFuts) {
            if (fut.valid() && fut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                State().detachedDetailFuts.push_back(fut);
            }
        }
        State().displayRows.clear();
        State().detailFuts.clear();
        State().detailPhase.clear();
        State().detailData.clear();
        State().detailScrolled.clear();
    }
    P4ClPreview::DetachInFlight();
    std::memset(State().callstackBuf, 0, sizeof(State().callstackBuf));
    State().beforeDateIso.clear();
    State().atClBuf[0] = '\0';
    State().lastUiStatus.clear();
    State().pendingSelectEntryIndex = -1;
    State().lastCallstackIssueKey.clear();
    State().annotateStreamlinedFromGrid = false;
    State().annotatePendingAutoProcess = false;
    State().annotateHidePreservesState = false;
    State().showRaw = false;
    *pOpen = false;
}

void OpenTrackerUserProfileForP4User(const AppController& app, const std::string& p4User) {
    State().openProfileModal = true;
    State().profileErr.clear();
    State().profileName.clear();
    State().profileEmail.clear();
    State().profileGroups.clear();
    if (p4User.empty() || p4User == "-" || p4User == "...") {
        State().profileName = "Past Employee";
        return;
    }
    if (State().profileInFlight) {
        return;
    }
    if (app.IsTrackerOffline()) {
        ProfileLookup offline;
        offline.Search = SkippedBecauseOffline();
        ApplyProfileLookup(app, p4User, offline);
        return;
    }
    // Pillar 2 — finding #5: dispatch the back-to-back user search + FetchUserGroupNames pair to a
    // worker. The modal renders "Loading..." until the post-back populates fields.
    State().profileInFlight = true;
    State().profileName = "Loading...";
    const std::string capturedUser = p4User;
    AppController& appMut = const_cast<AppController&>(app);
    try {
        appMut.LaunchBackgroundTask([&appMut, capturedUser]() {
            ProfileLookup lookup;
            lookup.Search = SearchTrackerForP4User(appMut, capturedUser);
            if (lookup.Search.Answered && !lookup.Search.Users.empty()) {
                const std::vector<TrackerUser>& users = lookup.Search.Users;
                auto it = std::find_if(users.begin(), users.end(),
                                       [](const TrackerUser& u) { return !u.EmailAddress.empty(); });
                const TrackerUser& best = (it != users.end()) ? *it : users[0];
                lookup.Name = best.DisplayName;
                lookup.Email = best.EmailAddress;
                if (!best.AccountId.empty()) {
                    Result<std::vector<std::string>> r = appMut.FetchUserGroupNames(best.AccountId);
                    if (r.has_value()) {
                        lookup.Groups = std::move(r.value());
                    } else {
                        // An empty Detail must not render as "no groups, no error" — the post-back only
                        // shows the message when it is non-empty (Issue #2064).
                        lookup.GroupsError = AnnotateUiPure::GroupLookupErrorMessage(r.error());
                    }
                }
            }
            appMut.PostToMainThread([&appMut, capturedUser, lookup]() {
                if (!HasLiveStateInstance()) {
                    return;
                }
                ApplyProfileLookup(appMut, capturedUser, lookup);
            });
        });
    } catch (const std::exception& ex) {
        LOG_WARN("Annotate UI: the user profile lookup did not start: %s", ex.what());
        ProfileLookup failed;
        failed.Search.Error = ex.what();
        ApplyProfileLookup(app, p4User, failed);
    }
}

void PrepareAssignModal(const AppController& app, const AnnotateRow& row, const std::string& p4UserCell) {
    State().assignRow = row;
    const std::string pu = p4UserCell.empty() ? row.Annotate.User : p4UserCell;
    State().assignAccountId.clear();
    State().assignHasJiraAccount = false;
    State().assignNoAccountReason.clear();
    if (pu.empty() || pu == "-" || pu == "...") {
        State().assignTitle = "Past Employee";
        return;
    }
    if (State().assignInFlight) {
        return;
    }
    if (app.IsTrackerOffline()) {
        ApplyAssignLookup(app, pu, SkippedBecauseOffline());
        return;
    }
    // Pillar 2 — finding #5/#6: the user search runs on a worker; its result both titles the dialog and
    // picks the account to assign (one request, not a second search to resolve the account).
    State().assignInFlight = true;
    State().assignTitle = "Loading...";
    AppController& appMut = const_cast<AppController&>(app);
    try {
        appMut.LaunchBackgroundTask([&appMut, pu]() {
            const P4UserSearch search = SearchTrackerForP4User(appMut, pu);
            appMut.PostToMainThread([&appMut, pu, search]() {
                if (!HasLiveStateInstance()) {
                    return;
                }
                ApplyAssignLookup(appMut, pu, search);
            });
        });
    } catch (const std::exception& ex) {
        LOG_WARN("Annotate UI: the assign user lookup did not start: %s", ex.what());
        P4UserSearch failed;
        failed.Error = ex.what();
        ApplyAssignLookup(app, pu, failed);
    }
}

std::string BuildCallstackRowTsv(const AnnotateRow& row, size_t displayIndex) {
    return AnnotateUiPure::BuildCallstackRowTsv(RowView(row), displayIndex);
}

std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln) { return AnnotateUiPure::BuildAnnotatedRowTsv(ln); }

} // namespace AnnotateInternal
