#include "JiraGridFieldDisplay.h"
#include "AppController.h"
#include "ConfigManager.h"
#include "JsonParseUtil.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "StringUtil.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <future>
#include <string>
#include <vector>

namespace {

struct AttachmentInfo {
    std::string filename;
    std::string url;
    std::string mimeType;
};

bool IsExplicitlyEmptyAttachmentsFieldValue(const std::string& currentValue) {
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        return true;
    }

    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j)) {
        return false;
    }

    if (j.is_null()) {
        return true;
    }
    if (j.is_array()) {
        return j.empty();
    }
    if (j.is_object() && j.contains("attachments") && j["attachments"].is_array()) {
        return j["attachments"].empty();
    }
    return false;
}

bool ParseAttachmentsFieldValue(const std::string& currentValue, std::vector<AttachmentInfo>& outAttachments) {
    outAttachments.clear();
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        return false;
    }

    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j)) {
        return false;
    }

    nlohmann::json items = nlohmann::json::array();
    if (j.is_array()) {
        items = j;
    } else if (j.is_object() && j.contains("attachments") && j["attachments"].is_array()) {
        items = j["attachments"];
    } else if (j.is_object()) {
        items.push_back(j);
    } else {
        return false;
    }

    for (const auto& item : items) {
        if (!item.is_object()) {
            continue;
        }

        AttachmentInfo info;
        info.filename = item.value("filename", std::string());
        if (info.filename.empty()) {
            info.filename = item.value("name", std::string());
        }
        info.mimeType = item.value("mimeType", std::string());
        if (info.mimeType.empty()) {
            info.mimeType = item.value("mime_type", std::string());
        }

        if (item.contains("content")) {
            const auto& content = item["content"];
            if (content.is_string()) {
                info.url = content.get<std::string>();
            } else if (content.is_object()) {
                info.url = content.value("url", std::string());
                if (info.url.empty()) {
                    info.url = content.value("self", std::string());
                }
                if (info.url.empty()) {
                    info.url = content.value("href", std::string());
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
    const std::string trimmed = TrimCopyAsciiWhitespace(raw);
    if (trimmed.empty()) {
        return false;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
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
    outWatchCount = ParseJsonIntFieldLoose(j, "watchCount", 0);
    if (outWatchCount < 0) {
        outWatchCount = 0;
    }
    outSelfUrl = j.value("self", std::string());
    return true;
}

bool ParseVotesFieldJson(const std::string& raw,
                         int& outVoteCount,
                         bool& outHasVoted,
                         std::string& outSelfUrl) {
    outVoteCount = 0;
    outHasVoted = false;
    outSelfUrl.clear();
    const std::string trimmed = TrimCopyAsciiWhitespace(raw);
    if (trimmed.empty()) {
        return false;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
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
    outVoteCount = ParseJsonIntFieldLoose(j, "votes", 0);
    if (outVoteCount < 0) {
        outVoteCount = 0;
    }
    outSelfUrl = j.value("self", std::string());
    return true;
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

bool ParseWorklogFieldJson(const std::string& raw, WorklogFieldSummary& out) {
    out = WorklogFieldSummary{};
    const std::string trimmed = TrimCopyAsciiWhitespace(raw);
    if (trimmed.empty()) {
        return false;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
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
        e.TimeSpentSeconds = ParseJsonInt64FieldLoose(w, "timeSpentSeconds", 0);
        if (e.TimeSpentSeconds > 0) {
            out.SumSecondsOnPage += e.TimeSpentSeconds;
        }
        out.Entries.push_back(std::move(e));
    }
    return true;
}

bool ColumnIdEqualsLower(const std::string& id, const char* lowerAscii) {
    return ToLowerAsciiCopy(id) == lowerAscii;
}

bool TryRenderIssueRestrictionCell(const std::string& currentValue,
                                   float availWidth,
                                   bool tooltipsEnabled) {
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        return false;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
        return false;
    }
    try {
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
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        ImGui::ProgressBar(0.0f, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
        return true;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
        return false;
    }
    try {
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
    const std::string lower = ToLowerAsciiCopy(id);
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
    std::vector<AttachmentInfo> attachments;
    if (!ParseAttachmentsFieldValue(currentValue, attachments)) {
        if (IsExplicitlyEmptyAttachmentsFieldValue(currentValue)) {
            RenderClippedFieldText(std::string(), availWidth, tooltipsEnabled, true);
            return;
        }
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    std::vector<AppController::AttachmentDescriptor> descriptors;
    descriptors.reserve(attachments.size());
    for (const auto& attachment : attachments) {
        AppController::AttachmentDescriptor descriptor;
        descriptor.Filename = attachment.filename.empty() ? std::string("Attachment") : attachment.filename;
        descriptor.Url = attachment.url;
        descriptor.MimeType = attachment.mimeType;
        descriptors.push_back(std::move(descriptor));
    }

    std::string display = descriptors.front().Filename;
    const int extraCount = static_cast<int>(descriptors.size()) - 1;
    if (extraCount > 0) {
        display += " +" + std::to_string(extraCount);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.0f, 1.0f));
    ImGui::TextUnformatted(display.c_str());
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        std::string tip = "Click to open attachments window.\n";
        tip += "Attachments:\n";
        for (const auto& descriptor : descriptors) {
            tip += " - " + descriptor.Filename;
            if (!descriptor.MimeType.empty()) {
                tip += " [" + descriptor.MimeType + "]";
            }
            tip += "\n";
        }
        ImGui::SetTooltip("%s", tip.c_str());
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        app.ShowAttachmentCollection(descriptors);
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
            try {
                WatchersLoadResult r = d.watchersFuture.get();
                d.watchersLoadInProgress = false;
                if (r.Ok) {
                    d.watchersLoadedList = std::move(r.Watchers);
                    d.watchersLoadedError.clear();
                } else {
                    d.watchersLoadedList.clear();
                    d.watchersLoadedError = std::move(r.Error);
                }
            } catch (const std::exception& ex) {
                d.watchersLoadInProgress = false;
                d.watchersLoadedList.clear();
                d.watchersLoadedError = std::string("Failed to load watchers: ") + ex.what();
                LOG_ERROR("JiraGridFieldDisplay: watchers future exception issue=%s err=%s",
                          d.watchersPopupIssueKey.c_str(),
                          ex.what());
            } catch (...) {
                d.watchersLoadInProgress = false;
                d.watchersLoadedList.clear();
                d.watchersLoadedError = "Failed to load watchers.";
                LOG_ERROR("JiraGridFieldDisplay: watchers future unknown exception issue=%s",
                          d.watchersPopupIssueKey.c_str());
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
            try {
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
            } catch (const std::exception& ex) {
                d.votesLoadInProgress = false;
                d.votesLoadedList.clear();
                d.votesLoadedError = std::string("Failed to load votes: ") + ex.what();
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
                LOG_ERROR("JiraGridFieldDisplay: votes future exception issue=%s err=%s",
                          d.votesPopupIssueKey.c_str(),
                          ex.what());
            } catch (...) {
                d.votesLoadInProgress = false;
                d.votesLoadedList.clear();
                d.votesLoadedError = "Failed to load votes.";
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
                LOG_ERROR("JiraGridFieldDisplay: votes future unknown exception issue=%s",
                          d.votesPopupIssueKey.c_str());
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
