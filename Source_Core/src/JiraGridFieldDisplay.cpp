#include "JiraGridFieldDisplay.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "SmatchetFieldRender.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <future>
#include <string>
#include <vector>

namespace {

std::string TrimCopy(const std::string& in) {
    size_t start = 0;
    size_t end = in.size();
    while (start < end &&
           (in[start] == ' ' || in[start] == '\t' || in[start] == '\n' || in[start] == '\r')) {
        ++start;
    }
    while (end > start &&
           (in[end - 1] == ' ' || in[end - 1] == '\t' || in[end - 1] == '\n' || in[end - 1] == '\r')) {
        --end;
    }
    return in.substr(start, end - start);
}

struct AttachmentInfo {
    std::string filename;
    std::string url;
    std::string mimeType;
};

bool ParseAttachmentsFieldValue(const std::string& currentValue, std::vector<AttachmentInfo>& outAttachments) {
    outAttachments.clear();
    const std::string trimmed = TrimCopy(currentValue);
    if (trimmed.empty()) {
        return false;
    }

    nlohmann::json j;
    try {
        if (!trimmed.empty() && trimmed.front() == '[') {
            j = nlohmann::json::parse(trimmed);
        } else {
            j = nlohmann::json::parse("[" + trimmed + "]");
        }
    } catch (...) {
        return false;
    }

    if (!j.is_array()) {
        return false;
    }

    for (const auto& item : j) {
        if (!item.is_object()) {
            continue;
        }

        AttachmentInfo info;
        info.filename = item.value("filename", std::string());
        info.mimeType = item.value("mimeType", std::string());

        if (item.contains("content")) {
            const auto& content = item["content"];
            if (content.is_string()) {
                info.url = content.get<std::string>();
            } else if (content.is_object()) {
                info.url = content.value("url", std::string());
                if (info.url.empty()) {
                    info.url = content.value("self", std::string());
                }
            }
        }

        if (info.url.empty()) {
            info.url = item.value("self", std::string());
        }

        if (!info.filename.empty() || !info.url.empty()) {
            outAttachments.push_back(std::move(info));
        }
    }
    return !outAttachments.empty();
}

bool ParseWatchersFieldJson(const std::string& raw,
                            int& outWatchCount,
                            bool& outIsWatching,
                            std::string& outSelfUrl) {
    outWatchCount = 0;
    outIsWatching = false;
    outSelfUrl.clear();
    const std::string trimmed = TrimCopy(raw);
    if (trimmed.empty()) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(trimmed);
        if (j.is_string()) {
            j = nlohmann::json::parse(j.get<std::string>());
        }
        if (!j.is_object()) {
            return false;
        }
        if (j.contains("isWatching")) {
            const auto& iw = j["isWatching"];
            if (iw.is_boolean()) {
                outIsWatching = iw.get<bool>();
            } else if (iw.is_number_integer()) {
                outIsWatching = (iw.get<long long>() != 0);
            }
        }
        if (j.contains("watchCount")) {
            const auto& wc = j["watchCount"];
            if (wc.is_number_integer()) {
                outWatchCount = static_cast<int>(wc.get<long long>());
            } else if (wc.is_number_unsigned()) {
                outWatchCount = static_cast<int>(wc.get<unsigned long long>());
            } else if (wc.is_number_float()) {
                outWatchCount = static_cast<int>(wc.get<double>());
            } else if (wc.is_string()) {
                try {
                    outWatchCount = std::stoi(wc.get<std::string>());
                } catch (...) {}
            }
        }
        if (outWatchCount < 0) {
            outWatchCount = 0;
        }
        outSelfUrl = j.value("self", std::string());
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseVotesFieldJson(const std::string& raw,
                         int& outVoteCount,
                         bool& outHasVoted,
                         std::string& outSelfUrl) {
    outVoteCount = 0;
    outHasVoted = false;
    outSelfUrl.clear();
    const std::string trimmed = TrimCopy(raw);
    if (trimmed.empty()) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(trimmed);
        if (j.is_string()) {
            j = nlohmann::json::parse(j.get<std::string>());
        }
        if (!j.is_object()) {
            return false;
        }
        if (j.contains("hasVoted")) {
            const auto& hv = j["hasVoted"];
            if (hv.is_boolean()) {
                outHasVoted = hv.get<bool>();
            } else if (hv.is_number_integer()) {
                outHasVoted = (hv.get<long long>() != 0);
            }
        }
        if (j.contains("votes")) {
            const auto& vc = j["votes"];
            if (vc.is_number_integer()) {
                outVoteCount = static_cast<int>(vc.get<long long>());
            } else if (vc.is_number_unsigned()) {
                outVoteCount = static_cast<int>(vc.get<unsigned long long>());
            } else if (vc.is_number_float()) {
                outVoteCount = static_cast<int>(vc.get<double>());
            } else if (vc.is_string()) {
                try {
                    outVoteCount = std::stoi(vc.get<std::string>());
                } catch (...) {}
            }
        }
        if (outVoteCount < 0) {
            outVoteCount = 0;
        }
        outSelfUrl = j.value("self", std::string());
        return true;
    } catch (...) {
        return false;
    }
}

struct WorklogFieldSummary {
    int Total = 0;
    int StartAt = 0;
    int MaxResults = 0;
    long long SumSecondsOnPage = 0;
    int WorklogsOnPage = 0;
    struct Entry {
        std::string Author;
        std::string StartedIso;
        std::string TimeSpentText;
        long long TimeSpentSeconds = 0;
    };
    std::vector<Entry> Entries;
};

int ParseJsonIntFieldLoose(const nlohmann::json& j, const char* key, int fallback = 0) {
    if (!j.contains(key)) {
        return fallback;
    }
    const auto& v = j[key];
    if (v.is_number_integer()) {
        return static_cast<int>(v.get<long long>());
    }
    if (v.is_number_unsigned()) {
        return static_cast<int>(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        return static_cast<int>(v.get<double>());
    }
    if (v.is_string()) {
        try {
            return std::stoi(v.get<std::string>());
        } catch (...) {}
    }
    return fallback;
}

long long ParseJsonLongLongFieldLoose(const nlohmann::json& w, const char* key) {
    if (!w.contains(key)) {
        return 0;
    }
    const auto& v = w[key];
    if (v.is_number_integer()) {
        return v.get<long long>();
    }
    if (v.is_number_unsigned()) {
        return static_cast<long long>(v.get<unsigned long long>());
    }
    if (v.is_number_float()) {
        return static_cast<long long>(v.get<double>());
    }
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {}
    }
    return 0;
}

bool ParseWorklogFieldJson(const std::string& raw, WorklogFieldSummary& out) {
    out = WorklogFieldSummary{};
    const std::string trimmed = TrimCopy(raw);
    if (trimmed.empty()) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(trimmed);
        if (j.is_string()) {
            j = nlohmann::json::parse(j.get<std::string>());
        }
        if (!j.is_object()) {
            return false;
        }
        if (!j.contains("worklogs") || !j["worklogs"].is_array()) {
            return false;
        }
        out.Total = ParseJsonIntFieldLoose(j, "total", 0);
        if (out.Total < 0) {
            out.Total = 0;
        }
        out.StartAt = ParseJsonIntFieldLoose(j, "startAt", 0);
        if (out.StartAt < 0) {
            out.StartAt = 0;
        }
        out.MaxResults = ParseJsonIntFieldLoose(j, "maxResults", 0);
        const auto& arr = j["worklogs"];
        out.WorklogsOnPage = static_cast<int>(arr.size());
        for (const auto& w : arr) {
            if (!w.is_object()) {
                continue;
            }
            WorklogFieldSummary::Entry e;
            if (w.contains("author") && w["author"].is_object()) {
                e.Author = w["author"].value("displayName", std::string());
                if (e.Author.empty()) {
                    e.Author = w["author"].value("accountId", std::string());
                }
            }
            if (e.Author.empty() && w.contains("updateAuthor") && w["updateAuthor"].is_object()) {
                e.Author = w["updateAuthor"].value("displayName", std::string());
            }
            e.StartedIso = w.value("started", std::string());
            e.TimeSpentText = w.value("timeSpent", std::string());
            e.TimeSpentSeconds = ParseJsonLongLongFieldLoose(w, "timeSpentSeconds");
            if (e.TimeSpentSeconds > 0) {
                out.SumSecondsOnPage += e.TimeSpentSeconds;
            }
            out.Entries.push_back(std::move(e));
        }
        return true;
    } catch (...) {
        return false;
    }
}

