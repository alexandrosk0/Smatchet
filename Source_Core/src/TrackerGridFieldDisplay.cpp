#include "TrackerGridFieldDisplay.h"
#include "UiPerfMonitor.h"
#include "AppController.h"
#include "ConfigManager.h"


#include "JsonParseUtil.h"
#include "Logger.h"
#include "SmatchetFieldRender.h"
#include "StringUtil.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
#define ImGui SmatchetLocalizedImGui

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <exception>
#include <future>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr std::size_t kMaxRenderCacheEntries = 256;

template <typename TValue, typename TBuilder>
TValue& GetOrBuildCachedValue(std::unordered_map<std::string, TValue>& cache, const std::string& key,
                              TBuilder&& build) {
    const auto it = cache.find(key);
    if (it != cache.end()) {
        return it->second;
    }
    if (cache.size() >= kMaxRenderCacheEntries) {
        cache.clear();
    }
    return cache.emplace(key, build()).first->second;
}

struct AttachmentInfo {
    std::string filename;
    std::string url;
    std::string mimeType;
};

struct AttachmentRenderModel {
    bool parsed = false;
    bool explicitEmpty = false;
    std::vector<AppController::AttachmentDescriptor> descriptors;
    std::string display;
    std::string tooltip;
};

struct WatchersRenderModel {
    bool parsed = false;
    std::string line;
    std::string tooltip;
};

struct VotesRenderModel {
    bool parsed = false;
    std::string line;
    std::string tooltip;
};

struct WorklogRenderModel {
    bool parsed = false;
    std::string line;
    std::string tooltip;
};

struct IssueRestrictionRenderModel {
    bool rendered = false;
    std::string display;
};

struct ProgressRenderModel {
    bool rendered = false;
    float fraction = 0.0f;
};

AttachmentRenderModel BuildAttachmentRenderModel(const std::string& currentValue);
WatchersRenderModel BuildWatchersRenderModel(const std::string& currentValue);
VotesRenderModel BuildVotesRenderModel(const std::string& currentValue);
WorklogRenderModel BuildWorklogRenderModel(const std::string& currentValue);
IssueRestrictionRenderModel BuildIssueRestrictionRenderModel(const std::string& currentValue);
ProgressRenderModel BuildProgressRenderModel(const std::string& currentValue);

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

