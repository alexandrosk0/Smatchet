#include "Sync/LeftViewReasonPure.h"

#include "Json/BoundedJsonParse.h"
#include "Sync/RemoteVersionPure.h"
#include "Sync/TicketChangeDiffPure.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <utility>

namespace smatchet {

namespace {

constexpr std::size_t kMaxReasonChanges = 3; // shown in the grid line; the rest become "+N more"
constexpr std::size_t kMaxDisplayValueChars = 120;

std::string ToLower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

bool IsWordChar(char c) {
    const unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) != 0 || c == '_';
}

// Whole-word containment over already lower-cased text.
bool ContainsWord(const std::string& haystack, const std::string& word) {
    if (word.empty()) {
        return false;
    }
    std::size_t pos = haystack.find(word);
    while (pos != std::string::npos) {
        const bool startOk = pos == 0 || !IsWordChar(haystack[pos - 1]);
        const std::size_t end = pos + word.size();
        const bool endOk = end >= haystack.size() || !IsWordChar(haystack[end]);
        if (startOk && endOk) {
            return true;
        }
        pos = haystack.find(word, pos + 1);
    }
    return false;
}

bool IsBookkeepingField(const std::string& fieldId) {
    static const char* const kSkipped[] = {"updated", "lastviewed",        "comment",    "comments",      "worklog",
                                           "history", "changelog",         "timespent",  "progress",      "watches",
                                           "votes",   "aggregateprogress", "attachment", "resolutiondate"};
    const std::string lower = ToLower(fieldId);
    for (const char* skipped : kSkipped) {
        if (lower == skipped) {
            return true;
        }
    }
    return false;
}

std::string Truncated(const std::string& value) {
    if (value.size() <= kMaxDisplayValueChars) {
        return value;
    }
    std::size_t cut = kMaxDisplayValueChars;
    // Never split a UTF-8 sequence: back up to a lead byte.
    while (cut > 0 && (static_cast<unsigned char>(value[cut]) & 0xC0) == 0x80) {
        --cut;
    }
    return value.substr(0, cut) + "...";
}

std::string DisplayLabel(const TrackerFieldChange& change,
                         const std::unordered_map<std::string, std::string>& fieldLabels) {
    const auto byId = fieldLabels.find(change.fieldId);
    if (byId != fieldLabels.end() && !byId->second.empty()) {
        return byId->second;
    }
    std::string label = !change.fieldLabel.empty() ? change.fieldLabel : change.fieldId;
    if (!label.empty()) {
        label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
    }
    return label;
}

// The change's text in the wording the toast uses ("Status: To Do -> Done", "Status: cleared", ...).
std::string ChangeText(const TrackerFieldChange& change,
                       const std::unordered_map<std::string, std::string>& fieldLabels) {
    TicketChangeSummary summary;
    summary.kind = TicketChangeKind::Modified;
    summary.changedFieldLabel = DisplayLabel(change, fieldLabels);
    summary.fromValue = Truncated(change.fromValue);
    summary.toValue = Truncated(change.toValue);
    std::string line = FormatTicketChangeToast(std::vector<TicketChangeSummary>(1, summary), 1);
    // The shared wording leads with the issue key, which is empty here.
    if (!line.empty() && line[0] == ' ') {
        line.erase(0, 1);
    }
    return line;
}

std::string JoinChanges(const std::vector<TrackerFieldChange>& changes,
                        const std::unordered_map<std::string, std::string>& fieldLabels) {
    std::string out;
    const std::size_t shown = (std::min)(changes.size(), kMaxReasonChanges);
    for (std::size_t i = 0; i < shown; ++i) {
        if (i != 0) {
            out += "; ";
        }
        out += ChangeText(changes[i], fieldLabels);
    }
    if (changes.size() > shown) {
        out += " +" + std::to_string(changes.size() - shown) + " more";
    }
    return out;
}

std::string NewestAuthor(const std::vector<TrackerFieldChange>& changes) {
    for (const TrackerFieldChange& change : changes) {
        if (!change.author.empty()) {
            return change.author;
        }
    }
    return std::string();
}

std::string DumpLenient(const nlohmann::json& value) {
    // Tracker text is not guaranteed valid UTF-8, and dump() throws on invalid input by default.
    return value.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

bool QueryNamesField(const std::string& query, const std::string& fieldId, const std::string& fieldLabel) {
    const std::string lowerQuery = ToLower(query);
    const std::string lowerId = ToLower(fieldId);
    const std::string lowerLabel = ToLower(fieldLabel);
    if (ContainsWord(lowerQuery, lowerId) || ContainsWord(lowerQuery, lowerLabel)) {
        return true;
    }
    // A label of several words is matched as written, and that includes the quoted form `"story points"`.
    if ((lowerId == "status" || lowerLabel == "status") && ContainsWord(lowerQuery, "statuscategory")) {
        return true;
    }
    static const std::string kCustomPrefix = "customfield_";
    if (lowerId.compare(0, kCustomPrefix.size(), kCustomPrefix) == 0) {
        const std::string shortForm = "cf[" + lowerId.substr(kCustomPrefix.size()) + "]";
        return lowerQuery.find(shortForm) != std::string::npos;
    }
    return false;
}

std::vector<TrackerFieldChange> DiffRowFields(const std::string& issueKey,
                                              const std::unordered_map<std::string, std::string>& lastKnown,
                                              const std::unordered_map<std::string, std::string>& fresh) {
    std::vector<TrackerFieldChange> out;
    for (const auto& entry : lastKnown) {
        if (IsBookkeepingField(entry.first)) {
            continue;
        }
        const auto freshIt = fresh.find(entry.first);
        if (freshIt == fresh.end() || freshIt->second == entry.second) {
            continue;
        }
        TrackerFieldChange change;
        change.issueKey = issueKey;
        change.fieldId = entry.first;
        change.fieldLabel = entry.first;
        change.fromValue = entry.second;
        change.toValue = freshIt->second;
        out.push_back(std::move(change));
    }
    std::sort(out.begin(), out.end(),
              [](const TrackerFieldChange& a, const TrackerFieldChange& b) { return a.fieldId < b.fieldId; });
    return out;
}

std::vector<TrackerFieldChange> ChangesSince(const std::vector<TrackerFieldChange>& history, std::int64_t sinceMs) {
    std::vector<std::pair<std::int64_t, const TrackerFieldChange*>> kept;
    for (const TrackerFieldChange& change : history) {
        if (IsBookkeepingField(change.fieldId.empty() ? change.fieldLabel : change.fieldId)) {
            continue;
        }
        // An unreadable stamp keeps the entry, sorted last: a real change is never dropped on a malformed timestamp.
        std::int64_t changedMs = 0;
        const bool readable = sync::ParseRemoteUpdatedMs(change.changedAt, changedMs);
        if (readable && changedMs <= sinceMs) {
            continue;
        }
        kept.emplace_back(readable ? changedMs : 0, &change);
    }
    std::stable_sort(kept.begin(), kept.end(),
                     [](const std::pair<std::int64_t, const TrackerFieldChange*>& a,
                        const std::pair<std::int64_t, const TrackerFieldChange*>& b) { return a.first > b.first; });
    std::vector<TrackerFieldChange> out;
    out.reserve(kept.size());
    for (const std::pair<std::int64_t, const TrackerFieldChange*>& entry : kept) {
        out.push_back(*entry.second);
    }
    return out;
}

LeftViewReason BuildLeftViewReason(const std::vector<TrackerFieldChange>& changes, const std::string& query,
                                   const std::unordered_map<std::string, std::string>& fieldLabels) {
    LeftViewReason reason;
    if (changes.empty()) {
        reason.kind = LeftViewReasonKind::NoChangeFound;
        reason.text = "Left view (cause unknown): no compared field changed.";
        return reason;
    }
    std::vector<TrackerFieldChange> named;
    for (const TrackerFieldChange& change : changes) {
        if (QueryNamesField(query, change.fieldId, DisplayLabel(change, fieldLabels))) {
            named.push_back(change);
        }
    }
    if (!named.empty()) {
        reason.kind = LeftViewReasonKind::QueryFieldChanged;
        reason.text = JoinChanges(named, fieldLabels);
        reason.author = NewestAuthor(named);
        reason.changes = std::move(named);
        return reason;
    }
    reason.kind = LeftViewReasonKind::OtherFieldChanged;
    reason.text = "Possibly " + JoinChanges(changes, fieldLabels) + " (the view's query does not name this field)";
    reason.author = NewestAuthor(changes);
    reason.changes = changes;
    return reason;
}

LeftViewReason DeletedReason() {
    LeftViewReason reason;
    reason.kind = LeftViewReasonKind::Deleted;
    reason.text = "Deleted on the tracker";
    return reason;
}

LeftViewReason UnavailableReason() {
    LeftViewReason reason;
    reason.kind = LeftViewReasonKind::Unavailable;
    reason.text = "Reason unavailable: the issue could not be read from the tracker.";
    return reason;
}

LeftViewReason NotVisibleReason() {
    LeftViewReason reason;
    reason.kind = LeftViewReasonKind::NotVisible;
    reason.text = "Deleted or no longer visible";
    return reason;
}

LeftViewReason NotCheckedReason() {
    LeftViewReason reason;
    reason.kind = LeftViewReasonKind::NotChecked;
    reason.text = "Reason not checked: too many issues left at once.";
    return reason;
}

std::string LastKnownFieldsToJson(const std::unordered_map<std::string, std::string>& fieldValues,
                                  std::size_t maxValueBytes, std::size_t maxTotalBytes) {
    // Ordered, so which fields survive the total cap does not depend on hash order.
    const std::map<std::string, std::string> ordered(fieldValues.begin(), fieldValues.end());
    nlohmann::json out = nlohmann::json::object();
    std::size_t total = 0;
    for (const auto& entry : ordered) {
        if (entry.second.empty()) {
            continue;
        }
        const std::string value = entry.second.substr(0, maxValueBytes);
        if (total + entry.first.size() + value.size() > maxTotalBytes) {
            continue;
        }
        total += entry.first.size() + value.size();
        out[entry.first] = value;
    }
    return DumpLenient(out);
}

std::unordered_map<std::string, std::string> LastKnownFieldsFromJson(const std::string& json) {
    std::unordered_map<std::string, std::string> out;
    const nlohmann::json parsed = json_safe::ParseBoundedOrDiscarded(json);
    if (!parsed.is_object()) {
        return out;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
        if (it.value().is_string()) {
            out[it.key()] = it.value().get<std::string>();
        }
    }
    return out;
}

std::string ReasonDetailToJson(const std::vector<TrackerFieldChange>& changes) {
    nlohmann::json out = nlohmann::json::array();
    for (const TrackerFieldChange& change : changes) {
        nlohmann::json row = nlohmann::json::object();
        row["fieldId"] = change.fieldId;
        row["field"] = change.fieldLabel;
        row["from"] = Truncated(change.fromValue);
        row["to"] = Truncated(change.toValue);
        row["author"] = change.author;
        row["at"] = change.changedAt;
        out.push_back(std::move(row));
    }
    return DumpLenient(out);
}

} // namespace smatchet