bool ColumnIdEqualsLower(const std::string& id, const char* lowerAscii) {
    std::string lower(id.size(), '\0');
    std::transform(id.begin(), id.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower == lowerAscii;
}

bool TryRenderIssueRestrictionCell(const std::string& currentValue,
                                   float availWidth,
                                   bool tooltipsEnabled) {
    const std::string trimmed = TrimCopy(currentValue);
    if (trimmed.empty()) {
        return false;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(trimmed);
        if (j.is_string()) {
            j = nlohmann::json::parse(j.get<std::string>());
        }
        if (!j.is_object()) {
            return false;
        }
        if (!j.contains("issuerestrictions") || !j["issuerestrictions"].is_object()) {
            return false;
        }
        const auto& rest = j["issuerestrictions"];
        bool shouldDisplay = true;
        if (j.contains("shouldDisplay")) {
            const auto& sd = j["shouldDisplay"];
            if (sd.is_boolean()) {
                shouldDisplay = sd.get<bool>();
            } else if (sd.is_number_integer()) {
                shouldDisplay = (sd.get<long long>() != 0);
            } else if (sd.is_number_unsigned()) {
                shouldDisplay = (sd.get<unsigned long long>() != 0);
            } else if (sd.is_number_float()) {
                shouldDisplay = (sd.get<double>() != 0.0);
            } else if (sd.is_string()) {
                const std::string s = sd.get<std::string>();
                shouldDisplay = !(s == "false" || s == "0" || s == "no" || s == "off" || s.empty());
            }
        }
        std::string display;
        if (!shouldDisplay) {
            display = "Not displayed";
        } else {
            const int n = static_cast<int>(rest.size());
            if (n == 0) {
                display = "No restrictions";
            } else {
                display = std::to_string(n) + (n == 1 ? " restriction" : " restrictions");
            }
        }
        const std::string* tip = tooltipsEnabled ? &currentValue : nullptr;
        RenderClippedFieldText(display, availWidth, tooltipsEnabled, true, tip);
        return true;
    } catch (...) {
        return false;
    }
}

bool TryRenderProgressBarCell(const std::string& currentValue, float availWidth) {
    const std::string trimmed = TrimCopy(currentValue);
    if (trimmed.empty()) {
        ImGui::ProgressBar(0.0f, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
        return true;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(trimmed);
        if (j.is_string()) {
            j = nlohmann::json::parse(j.get<std::string>());
        }
        if (!j.is_object()) {
            return false;
        }
        if (!j.contains("progress") || !j.contains("total")) {
            return false;
        }
        const int p = ParseJsonIntFieldLoose(j, "progress", 0);
        const int t = ParseJsonIntFieldLoose(j, "total", 0);
        const float frac = (t > 0) ? (static_cast<float>(p) / static_cast<float>(t)) : 0.0f;
        ImGui::ProgressBar(frac, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace

bool JiraGridFieldDisplay::IsWatchersColumnId(const std::string& id) {
    std::string lower(id.size(), '\0');
    std::transform(id.begin(), id.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return lower == "watchers" || lower == "watches";
}

bool JiraGridFieldDisplay::IsVotesColumnId(const std::string& id) {
    return ColumnIdEqualsLower(id, "votes");
}

bool JiraGridFieldDisplay::IsWorklogColumnId(const std::string& id) {
    return ColumnIdEqualsLower(id, "worklog");
}

bool JiraGridFieldDisplay::IsProgressStyleColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "aggregateprogress");
}

bool JiraGridFieldDisplay::IsProgressDisplayField(const JiraField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsProgressStyleColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "progress") ||
           ColumnIdEqualsLower(field->Name, "aggregateprogress") ||
           ColumnIdEqualsLower(field->Name, "aggregate progress");
}

bool JiraGridFieldDisplay::TryRenderProgressJsonField(const std::string& currentValue, float availWidth) {
    return TryRenderProgressBarCell(currentValue, availWidth);
}

bool JiraGridFieldDisplay::IsIssueRestrictionColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "issuerestriction");
}

bool JiraGridFieldDisplay::IsIssueRestrictionField(const JiraField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsIssueRestrictionColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "issue restriction") ||
           ColumnIdEqualsLower(field->Name, "issue restrictions");
}

