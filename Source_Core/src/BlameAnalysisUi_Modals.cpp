#include "BlameAnalysisUi_Internal.h"

#include "AppController.h"
#include "Logger.h"
#include "StringUtil.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <sstream>
#include <string>
#include <utility>

namespace BlameInternal {

bool ResolveP4UserForAssign(const AppController& app, const std::string& p4User, std::string& accountId,
                            std::string& err) {
    accountId.clear();
    err.clear();
    if (p4User.empty() || p4User == "-") {
        err = "No Perforce user.";
        return false;
    }
    std::vector<TrackerUser> users;
    if (!app.SearchUsersByQuery(p4User, users, err)) {
        return false;
    }
    const std::string pl = ToLowerAsciiCopy(p4User);
    for (const auto& u : users) {
        size_t at = u.EmailAddress.find('@');
        const std::string local = at == std::string::npos ? u.EmailAddress : u.EmailAddress.substr(0, at);
        if (!local.empty() && ToLowerAsciiCopy(local) == pl) {
            accountId = u.AccountId;
            return true;
        }
    }
    if (!users.empty()) {
        accountId = users.front().AccountId;
        return true;
    }
    err = "No Jira user match.";
    return false;
}

std::string BuildAiExport() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        oss << "#" << (i + 1) << " " << r.Parsed.Function << "\n  " << r.PathForP4 << ":" << r.Parsed.LineNumber
            << "\n  User=" << r.Blame.User << " CL=" << r.Blame.Changelist << " Date=" << r.Blame.Date;
        if (r.Blame.Approximate) {
            oss << " [approximate]";
        }
        oss << "\n";
        if (!r.Blame.LineSnippet.empty()) {
            oss << "  " << r.Blame.LineSnippet << "\n";
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

std::string BuildBlameExportCsv() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    std::ostringstream oss;
    oss << "entry,function,path,line,user,changelist,date,approximate,line_snippet\n";
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        oss << (i + 1) << "," << CsvEscape(r.Parsed.Function) << "," << CsvEscape(r.PathForP4) << ","
            << r.Parsed.LineNumber << "," << CsvEscape(r.Blame.User) << "," << CsvEscape(r.Blame.Changelist) << ","
            << CsvEscape(r.Blame.Date) << "," << (r.Blame.Approximate ? "true" : "false") << ","
            << CsvEscape(r.Blame.LineSnippet) << "\n";
    }
    return oss.str();
}

std::string BuildBlameExportJson() {
    std::lock_guard<std::mutex> lk(State().displayMutex);
    nlohmann::json root = nlohmann::json::object();
    root["entries"] = nlohmann::json::array();
    for (size_t i = 0; i < State().displayRows.size(); ++i) {
        const BlameRow& r = State().displayRows[i];
        nlohmann::json entry = nlohmann::json::object();
        entry["entry"] = static_cast<int>(i + 1);
        entry["function"] = r.Parsed.Function;
        entry["path"] = r.PathForP4;
        entry["line"] = r.Parsed.LineNumber;
        entry["user"] = r.Blame.User;
        entry["changelist"] = r.Blame.Changelist;
        entry["date"] = r.Blame.Date;
        entry["approximate"] = r.Blame.Approximate;
        entry["line_snippet"] = r.Blame.LineSnippet;
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

namespace {

std::string ReplaceStringPlaceholder(std::string str, const std::string& placeholder, const std::string& replacement) {
    size_t pos = 0;
    while ((pos = str.find(placeholder, pos)) != std::string::npos) {
        str.replace(pos, placeholder.length(), replacement);
        pos += replacement.length();
    }
    return str;
}

} // namespace

std::string BuildBlameQuickCommentTemplate(const std::string& issueKey, const std::string& templateId,
                                           const BlameRow& row, const std::vector<CommentTemplate>& templates) {
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
            text = "Need repro details for {key} (blame context: {path}:{line}, CL {cl}).";
        } else if (templateId == "need_logs") {
            text =
                "Please attach logs/diagnostics for {key} to continue triage.\nReference: {function} @ {path}:{line}.";
        } else {
            text = "Triage handoff for {key}:\n- Suggested owner: {user}\n- Suspect location: {function} "
                   "({path}:{line})\n- CL: {cl}";
        }
    }

    text = ReplaceStringPlaceholder(text, "{key}", issueKey);
    text = ReplaceStringPlaceholder(text, "{issueKey}", issueKey);
    text = ReplaceStringPlaceholder(text, "{path}", row.PathForP4);
    text = ReplaceStringPlaceholder(text, "{line}", std::to_string(row.Parsed.LineNumber));
    text = ReplaceStringPlaceholder(text, "{cl}", row.Blame.Changelist);
    text = ReplaceStringPlaceholder(text, "{function}", row.Parsed.Function);
    text = ReplaceStringPlaceholder(text, "{user}", row.Blame.User);

    return text;
}

