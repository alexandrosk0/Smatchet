#include "TrackerGridFieldDisplayPure.h"

#include "JsonParseUtil.h"
#include "StringUtil.h"
#include "TrackerFieldValueParser.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <string>
#include <vector>

// Byte-identical lift of TrackerGridFieldDisplay.cpp's anonymous-namespace model builders
// (see the header comment for the extraction rationale). The Parse* helpers stay file-local:
// the shell (and tests) consume only the Build* surface, whose models expose every parsed
// field the callers read.
namespace {

using TrackerGridFieldDisplayPure::AttachmentRenderModel;
using TrackerGridFieldDisplayPure::IssueRestrictionRenderModel;
using TrackerGridFieldDisplayPure::ProgressRenderModel;
using TrackerGridFieldDisplayPure::VotesRenderModel;
using TrackerGridFieldDisplayPure::WatchersRenderModel;
using TrackerGridFieldDisplayPure::WorklogRenderModel;

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

} // namespace

namespace TrackerGridFieldDisplayPure {

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
        AttachmentDescriptor descriptor;
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
    model.isWatching = isWatching;
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
    } catch (...) { // catch-all-ok: lifted byte-identical; hostile JSON shapes must degrade to the fallback model
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
                while (i < trimmed.size() &&
                       (std::isspace(static_cast<unsigned char>(trimmed[i])) || trimmed[i] == '"')) {
                    i++;
                }
                int val = 0;
                while (i < trimmed.size() && std::isdigit(static_cast<unsigned char>(trimmed[i]))) {
                    if (val >= 100000000) // clamp before *10 — avoid signed int overflow (UB) on oversized input
                        break;
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
    } catch (...) { // catch-all-ok: lifted byte-identical; hostile JSON shapes must degrade to the fallback model
        return ProgressRenderModel{};
    }
}

} // namespace TrackerGridFieldDisplayPure