bool ParseWatchersFieldJson(const std::string& raw, int& outWatchCount, bool& outIsWatching, std::string& outSelfUrl) {
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

bool ParseVotesFieldJson(const std::string& raw, int& outVoteCount, bool& outHasVoted, std::string& outSelfUrl) {
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

bool ColumnIdEqualsLower(const std::string& id, const char* lowerAscii) { return ToLowerAsciiCopy(id) == lowerAscii; }

AttachmentRenderModel BuildAttachmentRenderModel(const std::string& currentValue) {
    AttachmentRenderModel model;
    std::vector<AttachmentInfo> attachments;
    if (!ParseAttachmentsFieldValue(currentValue, attachments)) {
        model.explicitEmpty = IsExplicitlyEmptyAttachmentsFieldValue(currentValue);
        return model;
    }

    model.parsed = true;
    model.descriptors.reserve(attachments.size());
    for (const auto& attachment : attachments) {
        AppController::AttachmentDescriptor descriptor;
        descriptor.Filename = attachment.filename.empty() ? std::string("Attachment") : attachment.filename;
        descriptor.Url = attachment.url;
        descriptor.MimeType = attachment.mimeType;
        model.descriptors.push_back(std::move(descriptor));
    }

    model.display = model.descriptors.front().Filename;
    const int extraCount = static_cast<int>(model.descriptors.size()) - 1;
    if (extraCount > 0) {
        model.display += " +" + std::to_string(extraCount);
    }

    model.tooltip = "Click to open attachments window.\nAttachments:\n";
    for (const auto& descriptor : model.descriptors) {
        model.tooltip += " - " + descriptor.Filename;
        if (!descriptor.MimeType.empty()) {
            model.tooltip += " [" + descriptor.MimeType + "]";
        }
        model.tooltip += "\n";
    }
    return model;
}

WatchersRenderModel BuildWatchersRenderModel(const std::string& currentValue) {
    WatchersRenderModel model;
    int watchCount = 0;
    bool isWatching = false;
    std::string selfUrl;
    if (!ParseWatchersFieldJson(currentValue, watchCount, isWatching, selfUrl)) {
        return model;
    }

    model.parsed = true;
    if (watchCount <= 0) {
        model.line = isWatching ? "You watch" : "No watchers";
    } else if (watchCount == 1) {
        model.line = isWatching ? "1 watcher (you)" : "1 watcher";
    } else {
        model.line = isWatching ? (std::to_string(watchCount) + " watchers (incl. you)")
                                : (std::to_string(watchCount) + " watchers");
    }

    model.tooltip = "Watchers\n";
    model.tooltip += "Count: " + std::to_string(watchCount) + "\n";
    model.tooltip += isWatching ? "You are watching this issue.\n" : "You are not watching.\n";
    if (!selfUrl.empty()) {
        model.tooltip += "\n";
        model.tooltip += selfUrl;
    }
    return model;
}

VotesRenderModel BuildVotesRenderModel(const std::string& currentValue) {
    VotesRenderModel model;
    int voteCount = 0;
    bool hasVoted = false;
    std::string selfUrl;
    if (!ParseVotesFieldJson(currentValue, voteCount, hasVoted, selfUrl)) {
        return model;
    }

    model.parsed = true;
    if (voteCount <= 0) {
        model.line = hasVoted ? "You voted" : "No votes";
    } else if (voteCount == 1) {
        model.line = hasVoted ? "1 vote (you)" : "1 vote";
    } else {
        model.line =
            hasVoted ? (std::to_string(voteCount) + " votes (incl. you)") : (std::to_string(voteCount) + " votes");
    }

    model.tooltip = "Votes\n";
    model.tooltip += "Count: " + std::to_string(voteCount) + "\n";
    model.tooltip += hasVoted ? "You have voted on this issue.\n" : "You have not voted.\n";
    if (!selfUrl.empty()) {
        model.tooltip += "\n";
        model.tooltip += selfUrl;
    }
    return model;
}

WorklogRenderModel BuildWorklogRenderModel(const std::string& currentValue) {
    WorklogRenderModel model;
    WorklogFieldSummary s;
    if (!ParseWorklogFieldJson(currentValue, s)) {
        return model;
    }

    model.parsed = true;
    if (s.Total <= 0) {
        model.line = "-";
    } else {
        model.line = std::to_string(s.Total);
        model.line += (s.Total == 1) ? " work log" : " work logs";
        if (s.SumSecondsOnPage > 0) {
            model.line += " \xC2\xB7 ";
            model.line += FormatWorkDurationFromSeconds(s.SumSecondsOnPage);
            if (s.WorklogsOnPage < s.Total) {
                model.line += "*";
            }
        }
    }

    model.tooltip = "Log work\n";
    model.tooltip += "Total entries: " + std::to_string(s.Total) + "\n";
    if (s.Total > 0 && s.WorklogsOnPage > 0) {
        model.tooltip += "This page: " + std::to_string(s.StartAt + 1) + "\xe2\x80\x93" +
                         std::to_string(s.StartAt + s.WorklogsOnPage) + " of " + std::to_string(s.Total) + "\n";
    }
    if (s.MaxResults > 0) {
        model.tooltip += "Page size (maxResults): " + std::to_string(s.MaxResults) + "\n";
    }
    if (s.SumSecondsOnPage > 0) {
        model.tooltip += "Time on page: " + FormatWorkDurationFromSeconds(s.SumSecondsOnPage);
        if (s.WorklogsOnPage < s.Total) {
            model.tooltip += " (partial; not all work logs loaded)";
        }
        model.tooltip += "\n";
    }
    const int kMaxTipEntries = 12;
    int shown = 0;
    for (const auto& e : s.Entries) {
        if (shown >= kMaxTipEntries) {
            const int rest = static_cast<int>(s.Entries.size()) - shown;
            model.tooltip += "\n... +" + std::to_string(rest) + " on this page";
            break;
        }
        model.tooltip += "\n- ";
        model.tooltip += e.Author.empty() ? std::string("(unknown)") : e.Author;
        if (!e.TimeSpentText.empty()) {
            model.tooltip += " - " + e.TimeSpentText;
        } else if (e.TimeSpentSeconds > 0) {
            model.tooltip += " - " + FormatWorkDurationFromSeconds(e.TimeSpentSeconds);
        }
        if (!e.StartedIso.empty()) {
            model.tooltip += " (" + e.StartedIso + ")";
        }
        ++shown;
    }
    if (s.Total > s.WorklogsOnPage) {
        model.tooltip += "\n\nMore entries exist in Tracker than are included in this cell.";
    }
    return model;
}

IssueRestrictionRenderModel BuildIssueRestrictionRenderModel(const std::string& currentValue) {
    IssueRestrictionRenderModel model;
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        return model;
    }
    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
        return model;
    }
    try {
        if (!j.contains("issuerestrictions") || !j["issuerestrictions"].is_object()) {
            return model;
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
        if (!shouldDisplay) {
            model.display = "Not displayed";
        } else {
            const int n = static_cast<int>(rest.size());
            if (n == 0) {
                model.display = "No restrictions";
            } else {
                model.display = std::to_string(n) + (n == 1 ? " restriction" : " restrictions");
            }
        }
        model.rendered = true;
        return model;
    } catch (...) {
        return IssueRestrictionRenderModel{};
    }
}

ProgressRenderModel BuildProgressRenderModel(const std::string& currentValue) {
    ProgressRenderModel model;
    const std::string trimmed = TrimCopyAsciiWhitespace(currentValue);
    if (trimmed.empty()) {
        model.rendered = true;
        model.fraction = 0.0f;
        return model;
    }

    // High-performance zero-allocation fast-path scanner for typical progress JSON: {"progress":P,"total":T}
    const size_t progPos = trimmed.find("\"progress\"");
    const size_t totPos = trimmed.find("\"total\"");
    if (progPos != std::string::npos && totPos != std::string::npos) {
        const size_t col1 = trimmed.find(':', progPos);
        const size_t col2 = trimmed.find(':', totPos);
        if (col1 != std::string::npos && col2 != std::string::npos) {
            auto parse_num = [&](size_t colonIdx) -> int {
                size_t i = colonIdx + 1;
                while (i < trimmed.size() && (std::isspace(static_cast<unsigned char>(trimmed[i])) || trimmed[i] == '"')) {
                    i++;
                }
                int val = 0;
                while (i < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
                    val = val * 10 + (trimmed[i] - '0');
                    i++;
                }
                return val;
            };
            const int p = parse_num(col1);
            const int t = parse_num(col2);
            if (t > 0) {
                model.rendered = true;
                model.fraction = static_cast<float>(p) / static_cast<float>(t);
                return model;
            }
        }
    }

    nlohmann::json j;
    if (!TryParseJsonMaybeDoubleEncoded(trimmed, j) || !j.is_object()) {
        return model;
    }
    try {
        if (!j.contains("progress") || !j.contains("total")) {
            return model;
        }
        const int p = ParseJsonIntFieldLoose(j, "progress", 0);
        const int t = ParseJsonIntFieldLoose(j, "total", 0);
        model.rendered = true;
        model.fraction = (t > 0) ? (static_cast<float>(p) / static_cast<float>(t)) : 0.0f;
        return model;
    } catch (...) {
        return ProgressRenderModel{};
    }
}

} // namespace