ImVec4 ThCol(const float* c) { return ImVec4(c[0], c[1], c[2], c[3]); }

ImVec4 BlameLinkText(const BlameUiThemeColors& theme) { return ThCol(theme.ImportExisting); }

void PushBlameLinkButtonColors(const BlameUiThemeColors& theme) {
    const ImVec4 link = BlameLinkText(theme);
    ImGui::PushStyleColor(ImGuiCol_Text, link);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(link.x * 0.22f, link.y * 0.28f, link.z * 0.42f, 0.9f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(link.x * 0.34f, link.y * 0.42f, link.z * 0.58f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(link.x * 0.48f, link.y * 0.54f, link.z * 0.72f, 1.f));
}

void PopBlameLinkButtonColors() { ImGui::PopStyleColor(4); }

void PushBlameLinkTextOnly(const BlameUiThemeColors& theme) {
    ImGui::PushStyleColor(ImGuiCol_Text, BlameLinkText(theme));
}

void PopBlameLinkTextOnly() { ImGui::PopStyleColor(1); }

std::string NormalizeDateDisplay(const std::string& raw) {
    if (raw.size() >= 10 && raw[4] == '-' && raw[7] == '-') {
        return raw.substr(0, 4) + "/" + raw.substr(5, 2) + "/" + raw.substr(8, 2);
    }
    return raw;
}

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

void CloseBlameModal(bool* pOpen) {
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
    State().clHoverCl.clear();
    if (State().clHoverFut.valid() &&
        State().clHoverFut.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
        State().detachedClHoverFuts.push_back(State().clHoverFut);
    }
    State().clHoverFut = std::shared_future<P4ChangelistDetails>();
    std::memset(State().callstackBuf, 0, sizeof(State().callstackBuf));
    State().beforeDateIso.clear();
    State().atClBuf[0] = '\0';
    State().lastUiStatus.clear();
    State().pendingSelectEntryIndex = -1;
    State().lastCallstackIssueKey.clear();
    State().blameStreamlinedFromGrid = false;
    State().blamePendingAutoProcess = false;
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
        const bool searchOk = appMut.SearchUsersByQuery(capturedUser, users, qerr);
        std::string bestDisplayName;
        std::string bestEmail;
        std::string bestAccountId;
        if (searchOk && !users.empty()) {
            auto it = std::find_if(users.begin(), users.end(),
                                   [](const TrackerUser& u) { return !u.EmailAddress.empty(); });
            const TrackerUser& best = (it != users.end()) ? *it : users[0];
            bestDisplayName = best.DisplayName;
            bestEmail = best.EmailAddress;
            bestAccountId = best.AccountId;
        }
        std::vector<std::string> groups;
        std::string gerr;
        if (!bestAccountId.empty()) {
            appMut.FetchUserGroupNames(bestAccountId, groups, gerr);
        }
        const bool found = searchOk && !users.empty();
        appMut.mainThreadDispatcher.PostToMainThread(
            [capturedUser, found, bestDisplayName, bestEmail, groups, qerr, gerr]() {
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

void PrepareAssignModal(const AppController& app, const BlameRow& row, const std::string& p4UserCell) {
    State().assignRow = row;
    const std::string& pu = p4UserCell.empty() ? row.Blame.User : p4UserCell;
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
    // SearchUsersByQuery) to a worker. Both share the same blame-row, so a single dispatch
    // sequentialises them.
    State().assignInFlight = true;
    State().assignTitle = "Loading...";
    const std::string capturedUser = pu;
    AppController& appMut = const_cast<AppController&>(app);
    appMut.LaunchBackgroundTask([&appMut, capturedUser]() {
        std::vector<TrackerUser> users;
        std::string err;
        const bool searchOk = appMut.SearchUsersByQuery(capturedUser, users, err);
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
        appMut.mainThreadDispatcher.PostToMainThread(
            [capturedUser, foundAny, hasJiraAccount, accountId, displayName]() {
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

std::string BuildCallstackRowTsv(const BlameRow& row, size_t displayIndex) {
    std::ostringstream o;
    o << (displayIndex + 1) << '\t' << row.Parsed.Function << '\t' << row.PathForP4 << ':' << row.Parsed.LineNumber
      << '\t' << row.Blame.User << '\t' << row.Blame.Changelist << '\t' << row.Blame.Date;
    return o.str();
}

namespace {

std::string SanitizeTsvCell(std::string s) {
    for (char& c : s) {
        if (c == '\t' || c == '\r' || c == '\n') {
            c = ' ';
        }
    }
    return s;
}

} // namespace

std::string BuildAnnotatedRowTsv(const P4AnnotatedLine& ln) {
    std::ostringstream o;
    o << ln.SourceLine << '\t' << SanitizeTsvCell(ln.Changelist) << '\t' << SanitizeTsvCell(ln.User) << '\t'
      << SanitizeTsvCell(ln.Date) << '\t' << SanitizeTsvCell(ln.Code);
    return o.str();
}

void DrawClTooltipAsync(const std::string& cl, const BlameAnalysisConfig& cfg, const BlameUiThemeColors& theme) {
    if (cl.empty()) {
        return;
    }
    if (State().clHoverCl != cl) {
        State().clHoverCl = cl;
        BlameAnalysisConfig cfgCopy = cfg;
        State().clHoverFut = std::async(std::launch::async, [cfgCopy, cl]() {
                                 return State().tooltipClCache.GetOrFetch(cfgCopy, cl);
                             }).share();
    }
    ImGui::BeginTooltip();
    ImGui::TextDisabled("Left-click this changelist cell to open it in p4vc.");
    ImGui::Separator();
    const float wrapX = ImGui::GetCursorPosX() + 600.f;
    ImGui::PushTextWrapPos(wrapX);
    if (!State().clHoverFut.valid()) {
        ImGui::TextUnformatted("Loading CL info...");
    } else if (State().clHoverFut.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
        ImGui::TextUnformatted("Loading CL info...");
    } else {
        try {
            const P4ChangelistDetails d = State().clHoverFut.get();
            if (!d.Error.empty()) {
                ImGui::TextUnformatted(d.Error.c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ThCol(theme.ClTooltipTitle));
                ImGui::Text("CL %s", cl.c_str());
                ImGui::PopStyleColor();
                if (!d.Author.empty()) {
                    ImGui::TextUnformatted(("by " + d.Author).c_str());
                }
                if (!d.Date.empty()) {
                    ImGui::TextUnformatted(d.Date.c_str());
                }
                if (!d.Description.empty()) {
                    ImGui::TextWrapped("%s", d.Description.c_str());
                }
                if (d.Author.empty() && d.Date.empty() && d.Description.empty()) {
                    ImGui::TextUnformatted("(no describe details)");
                }
            }
        } catch (const std::exception& ex) {
            LOG_WARN("Blame tooltip: changelist detail future exception: %s", ex.what());
            ImGui::TextUnformatted("Loading CL info...");
        } catch (...) {
            LOG_WARN("Blame tooltip: changelist detail future unknown exception");
            ImGui::TextUnformatted("Loading CL info...");
        }
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}

} // namespace BlameInternal
