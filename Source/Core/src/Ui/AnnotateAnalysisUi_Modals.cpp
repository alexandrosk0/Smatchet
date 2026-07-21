#include "AnnotateAnalysisUi_Internal.h"

#include "AppController.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"
#include "Ui/AnnotateAnalysisUi_Modals_detail.h"
#include "Ui/P4ClPreview.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
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

} // namespace

namespace AnnotateInternal {

bool ResolveP4UserForAssign(const AppController& app, const std::string& p4User, std::string& accountId,
                            std::string& err) {
    accountId.clear();
    err.clear();
    if (p4User.empty() || p4User == "-") {
        err = "No Perforce user.";
        return false;
    }
    Result<std::vector<TrackerUser>> usersResult = app.SearchUsersByQuery(p4User);
    if (!usersResult.has_value()) {
        err = usersResult.error();
        return false;
    }
    return AnnotateUiPure::PickJiraAccountForP4User(usersResult.value(), p4User, accountId, err);
}

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
    // Pillar 2 — finding #5: dispatch the back-to-back SearchUsersByQuery + FetchUserGroupNames
    // pair to a worker. The modal renders "Loading..." until the post-back populates fields.
    State().profileInFlight = true;
    State().profileName = "Loading...";
    const std::string capturedUser = p4User;
    AppController& appMut = const_cast<AppController&>(app);
    appMut.LaunchBackgroundTask([&appMut, capturedUser]() {
        std::vector<TrackerUser> users;
        std::string qerr;
        bool searchOk = false;
        UnpackResult(appMut.SearchUsersByQuery(capturedUser), searchOk, users, qerr);
        std::string bestDisplayName;
        std::string bestEmail;
        std::string bestAccountId;
        if (searchOk && !users.empty()) {
            auto it =
                std::find_if(users.begin(), users.end(), [](const TrackerUser& u) { return !u.EmailAddress.empty(); });
            const TrackerUser& best = (it != users.end()) ? *it : users[0];
            bestDisplayName = best.DisplayName;
            bestEmail = best.EmailAddress;
            bestAccountId = best.AccountId;
        }
        std::vector<std::string> groups;
        std::string gerr;
        if (!bestAccountId.empty()) {
            Result<std::vector<std::string>> r = appMut.FetchUserGroupNames(bestAccountId);
            if (r.has_value()) {
                groups = std::move(r.value());
            } else {
                gerr = r.error();
            }
        }
        const bool found = searchOk && !users.empty();
        appMut.PostToMainThread([capturedUser, found, bestDisplayName, bestEmail, groups, qerr, gerr]() {
            if (!HasLiveStateInstance()) {
                return;
            }
            State().profileInFlight = false;
            if (!found) {
                State().profileName = "Past Employee";
                State().profileEmail = capturedUser;
                if (!qerr.empty()) {
                    State().profileErr = qerr;
                }
                return;
            }
            State().profileName = bestDisplayName;
            State().profileEmail = bestEmail;
            State().profileGroups = groups;
            if (State().profileGroups.empty() && !gerr.empty()) {
                State().profileErr = gerr;
            }
        });
    });
}

void PrepareAssignModal(const AppController& app, const AnnotateRow& row, const std::string& p4UserCell) {
    State().assignRow = row;
    const std::string& pu = p4UserCell.empty() ? row.Annotate.User : p4UserCell;
    State().assignAccountId.clear();
    State().assignHasJiraAccount = false;
    if (pu.empty() || pu == "-" || pu == "...") {
        State().assignTitle = "Past Employee";
        return;
    }
    if (State().assignInFlight) {
        return;
    }
    // Pillar 2 — finding #5/#6: dispatch SearchUsersByQuery (and ResolveP4UserForAssign's own
    // SearchUsersByQuery) to a worker. Both share the same annotate-row, so a single dispatch
    // sequentialises them.
    State().assignInFlight = true;
    State().assignTitle = "Loading...";
    const std::string capturedUser = pu;
    AppController& appMut = const_cast<AppController&>(app);
    appMut.LaunchBackgroundTask([&appMut, capturedUser]() {
        std::vector<TrackerUser> users;
        std::string err;
        bool searchOk = false;
        UnpackResult(appMut.SearchUsersByQuery(capturedUser), searchOk, users, err);
        std::string accountId;
        std::string resolveErr;
        bool hasJiraAccount = false;
        if (searchOk && !users.empty()) {
            if (ResolveP4UserForAssign(appMut, capturedUser, accountId, resolveErr) && !accountId.empty()) {
                hasJiraAccount = true;
            }
        }
        std::string displayName;
        if (hasJiraAccount) {
            auto it = std::find_if(users.begin(), users.end(),
                                   [&accountId](const TrackerUser& u) { return u.AccountId == accountId; });
            displayName = (it != users.end()) ? it->DisplayName : users[0].DisplayName;
        }
        const bool foundAny = searchOk && !users.empty();
        appMut.PostToMainThread([capturedUser, foundAny, hasJiraAccount, accountId, displayName]() {
            if (!HasLiveStateInstance()) {
                return;
            }
            State().assignInFlight = false;
            if (!foundAny) {
                State().assignTitle = std::string("Past Employee (") + capturedUser + ")";
                return;
            }
            if (hasJiraAccount) {
                State().assignAccountId = accountId;
                State().assignHasJiraAccount = true;
                State().assignTitle = displayName + " (" + capturedUser + ")";
            } else {
                State().assignTitle = std::string("Past Employee (") + capturedUser + ")";
            }
        });
    });
}

std::string BuildCallstackRowTsv(const AnnotateRow& row, size_t displayIndex) {
    return AnnotateUiPure::BuildCallstackRowTsv(RowView(row), displayIndex);
}

std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln) { return AnnotateUiPure::BuildAnnotatedRowTsv(ln); }

} // namespace AnnotateInternal