bool TrackerGridFieldDisplay::IsWatchersColumnId(const std::string& id) {
    const std::string lower = ToLowerAsciiCopy(id);
    return lower == "watchers" || lower == "watches";
}

bool TrackerGridFieldDisplay::IsVotesColumnId(const std::string& id) { return ColumnIdEqualsLower(id, "votes"); }

bool TrackerGridFieldDisplay::IsWorklogColumnId(const std::string& id) { return ColumnIdEqualsLower(id, "worklog"); }

bool TrackerGridFieldDisplay::IsProgressStyleColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "aggregateprogress");
}

bool TrackerGridFieldDisplay::IsProgressDisplayField(const TrackerField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsProgressStyleColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "progress") || ColumnIdEqualsLower(field->Name, "aggregateprogress") ||
           ColumnIdEqualsLower(field->Name, "aggregate progress");
}

bool TrackerGridFieldDisplay::TryRenderProgressJsonField(const std::string& currentValue, float availWidth) {
    SMATCHET_UI_PERF_SCOPE("TryRenderProgressJsonField");

    // Zero-overhead shortcut for empty cells (no hashing, no map lookups)
    const bool isEmpty = !std::any_of(currentValue.begin(), currentValue.end(), [](char c) {
        return !std::isspace(static_cast<unsigned char>(c));
    });
    if (isEmpty) {
        ImGui::ProgressBar(0.0f, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
        return true;
    }

    static thread_local std::unordered_map<std::string, ProgressRenderModel> cache;
    const ProgressRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildProgressRenderModel(currentValue); });
    if (model.rendered) {
        ImGui::ProgressBar(model.fraction, ImVec2(std::max(1.0f, availWidth), ImGui::GetFrameHeight()));
    }
    return model.rendered;
}