bool JiraGridFieldDisplay::TryRenderIssueRestrictionField(const std::string& currentValue,
                                                          float availWidth,
                                                          bool tooltipsEnabled) {
    return TryRenderIssueRestrictionCell(currentValue, availWidth, tooltipsEnabled);
}

void JiraGridFieldDisplay::RenderAttachmentsField(AppController& app,
                                                  const std::string& currentValue,
                                                  float availWidth,
                                                  bool tooltipsEnabled) {
    (void)availWidth;
    (void)tooltipsEnabled;

    std::vector<AttachmentInfo> attachments;
    if (!ParseAttachmentsFieldValue(currentValue, attachments)) {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    const AttachmentInfo& first = attachments.front();
    const std::string firstName = first.filename.empty() ? std::string("Attachment") : first.filename;
    const int extraCount = static_cast<int>(attachments.size()) - 1;

    std::string display = firstName;
    if (extraCount > 0) {
        display += " +" + std::to_string(extraCount);
    }

    ImGui::TextUnformatted(display.c_str());
    if (ImGui::IsItemHovered()) {
        if (!first.mimeType.empty()) {
            ImGui::SetTooltip("MIME: %s", first.mimeType.c_str());
        } else if (extraCount > 0) {
            std::string tip;
            tip += "Attachments:\n";
            for (const auto& a : attachments) {
                const std::string name = a.filename.empty() ? std::string("Attachment") : a.filename;
                tip += " - " + name + "\n";
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    }

    if (!first.url.empty()) {
        ImGui::SameLine();
        const std::string openId = "##att_open_first";
        if (ImGui::SmallButton(openId.c_str())) {
            app.OpenAttachment(first.url, firstName, first.mimeType);
        }

        ImGui::SameLine();
        const std::string copyId = "##att_copy_first";
        if (ImGui::SmallButton(copyId.c_str())) {
            ImGui::SetClipboardText(first.url.c_str());
        }
    }
}

void JiraGridFieldDisplay::RenderWatchersField(const std::string& issueKey,
                                               const std::string& currentValue,
                                               float availWidth,
                                               bool tooltipsEnabled,
                                               JiraGridFieldAsyncState& async) {
    int watchCount = 0;
    bool isWatching = false;
    std::string selfUrl;
    const bool parsed = ParseWatchersFieldJson(currentValue, watchCount, isWatching, selfUrl);
    if (parsed) {
        std::string line;
        if (watchCount <= 0) {
            line = isWatching ? "You watch" : "No watchers";
        } else if (watchCount == 1) {
            line = isWatching ? "1 watcher (you)" : "1 watcher";
        } else {
            line = isWatching ? (std::to_string(watchCount) + " watchers (incl. you)")
                              : (std::to_string(watchCount) + " watchers");
        }

        ImGui::TextUnformatted(line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            std::string tip = "Watchers\n";
            tip += "Count: " + std::to_string(watchCount) + "\n";
            tip += isWatching ? "You are watching this issue.\n" : "You are not watching.\n";
            if (!selfUrl.empty()) {
                tip += "\n";
                tip += selfUrl;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##watch_" + issueKey;
    const bool watchersBusy =
        async.watchersLoadInProgress && async.watchersFuture.valid() &&
        async.watchersFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (watchersBusy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(loadBtn.c_str())) {
        async.watchersPopupIssueKey = issueKey;
        async.watchersPanelOpen = true;
        async.watchersLoadInProgress = true;
        async.watchersLoadedList.clear();
        async.watchersLoadedError.clear();
        async.watchersFuture = std::async(std::launch::async, [issueKey]() {
            WatchersLoadResult r;
            JiraClient client;
            const JiraConfig cfg = ConfigManager::Load();
            std::string err;
            if (client.FetchIssueWatchers(cfg, issueKey, r.Watchers, err)) {
                r.Ok = true;
            } else {
                r.Ok = false;
                r.Error = std::move(err);
            }
            return r;
        });
    }
    if (watchersBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(
            watchersBusy ? "Loading watchers..." : "Load watcher list from Jira");
    }
}

void JiraGridFieldDisplay::RenderVotesField(const std::string& issueKey,
                                             const std::string& currentValue,
                                             float availWidth,
                                             bool tooltipsEnabled,
                                             JiraGridFieldAsyncState& async) {
    int voteCount = 0;
    bool hasVoted = false;
    std::string selfUrl;
    const bool parsed = ParseVotesFieldJson(currentValue, voteCount, hasVoted, selfUrl);
    if (parsed) {
        std::string line;
        if (voteCount <= 0) {
            line = hasVoted ? "You voted" : "No votes";
        } else if (voteCount == 1) {
            line = hasVoted ? "1 vote (you)" : "1 vote";
        } else {
            line = hasVoted ? (std::to_string(voteCount) + " votes (incl. you)")
                            : (std::to_string(voteCount) + " votes");
        }

        ImGui::TextUnformatted(line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            std::string tip = "Votes\n";
            tip += "Count: " + std::to_string(voteCount) + "\n";
            tip += hasVoted ? "You have voted on this issue.\n" : "You have not voted.\n";
            if (!selfUrl.empty()) {
                tip += "\n";
                tip += selfUrl;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##votes_" + issueKey;
    const bool votesBusy =
        async.votesLoadInProgress && async.votesFuture.valid() &&
        async.votesFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    if (votesBusy) {
        ImGui::BeginDisabled();
    }
    if (ImGui::SmallButton(loadBtn.c_str())) {
        async.votesPopupIssueKey = issueKey;
        async.votesPanelOpen = true;
        async.votesLoadInProgress = true;
        async.votesLoadedList.clear();
        async.votesLoadedError.clear();
        async.votesLoadedVoteCount = 0;
        async.votesLoadedHasVoted = false;
        async.votesLoadedVotersArrayInResponse = false;
        async.votesFuture = std::async(std::launch::async, [issueKey]() {
            VotesLoadResult r;
            JiraClient client;
            const JiraConfig cfg = ConfigManager::Load();
            std::string err;
            if (client.FetchIssueVotes(cfg,
                    issueKey,
                    r.Voters,
                    err,
                    &r.VoteCount,
                    &r.HasVoted,
                    &r.VotersArrayInResponse)) {
                r.Ok = true;
            } else {
                r.Ok = false;
                r.Error = std::move(err);
            }
            return r;
        });
    }
    if (votesBusy) {
        ImGui::EndDisabled();
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip(votesBusy ? "Loading votes..." : "Load voter list from Jira");
    }
}

void JiraGridFieldDisplay::RenderWorklogField(const std::string& currentValue,
                                              float availWidth,
                                              bool tooltipsEnabled) {
    WorklogFieldSummary s;
    const bool parsed = ParseWorklogFieldJson(currentValue, s);
    if (!parsed) {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    std::string line;
    if (s.Total <= 0) {
        line = "No work logged";
    } else {
        line = std::to_string(s.Total);
        line += (s.Total == 1) ? " work log" : " work logs";
        if (s.SumSecondsOnPage > 0) {
            line += " \xC2\xB7 ";
            line += FormatWorkDurationFromSeconds(s.SumSecondsOnPage);
            if (s.WorklogsOnPage < s.Total) {
                line += "*";
            }
        }
    }

    ImGui::TextUnformatted(line.c_str());
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        std::string tip = "Log work\n";
        tip += "Total entries: " + std::to_string(s.Total) + "\n";
        if (s.Total > 0 && s.WorklogsOnPage > 0) {
            tip += "This page: " + std::to_string(s.StartAt + 1) + "\xe2\x80\x93" +
                   std::to_string(s.StartAt + s.WorklogsOnPage) + " of " + std::to_string(s.Total) + "\n";
        }
        if (s.MaxResults > 0) {
            tip += "Page size (maxResults): " + std::to_string(s.MaxResults) + "\n";
        }
        if (s.SumSecondsOnPage > 0) {
            tip += "Time on page: " + FormatWorkDurationFromSeconds(s.SumSecondsOnPage);
            if (s.WorklogsOnPage < s.Total) {
                tip += " (partial; not all work logs loaded)";
            }
            tip += "\n";
        }
        const int kMaxTipEntries = 12;
        int shown = 0;
        for (const auto& e : s.Entries) {
            if (shown >= kMaxTipEntries) {
                const int rest = static_cast<int>(s.Entries.size()) - shown;
                tip += "\n... +" + std::to_string(rest) + " on this page";
                break;
            }
            tip += "\n- ";
            tip += e.Author.empty() ? std::string("(unknown)") : e.Author;
            if (!e.TimeSpentText.empty()) {
                tip += " — " + e.TimeSpentText;
            } else if (e.TimeSpentSeconds > 0) {
                tip += " — " + FormatWorkDurationFromSeconds(e.TimeSpentSeconds);
            }
            if (!e.StartedIso.empty()) {
                tip += " (" + e.StartedIso + ")";
            }
            ++shown;
        }
        if (s.Total > s.WorklogsOnPage) {
            tip += "\n\nMore entries exist in Jira than are included in this cell.";
        }
        ImGui::SetTooltip("%s", tip.c_str());
    }
}

void JiraGridFieldDisplay::DrawWatchersListWindow(JiraGridFieldAsyncState& d) {
    if (d.watchersFuture.valid()) {
        if (d.watchersFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            WatchersLoadResult r = d.watchersFuture.get();
            d.watchersLoadInProgress = false;
            if (r.Ok) {
                d.watchersLoadedList = std::move(r.Watchers);
                d.watchersLoadedError.clear();
            } else {
                d.watchersLoadedList.clear();
                d.watchersLoadedError = std::move(r.Error);
            }
        }
    }

    if (!d.watchersPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Watchers", &d.watchersPanelOpen)) {
        ImGui::TextUnformatted("Issue:");
        ImGui::SameLine();
        ImGui::TextUnformatted(d.watchersPopupIssueKey.c_str());
        ImGui::Separator();
        if (d.watchersLoadInProgress) {
            ImGui::TextDisabled("Loading...");
        } else if (!d.watchersLoadedError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.watchersLoadedError.c_str());
            ImGui::PopStyleColor();
        } else {
            if (d.watchersLoadedList.empty()) {
                ImGui::TextDisabled("No watchers.");
            } else {
                ImGui::BeginChild("WatchersList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
                for (const auto& w : d.watchersLoadedList) {
                    const std::string label = w.DisplayName.empty() ? w.AccountId : w.DisplayName;
                    ImGui::BulletText("%s", label.c_str());
                }
                ImGui::EndChild();
            }
        }
        if (ImGui::Button("Close")) {
            d.watchersPanelOpen = false;
        }
    }
    ImGui::End();
}

void JiraGridFieldDisplay::DrawVotesListWindow(JiraGridFieldAsyncState& d) {
    if (d.votesFuture.valid()) {
        if (d.votesFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            VotesLoadResult r = d.votesFuture.get();
            d.votesLoadInProgress = false;
            if (r.Ok) {
                d.votesLoadedList = std::move(r.Voters);
                d.votesLoadedError.clear();
                d.votesLoadedVoteCount = r.VoteCount;
                d.votesLoadedHasVoted = r.HasVoted;
                d.votesLoadedVotersArrayInResponse = r.VotersArrayInResponse;
            } else {
                d.votesLoadedList.clear();
                d.votesLoadedError = std::move(r.Error);
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
            }
        }
    }

    if (!d.votesPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Votes", &d.votesPanelOpen)) {
        ImGui::TextUnformatted("Issue:");
        ImGui::SameLine();
        ImGui::TextUnformatted(d.votesPopupIssueKey.c_str());
        ImGui::Separator();
        if (d.votesLoadInProgress) {
            ImGui::TextDisabled("Loading...");
        } else if (!d.votesLoadedError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.35f, 0.35f, 1.0f));
            ImGui::TextWrapped("%s", d.votesLoadedError.c_str());
            ImGui::PopStyleColor();
        } else {
            std::string summary = std::to_string(d.votesLoadedVoteCount) + " vote";
            if (d.votesLoadedVoteCount != 1) {
                summary += "s";
            }
            if (d.votesLoadedHasVoted) {
                summary += " (you voted)";
            }
            ImGui::TextUnformatted(summary.c_str());
            ImGui::Spacing();
            if (d.votesLoadedList.empty()) {
                if (d.votesLoadedVoteCount == 0) {
                    ImGui::TextDisabled("No votes.");
                } else if (!d.votesLoadedVotersArrayInResponse) {
                    ImGui::TextDisabled(
                        "Voter names are hidden by Jira permissions (View voters and watchers).");
                } else {
                    ImGui::TextDisabled("No voters to list.");
                }
            } else {
                ImGui::BeginChild("VotesList", ImVec2(0, -ImGui::GetFrameHeightWithSpacing()), true);
                for (const auto& w : d.votesLoadedList) {
                    const std::string label = w.DisplayName.empty() ? w.AccountId : w.DisplayName;
                    ImGui::BulletText("%s", label.c_str());
                }
                ImGui::EndChild();
            }
        }
        if (ImGui::Button("Close")) {
            d.votesPanelOpen = false;
        }
    }
    ImGui::End();
}