bool TrackerGridFieldDisplay::IsIssueRestrictionColumnId(const std::string& fieldId) {
    return ColumnIdEqualsLower(fieldId, "issuerestriction");
}

bool TrackerGridFieldDisplay::IsIssueRestrictionField(const TrackerField* field) {
    if (field == nullptr) {
        return false;
    }
    if (IsIssueRestrictionColumnId(field->Id)) {
        return true;
    }
    return ColumnIdEqualsLower(field->Name, "issue restriction") ||
           ColumnIdEqualsLower(field->Name, "issue restrictions");
}

bool TrackerGridFieldDisplay::TryRenderIssueRestrictionField(const std::string& currentValue, float availWidth,
                                                          bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("TryRenderIssueRestrictionField");
    static thread_local std::unordered_map<std::string, IssueRestrictionRenderModel> cache;
    const IssueRestrictionRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildIssueRestrictionRenderModel(currentValue); });
    if (model.rendered) {
        const std::string* tip = tooltipsEnabled ? &currentValue : nullptr;
        RenderClippedFieldText(model.display, availWidth, tooltipsEnabled, true, tip);
    }
    return model.rendered;
}

void TrackerGridFieldDisplay::RenderAttachmentsField(AppController& app, const std::string& currentValue, float availWidth,
                                                  bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("RenderAttachmentsField");
    static thread_local std::unordered_map<std::string, AttachmentRenderModel> cache;
    const AttachmentRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildAttachmentRenderModel(currentValue); });
    if (!model.parsed) {
        if (model.explicitEmpty) {
            RenderClippedFieldText(std::string(), availWidth, tooltipsEnabled, true);
            return;
        }
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.35f, 0.65f, 1.0f, 1.0f));
    ImGui::TextUnformatted(model.display.c_str());
    ImGui::PopStyleColor();

    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", model.tooltip.c_str());
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        app.ShowAttachmentCollection(model.descriptors);
    }
}

void TrackerGridFieldDisplay::RenderWatchersField(AppController& app, const std::string& issueKey, const std::string& currentValue,
                                               float availWidth, bool tooltipsEnabled, TrackerGridFieldAsyncState& async) {
    SMATCHET_UI_PERF_SCOPE("RenderWatchersField");
    static thread_local std::unordered_map<std::string, WatchersRenderModel> cache;
    const WatchersRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildWatchersRenderModel(currentValue); });
    if (model.parsed) {
        ImGui::TextUnformatted(model.line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", model.tooltip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##watch_" + issueKey;
    const bool watchersBusy = async.watchersLoadInProgress && async.watchersFuture.valid() &&
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
        async.watchersFuture = std::async(std::launch::async, [&app, issueKey]() {
            WatchersLoadResult r;
            std::string err;
            std::vector<TrackerUser> watchers;
            if (app.FetchIssueWatchers(issueKey, watchers, err)) {
                r.Watchers = watchers;
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
        ImGui::SetTooltip(watchersBusy ? "Loading watchers..." : "Load watcher list from Tracker");
    }
}

void TrackerGridFieldDisplay::RenderVotesField(AppController& app, const std::string& issueKey, const std::string& currentValue,
                                            float availWidth, bool tooltipsEnabled, TrackerGridFieldAsyncState& async) {
    SMATCHET_UI_PERF_SCOPE("RenderVotesField");
    static thread_local std::unordered_map<std::string, VotesRenderModel> cache;
    const VotesRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildVotesRenderModel(currentValue); });
    if (model.parsed) {
        ImGui::TextUnformatted(model.line.c_str());
        if (tooltipsEnabled && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", model.tooltip.c_str());
        }
    } else {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
    }

    ImGui::SameLine();
    const std::string loadBtn = "Load##votes_" + issueKey;
    const bool votesBusy = async.votesLoadInProgress && async.votesFuture.valid() &&
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
        async.votesFuture = std::async(std::launch::async, [&app, issueKey]() {
            VotesLoadResult r;
            std::string err;
            std::vector<TrackerUser> voters;
            if (app.FetchIssueVotes(issueKey, voters, err, &r.VoteCount, &r.HasVoted,
                                    &r.VotersArrayInResponse)) {
                r.Voters = voters;
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
        ImGui::SetTooltip(votesBusy ? "Loading votes..." : "Load voter list from Tracker");
    }
}

void TrackerGridFieldDisplay::RenderWorklogField(const std::string& currentValue, float availWidth, bool tooltipsEnabled) {
    SMATCHET_UI_PERF_SCOPE("RenderWorklogField");
    static thread_local std::unordered_map<std::string, WorklogRenderModel> cache;
    const WorklogRenderModel& model =
        GetOrBuildCachedValue(cache, currentValue, [&]() { return BuildWorklogRenderModel(currentValue); });
    if (!model.parsed) {
        RenderClippedFieldText(currentValue, availWidth, tooltipsEnabled, true);
        return;
    }

    ImGui::TextUnformatted(model.line.c_str());
    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", model.tooltip.c_str());
    }
}

void TrackerGridFieldDisplay::DrawWatchersListWindow(TrackerGridFieldAsyncState& d) {
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
                LOG_ERROR("TrackerGridFieldDisplay: watchers future exception issue=%s err=%s",
                          d.watchersPopupIssueKey.c_str(), ex.what());
            } catch (...) {
                d.watchersLoadInProgress = false;
                d.watchersLoadedList.clear();
                d.watchersLoadedError = "Failed to load watchers.";
                LOG_ERROR("TrackerGridFieldDisplay: watchers future unknown exception issue=%s",
                          d.watchersPopupIssueKey.c_str());
            }
        }
    }

    if (!d.watchersPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Watchers", &d.watchersPanelOpen,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
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

void TrackerGridFieldDisplay::DrawVotesListWindow(TrackerGridFieldAsyncState& d) {
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
                LOG_ERROR("TrackerGridFieldDisplay: votes future exception issue=%s err=%s", d.votesPopupIssueKey.c_str(),
                          ex.what());
            } catch (...) {
                d.votesLoadInProgress = false;
                d.votesLoadedList.clear();
                d.votesLoadedError = "Failed to load votes.";
                d.votesLoadedVoteCount = 0;
                d.votesLoadedHasVoted = false;
                d.votesLoadedVotersArrayInResponse = false;
                LOG_ERROR("TrackerGridFieldDisplay: votes future unknown exception issue=%s",
                          d.votesPopupIssueKey.c_str());
            }
        }
    }

    if (!d.votesPanelOpen) {
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(420, 320), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Votes", &d.votesPanelOpen,
                     ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
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
                    ImGui::TextDisabled("Voter names are hidden by Tracker permissions (View voters and watchers).");
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









