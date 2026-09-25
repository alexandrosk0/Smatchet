#include "TrackerFieldValueParser.h"
#include "TrackerFieldValueUtils.h"

#include "Json/BoundedJsonParse.h"
#include "JiraCommentMappingPure.h"
#include "Logger.h"
#include "Tracker/CommentBlobFormatPure.h"
#include "StringUtil.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_set>

// nlohmann::value("k", std::string()) throws type_error.302 if "k" exists but is not a string.
std::string JsonGetStringIfString(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

std::string TrimTrailingZeros(const std::string& number);

std::string JsonValueToCompactString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
        return TrimTrailingZeros(oss.str());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    return {};
}

std::string JsonIdToString(const nlohmann::json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    return {};
}

std::string BuildTrackerOptionDisplayValue(const nlohmann::json& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_primitive()) {
        return JsonValueToCompactString(value);
    }
    if (!value.is_object()) {
        return {};
    }

    if (value.contains("key") && value["key"].is_string()) {
        const std::string key = value["key"].get<std::string>();
        const std::string summary = value.contains("fields") && value["fields"].is_object()
                                        ? JsonGetStringIfString(value["fields"], "summary")
                                        : std::string();
        if (!key.empty() && !summary.empty()) {
            return key + " - " + summary;
        }
        if (!key.empty()) {
            return key;
        }
    }

    static const char* kPreferredLabelKeys[] = {"displayName", "value", "name",    "label",     "title",
                                                "summary",     "key",   "groupId", "accountId", "id"};
    for (const char* key : kPreferredLabelKeys) {
        const auto it = value.find(key);
        if (it == value.end()) {
            continue;
        }
        const std::string label = JsonValueToCompactString(*it);
        if (!label.empty()) {
            return label;
        }
    }
    return {};
}

std::string BuildTrackerOptionId(const nlohmann::json& value) {
    if (value.is_null()) {
        return {};
    }
    if (value.is_primitive()) {
        return JsonValueToCompactString(value);
    }
    if (!value.is_object()) {
        return {};
    }
    static const char* kPreferredIdKeys[] = {"id", "accountId", "groupId", "key", "value", "name"};
    for (const char* key : kPreferredIdKeys) {
        const auto it = value.find(key);
        if (it == value.end()) {
            continue;
        }
        const std::string id = JsonIdToString(*it);
        if (!id.empty()) {
            return id;
        }
    }
    return {};
}

bool IsSimpleOptionObject(const nlohmann::json& value) {
    if (!value.is_object()) {
        return false;
    }
    static const std::unordered_set<std::string> kSimpleKeys = {"id",          "value", "name",     "disabled", "self",
                                                                "description", "label", "archived", "released"};
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (kSimpleKeys.find(it.key()) == kSimpleKeys.end()) {
            return false;
        }
    }
    return true;
}

TrackerFieldOption TrackerFieldOptionFromJson(const nlohmann::json& value) {
    TrackerFieldOption option;
    option.Id = BuildTrackerOptionId(value);
    option.Value = BuildTrackerOptionDisplayValue(value);
    if (option.Value.empty()) {
        option.Value = option.Id;
    }
    if (value.is_object()) {
        option.SecondaryValue = JsonGetStringIfString(value, "description");
        if (option.SecondaryValue.empty()) {
            option.SecondaryValue = JsonGetStringIfString(value, "key");
            if (option.SecondaryValue == option.Value) {
                option.SecondaryValue.clear();
            }
        }
        const auto disabledIt = value.find("disabled");
        option.Disabled = disabledIt != value.end() && disabledIt->is_boolean() && disabledIt->get<bool>();
        const auto childrenIt = value.find("children");
        if (childrenIt != value.end() && childrenIt->is_array()) {
            for (const auto& child : *childrenIt) {
                option.Children.push_back(TrackerFieldOptionFromJson(child));
            }
        }
    }
    try {
        option.PayloadJson = value.dump();
    } catch (...) {
        option.PayloadJson.clear();
    }
    return option;
}

std::string TrackerFieldOptionKey(const TrackerFieldOption& option) {
    if (!option.Id.empty()) {
        return "id:" + ToLowerAsciiCopy(option.Id);
    }
    return "value:" + ToLowerAsciiCopy(option.Value);
}

void MergeTrackerFieldOption(std::vector<TrackerFieldOption>& target, const TrackerFieldOption& incoming) {
    const std::string incomingKey = TrackerFieldOptionKey(incoming);
    for (auto& existing : target) {
        if (TrackerFieldOptionKey(existing) != incomingKey) {
            continue;
        }
        if (existing.Value.empty()) {
            existing.Value = incoming.Value;
        }
        if (existing.SecondaryValue.empty()) {
            existing.SecondaryValue = incoming.SecondaryValue;
        }
        if (existing.PayloadJson.empty()) {
            existing.PayloadJson = incoming.PayloadJson;
        }
        existing.Disabled = existing.Disabled || incoming.Disabled;
        for (const auto& child : incoming.Children) {
            MergeTrackerFieldOption(existing.Children, child);
        }
        return;
    }
    target.push_back(incoming);
}

void RefreshTrackerAllowedValuesFromOptions(TrackerField& field) {
    field.AllowedValues.clear();
    field.AllowedValues.reserve(field.AllowedValueOptions.size());
    for (const auto& option : field.AllowedValueOptions) {
        if (!option.Value.empty()) {
            field.AllowedValues.push_back(option.Value);
        }
    }
}

TrackerFieldFamily ClassifyTrackerFieldFamily(const TrackerField& field) {
    const std::string lowerId = ToLowerAsciiCopy(field.Id);
    const std::string lowerCustom = ToLowerAsciiCopy(field.SchemaCustom);
    if (lowerId == "labels") {
        return TrackerFieldFamily::Labels;
    }
    if (lowerId == "status") {
        return TrackerFieldFamily::Status;
    }
    if (lowerId == "issuetype") {
        return TrackerFieldFamily::IssueType;
    }
    if (lowerId == "components" || ToLowerAsciiCopy(field.ItemsType) == "component") {
        return TrackerFieldFamily::SelectMulti;
    }
    if (lowerCustom.find("gh-sprint") != std::string::npos) {
        return TrackerFieldFamily::Sprint;
    }
    if (field.Type == "date") {
        return TrackerFieldFamily::Date;
    }
    if (field.Type == "datetime") {
        return TrackerFieldFamily::DateTime;
    }
    if (field.Type == "number") {
        return TrackerFieldFamily::Number;
    }
    if (field.IsUserType) {
        return field.IsArray ? TrackerFieldFamily::UserMulti : TrackerFieldFamily::UserSingle;
    }
    if (lowerCustom.find("cascadingselect") != std::string::npos) {
        return TrackerFieldFamily::CascadingSelect;
    }
    if (!field.AllowedValueOptions.empty()) {
        const bool hasChildren = std::any_of(field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(),
                                             [](const TrackerFieldOption& option) { return !option.Children.empty(); });
        if (hasChildren) {
            return TrackerFieldFamily::CascadingSelect;
        }
        const bool likelyStructured = std::any_of(
            field.AllowedValueOptions.begin(), field.AllowedValueOptions.end(), [](const TrackerFieldOption& option) {
                if (option.PayloadJson.empty()) {
                    return false;
                }
                // PayloadJson is a re-parse of a tracker-option value (or a value loaded from the
                // on-disk field-catalog cache). A bare json::parse here — even the non-throwing
                // 3-arg form — builds the full DOM and stack-overflows the recursive ~json teardown
                // on a deeply-nested string (uncatchable). Route through the depth/node/byte-bounded
                // ParseBounded; on failure treat the option as not-structured (graceful, Pillar 3).
                std::string parseErr;
                nlohmann::json raw = smatchet::json_safe::ParseBounded(option.PayloadJson, parseErr);
                if (!parseErr.empty()) {
                    return false;
                }
                return raw.is_object() && !IsSimpleOptionObject(raw);
            });
        if (likelyStructured) {
            return field.IsArray ? TrackerFieldFamily::StructuredMulti : TrackerFieldFamily::StructuredSingle;
        }
        return field.IsArray ? TrackerFieldFamily::SelectMulti : TrackerFieldFamily::SelectSingle;
    }
    return TrackerFieldFamily::Text;
}

std::string TrimTrailingZeros(const std::string& number) {
    if (number.find('.') == std::string::npos) {
        return number;
    }
    if (number.find('e') != std::string::npos || number.find('E') != std::string::npos) {
        return number;
    }

    std::string out = number;
    while (!out.empty() && out.back() == '0') {
        out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out.empty() ? std::string("0") : out;
}

std::string MaybeFormatDateString(const std::string& value) {
    // Keep display compact for ISO datetime values.
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-') {
        return value.substr(0, 10);
    }
    return value;
}

// Atlassian Document Format is server-supplied JSON (a foreign trust boundary):
// a malicious or buggy tracker can return ADF nested thousands of levels deep,
// blowing the C++ stack via the recursive walkers below (Pillar 3 — Never crash).
// Real ADF is shallow (a handful of nesting levels); 256 sits far above any
// legitimate document while still bounding stack growth well short of overflow.
// On reaching the cap we stop recursing and degrade gracefully (truncate the
// sub-tree) rather than throw — Pillar 3 wants graceful degradation, not an
// exception unwinding through the parse.
const int kMaxAdfRecursionDepth = 256;

void WarnAdfDepthCapped() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARN("TrackerFieldValueParser: ADF nesting exceeded depth cap (%d); truncating remainder of "
                 "document. Possible hostile or malformed tracker response.",
                 kMaxAdfRecursionDepth);
    }
}

// Bounded depth probe: true iff `v` nests deeper than `cap`. The probe's own
// recursion STOPS at `cap`, so it is bounded (ASAN-safe) even on a hostile
// arbitrarily-deep value.
bool JsonExceedsDepth(const nlohmann::json& v, int cap, int depth = 0) {
    if (depth >= cap) {
        return true;
    }
    if (v.is_object() || v.is_array()) {
        for (const auto& el : v) {
            if (JsonExceedsDepth(el, cap, depth + 1)) {
                return true;
            }
        }
    }
    return false;
}

// Serialize a SERVER-CONTROLLED value safely. nlohmann's serializer::dump
// recurses once per nesting level, so a deeply-nested hostile field or comment
// value overflows the stack — a Pillar 3 DoS, and the dump-path sibling of the ADF
// walker cap. The #1220 fix capped the walkers, but these dump fallbacks still
// recursed unbounded and crashed the ASan doctest rig. A value nested past the cap
// is therefore NOT dumped: return a bounded marker rather than recurse the serializer.
std::string SafeJsonDump(const nlohmann::json& v) {
    if (JsonExceedsDepth(v, kMaxAdfRecursionDepth)) {
        WarnAdfDepthCapped();
        return std::string("[value nesting exceeded depth cap]");
    }
    return v.dump();
}

void CollectAdfText(const nlohmann::json& node, std::vector<std::string>& out, int depth = 0) {
    if (depth >= kMaxAdfRecursionDepth) {
        WarnAdfDepthCapped();
        return;
    }
    if (!node.is_object()) {
        return;
    }

    if (node.contains("text") && node["text"].is_string()) {
        const std::string text = TrimCopy(node["text"].get<std::string>());
        if (!text.empty()) {
            out.push_back(text);
        }
    }

    if (node.contains("content") && node["content"].is_array()) {
        for (const auto& child : node["content"]) {
            CollectAdfText(child, out, depth + 1);
        }
    }
}

void ExtractAdfTextToStream(const nlohmann::json& node, std::ostringstream& out, int depth = 0) {
    if (depth >= kMaxAdfRecursionDepth) {
        WarnAdfDepthCapped();
        return;
    }
    if (node.is_array()) {
        for (const auto& child : node) {
            ExtractAdfTextToStream(child, out, depth + 1);
        }
        return;
    }
    if (!node.is_object()) {
        return;
    }

    const std::string nodeType = JsonGetStringIfString(node, "type");

    if (nodeType == "hardBreak") {
        out << "\n";
    }

    if (node.contains("text") && node["text"].is_string()) {
        const std::string text = node["text"].get<std::string>();
        if (!text.empty()) {
            out << text;
        }
    } else if (node.contains("attrs") && node["attrs"].is_object()) {
        const auto& attrs = node["attrs"];
        if (attrs.contains("text") && attrs["text"].is_string()) {
            out << attrs["text"].get<std::string>();
        } else if (attrs.contains("title") && attrs["title"].is_string()) {
            out << attrs["title"].get<std::string>();
        } else if (attrs.contains("url") && attrs["url"].is_string()) {
            out << attrs["url"].get<std::string>();
        } else if (attrs.contains("shortName") && attrs["shortName"].is_string()) {
            out << attrs["shortName"].get<std::string>();
        }
    }

    if (node.contains("content")) {
        ExtractAdfTextToStream(node["content"], out, depth + 1);
        if (nodeType == "paragraph" || nodeType == "heading" || nodeType == "listItem") {
            out << "\n";
        }
    }
}

std::string FormatDateIfIso(const std::string& value) {
    // `value` is server-supplied (a foreign trust boundary). The size() >= 10 guard MUST
    // precede the fixed-index reads below — value[4]/value[7] on a shorter field is an
    // out-of-bounds read (Pillar 3 — Never crash). Do not weaken this to a bare index.
    if (value.size() >= 10 && value[4] == '-' && value[7] == '-') {
        return value.substr(0, 10);
    }
    return value;
}

std::string ParseCommentAuthor(const nlohmann::json& commentNode) {
    std::string author = "Unknown";
    if (commentNode.contains("author") && commentNode["author"].is_object()) {
        const auto& authorObj = commentNode["author"];
        if (authorObj.contains("displayName") && authorObj["displayName"].is_string()) {
            author = authorObj["displayName"].get<std::string>();
        }
    }
    return author;
}

std::string AdfBodyToPlainText(const nlohmann::json& body) {
    if (body.is_string()) {
        return body.get<std::string>();
    }
    if (body.is_object() && body.contains("content")) {
        std::ostringstream textStream;
        ExtractAdfTextToStream(body, textStream);
        std::string text = TrimCopy(textStream.str());
        if (text.empty()) {
            std::vector<std::string> fallbackParts;
            CollectAdfText(body, fallbackParts);
            text = JoinStrings(fallbackParts, " ");
        }
        return text;
    }
    return std::string();
}

void SortTrackerUsersForDisplay(std::vector<TrackerUser>& users) {
    std::sort(users.begin(), users.end(), [](const TrackerUser& a, const TrackerUser& b) {
        const std::string& lhs = a.DisplayName.empty() ? a.AccountId : a.DisplayName;
        const std::string& rhs = b.DisplayName.empty() ? b.AccountId : b.DisplayName;
        return std::lexicographical_compare(
            lhs.begin(), lhs.end(), rhs.begin(), rhs.end(),
            [](unsigned char ca, unsigned char cb) { return std::tolower(ca) < std::tolower(cb); });
    });
}

void AppendTrackerUsersFromJsonArray(const nlohmann::json& arr, std::vector<TrackerUser>& outUsers) {
    if (!arr.is_array()) {
        return;
    }
    for (const auto& node : arr) {
        if (!node.is_object()) {
            continue;
        }
        TrackerUser u;
        u.AccountId = node.value("accountId", std::string());
        u.DisplayName = node.value("displayName", std::string());
        u.EmailAddress = node.value("emailAddress", std::string());
        u.Active = node.value("active", true);
        if (!u.AccountId.empty() || !u.DisplayName.empty()) {
            outUsers.push_back(std::move(u));
        }
    }
}

std::string ParseComments(const nlohmann::json& commentsArray) {
    // Delegates to the ONE shared tooltip-blob pipeline: the Jira node mapper
    // (author/ADF-body/timestamp extraction) feeds the backend-agnostic
    // FormatCommentBlob, so Jira search rows, the lazy hover fetch and the
    // modal post-back all produce byte-identical fieldValues["comment"] text.
    return smatchet::tracker::FormatCommentBlob(smatchet::jira::MapJiraIssueComments(commentsArray));
}

static std::string FormatChangelogTimeValue(const std::string& value) {
    if (value.empty()) {
        return value;
    }
    const bool allDigits = std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch); });
    if (allDigits) {
        try {
            long long seconds = std::stoll(value);
            if (seconds == 0) {
                return "0m";
            }
            std::string formatted = FormatWorkDurationFromSeconds(seconds);
            if (!formatted.empty()) {
                return formatted;
            }
        } catch (...) {
            // Keep original string if there's a parsing/overflow error
        }
    }
    return value;
}

namespace {

// Convert one changelog scalar (the value behind a fromString/toString/from/to
// key) to its string form. Returns empty for unhandled JSON types.
std::string ChangelogScalarToString(const nlohmann::json& value) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    if (value.is_number_unsigned())
        return std::to_string(value.get<unsigned long long>());
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
        return TrimTrailingZeros(oss.str());
    }
    if (value.is_boolean())
        return value.get<bool>() ? "true" : "false";
    if (value.is_object() || value.is_array())
        return SafeJsonDump(value);
    return std::string();
}

// Prefer the human-readable string key, then the raw identifier key. The
// identifier path intentionally skips dumping nested objects and arrays —
// preserved verbatim from the original two-block lambda.
std::string ChangelogResolveValue(const nlohmann::json& changeItem, const char* stringKey, const char* idKey) {
    if (changeItem.contains(stringKey) && !changeItem[stringKey].is_null()) {
        const std::string s = ChangelogScalarToString(changeItem[stringKey]);
        if (!s.empty() || changeItem[stringKey].is_string()) {
            return s;
        }
    }
    if (changeItem.contains(idKey) && !changeItem[idKey].is_null()) {
        const auto& value = changeItem[idKey];
        if (value.is_string())
            return value.get<std::string>();
        if (value.is_number_integer())
            return std::to_string(value.get<long long>());
        if (value.is_number_unsigned())
            return std::to_string(value.get<unsigned long long>());
        if (value.is_number_float()) {
            std::ostringstream oss;
            oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
            return TrimTrailingZeros(oss.str());
        }
    }
    return std::string();
}

// Process every change item in one history entry, appending formatted lines.
// Returns once entryCount reaches the cap so the caller can stop iterating.
void ParseChangelogHistory(const nlohmann::json& history, size_t maxEntries, size_t maxValueLength,
                           std::ostringstream& formatted, size_t& entryCount) {
    if (!history.is_object()) {
        return;
    }

    std::string author = "Unknown";
    if (history.contains("author") && history["author"].is_object()) {
        const auto& authorObj = history["author"];
        if (authorObj.contains("displayName") && authorObj["displayName"].is_string()) {
            author = authorObj["displayName"].get<std::string>();
        }
    }

    std::string created;
    if (history.contains("created") && history["created"].is_string()) {
        created = MaybeFormatDateString(history["created"].get<std::string>());
    }

    if (!history.contains("items") || !history["items"].is_array()) {
        return;
    }

    for (const auto& changeItem : history["items"]) {
        if (!changeItem.is_object()) {
            continue;
        }
        std::string fieldName;
        if (changeItem.contains("field") && !changeItem["field"].is_null()) {
            const auto& f = changeItem["field"];
            fieldName = f.is_string() ? f.get<std::string>() : f.dump();
        }

        std::string fromValue = ChangelogResolveValue(changeItem, "fromString", "from");
        std::string toValue = ChangelogResolveValue(changeItem, "toString", "to");

        if (TrackerFieldValueUtils::IsTimeDurationField(fieldName)) {
            fromValue = FormatChangelogTimeValue(fromValue);
            toValue = FormatChangelogTimeValue(toValue);
        }

        if (fromValue.size() > maxValueLength) {
            fromValue.resize(maxValueLength);
            fromValue += "...";
        }
        if (toValue.size() > maxValueLength) {
            toValue.resize(maxValueLength);
            toValue += "...";
        }

        if (entryCount > 0) {
            formatted << "\n";
        }
        formatted << "[" << author << "] " << created << "\n"
                  << fieldName << ": " << fromValue << " -> " << toValue << "\n";
        entryCount++;

        if (entryCount >= maxEntries) {
            return;
        }
    }
}

} // namespace

std::string ParseChangelog(const nlohmann::json& histories) {
    const size_t kMaxChangelogEntries = 60;
    const size_t kMaxValueLength = 200;
    const size_t kMaxRawChangelogChars = 16000;

    if (!histories.is_array() || histories.empty()) {
        return std::string();
    }

    std::ostringstream formatted;
    size_t entryCount = 0;

    for (auto it = histories.begin(); it != histories.end() && entryCount < kMaxChangelogEntries; ++it) {
        ParseChangelogHistory(*it, kMaxChangelogEntries, kMaxValueLength, formatted, entryCount);
    }

    if (entryCount >= kMaxChangelogEntries) {
        formatted << "\n[... truncated ...]\n";
    }

    if (!formatted.str().empty()) {
        return formatted.str();
    }

    std::string raw = histories.dump();
    if (raw.size() > kMaxRawChangelogChars) {
        raw.resize(kMaxRawChangelogChars);
        raw += "...";
    }
    return raw;
}

long long ParseWorkDurationToSeconds(const std::string& input) {
    std::string s = TrimCopy(input);
    if (s.empty()) {
        return 0;
    }

    // Support a plain number (seconds).
    const bool allDigits = std::all_of(s.begin(), s.end(), [](unsigned char ch) { return std::isdigit(ch); });
    if (allDigits) {
        try {
            return std::stoll(s);
        } catch (const std::exception& ex) {
            LOG_DEBUG("JiraClient: ParseWorkDurationToSeconds overflow/invalid value=%s err=%s", s.c_str(), ex.what());
        } catch (...) {
            LOG_DEBUG("JiraClient: ParseWorkDurationToSeconds unknown parse failure value=%s", s.c_str());
        }
    }

    size_t pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == ' ')
            pos++;
        if (pos >= s.size())
            break;

        long long number = 0;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t start = pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos])))
                pos++;
            try {
                number = std::stoll(s.substr(start, pos - start));
            } catch (const std::exception& ex) {
                LOG_DEBUG("JiraClient: ParseWorkDurationToSeconds token parse failure value=%s err=%s", s.c_str(),
                          ex.what());
                break;
            } catch (...) {
                LOG_DEBUG("JiraClient: ParseWorkDurationToSeconds token parse unknown failure value=%s", s.c_str());
                break;
            }
        } else {
            break;
        }

        // Clamp to a sane upper bound: a tracker-supplied worklog string with a huge
        // magnitude would otherwise make `number * <unit multiplier>` and the running
        // `total` signed-overflow (UB). 1e9 units is far beyond any real duration and
        // keeps every product (max multiplier 144000) well inside long long.
        constexpr long long kMaxDurationUnits = 1000000000LL;
        if (number > kMaxDurationUnits)
            number = kMaxDurationUnits;

        if (pos >= s.size())
            break;
        char u = s[pos++];
        long long addend = 0;
        if (u == 'w' || u == 'W') {
            addend = number * 5 * 8 * 60 * 60;
        } else if (u == 'd' || u == 'D') {
            addend = number * 8 * 60 * 60;
        } else if (u == 'h' || u == 'H') {
            addend = number * 60 * 60;
        } else if (u == 'm' || u == 'M') {
            addend = number * 60;
        } else {
            break;
        }
        // Saturating add: clamping each `number` bounds a single addend, but a worklog
        // string with thousands of tokens could still overflow the running `total`. Cap it
        // at a ceiling far beyond any real duration so the accumulation can never wrap (UB).
        constexpr long long kMaxTotalSeconds = 1000000000000000LL; // ~31.7 million years
        if (total > kMaxTotalSeconds - addend)
            total = kMaxTotalSeconds;
        else
            total += addend;
    }
    return total;
}

bool JsonLooksLikeJiraTimetracking(const nlohmann::json& o) {
    if (!o.is_object()) {
        return false;
    }
    static const char* keys[] = {"originalEstimate",  "originalEstimateSeconds",
                                 "remainingEstimate", "remainingEstimateSeconds",
                                 "timeSpent",         "timeSpentSeconds"};
    for (const char* k : keys) {
        if (o.contains(k)) {
            return true;
        }
    }
    return false;
}

// Jira returns timetracking as an object with human strings and parallel *Seconds fields.
std::string FormatTrackerTimetrackingDisplay(const nlohmann::json& o) {
    if (!o.is_object()) {
        return o.dump();
    }

    auto scalar = [&](const char* strKey, const char* secKey) -> std::string {
        if (o.contains(strKey) && o[strKey].is_string()) {
            std::string s = TrimCopy(o[strKey].get<std::string>());
            if (!s.empty()) {
                return s;
            }
        }
        if (!o.contains(secKey) || o[secKey].is_null()) {
            return {};
        }
        long long sec = 0;
        if (o[secKey].is_number_integer()) {
            sec = o[secKey].get<long long>();
        } else if (o[secKey].is_number_unsigned()) {
            sec = static_cast<long long>(o[secKey].get<unsigned long long>());
        } else {
            return {};
        }
        return FormatWorkDurationFromSeconds(sec);
    };

    std::vector<std::string> bits;
    const std::string orig = scalar("originalEstimate", "originalEstimateSeconds");
    if (!orig.empty()) {
        bits.push_back("Original estimate " + orig);
    }
    const std::string spent = scalar("timeSpent", "timeSpentSeconds");
    if (!spent.empty()) {
        bits.push_back("Spent " + spent);
    }
    const std::string rem = scalar("remainingEstimate", "remainingEstimateSeconds");
    if (!rem.empty()) {
        bits.push_back("Remaining " + rem);
    }
    if (!bits.empty()) {
        return JoinStrings(bits, " | ");
    }
    // Empty {} or object with no usable estimates/spent (show blank cell, not "{}" or raw JSON).
    return {};
}

namespace {

// Resolve the "parent issue" shape ({key, fields:{summary}}). Sets `handled`
// when this object owns a "key" string (even if both parts are empty, the
// original returned nothing and fell through — so handled stays false then).
std::string NormalizeParentRefObject(const nlohmann::json& value, bool& handled) {
    handled = false;
    if (!(value.contains("key") && value["key"].is_string())) {
        return std::string();
    }
    std::string parentKey = value["key"].get<std::string>();
    std::string parentSummary;
    if (value.contains("fields") && value["fields"].is_object()) {
        const auto& parentFields = value["fields"];
        if (parentFields.contains("summary") && parentFields["summary"].is_string()) {
            parentSummary = parentFields["summary"].get<std::string>();
        }
    }
    if (!parentKey.empty() && !parentSummary.empty()) {
        handled = true;
        return parentKey + " - " + parentSummary;
    }
    if (!parentKey.empty()) {
        handled = true;
        return parentKey;
    }
    if (!parentSummary.empty()) {
        handled = true;
        return parentSummary;
    }
    return std::string();
}

// Resolve an "id" field to its string form. Sets `handled` only when a non-empty
// string id or a numeric id is produced (matches the original fall-through).
std::string NormalizeIdObject(const nlohmann::json& value, bool& handled) {
    handled = false;
    if (!value.contains("id")) {
        return std::string();
    }
    if (value["id"].is_string()) {
        const std::string sid = value["id"].get<std::string>();
        if (!sid.empty()) {
            handled = true;
            return sid;
        }
    } else if (value["id"].is_number_integer()) {
        handled = true;
        return std::to_string(value["id"].get<long long>());
    } else if (value["id"].is_number_unsigned()) {
        handled = true;
        return std::to_string(value["id"].get<unsigned long long>());
    }
    return std::string();
}

// Dispatch the object-shaped variants of a tracker field value. Order is
// behaviour-significant and preserved verbatim from the original branch tower.
std::string NormalizeTrackerObjectValue(const nlohmann::json& value) {
    if (value.contains("comments") && value["comments"].is_array()) {
        return ParseComments(value["comments"]);
    }
    const auto typeIt = value.find("type");
    const bool isDoc = typeIt != value.end() && typeIt->is_string() && typeIt->get_ref<const std::string&>() == "doc";
    if (isDoc && value.contains("content") && value["content"].is_array()) {
        std::ostringstream out;
        ExtractAdfTextToStream(value, out);
        return TrimCopy(out.str());
    }

    bool handled = false;
    const std::string parentRef = NormalizeParentRefObject(value, handled);
    if (handled) {
        return parentRef;
    }

    if (value.contains("accountId") && value["accountId"].is_string()) {
        std::string displayName;
        if (value.contains("displayName") && value["displayName"].is_string()) {
            displayName = value["displayName"].get<std::string>();
        }
        return displayName.empty() ? value["accountId"].get<std::string>() : displayName;
    }
    if (value.contains("displayName") && value["displayName"].is_string()) {
        const std::string dn = value["displayName"].get<std::string>();
        if (!dn.empty()) {
            return dn;
        }
    }
    if (value.contains("value") && value["value"].is_string() && value.contains("child") &&
        value["child"].is_object()) {
        const std::string parentValue = value["value"].get<std::string>();
        const std::string childValue = BuildTrackerOptionDisplayValue(value["child"]);
        if (!parentValue.empty() && !childValue.empty()) {
            return parentValue + " > " + childValue;
        }
    }
    if (value.contains("value") && value["value"].is_string()) {
        const std::string vv = value["value"].get<std::string>();
        if (!vv.empty()) {
            return vv;
        }
    }
    if (value.contains("name") && value["name"].is_string()) {
        const std::string nm = value["name"].get<std::string>();
        if (!nm.empty()) {
            return nm;
        }
    }

    const std::string idStr = NormalizeIdObject(value, handled);
    if (handled) {
        return idStr;
    }

    if (JsonLooksLikeJiraTimetracking(value)) {
        return FormatTrackerTimetrackingDisplay(value);
    }
    return SafeJsonDump(value);
}

} // namespace

// Depth-bounded recursion. Field values are tracker-HTTP-sourced; with the upstream
// bounded parse the DOM is already <=256 deep, but this guard keeps the walker safe
// even if a value arrives from a not-yet-migrated parse path (defense-in-depth,
// matches json_safe::kDefaultMaxDepth). On cap we drop the deep subtree (return
// empty) rather than recurse — Pillar 3 graceful degradation.
static std::string NormalizeTrackerFieldValueDepth(const nlohmann::json& value, int depth);

std::string NormalizeTrackerFieldValue(const nlohmann::json& value) {
    return NormalizeTrackerFieldValueDepth(value, 0);
}

static std::string NormalizeTrackerFieldValueDepth(const nlohmann::json& value, int depth) {
    if (depth > 256) {
        return std::string(); // depth cap reached — stop recursing
    }
    if (value.is_null()) {
        return std::string();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    if (value.is_number_integer()) {
        return std::to_string(value.get<long long>());
    }
    if (value.is_number_unsigned()) {
        return std::to_string(value.get<unsigned long long>());
    }
    if (value.is_number_float()) {
        std::ostringstream oss;
        oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
        return TrimTrailingZeros(oss.str());
    }
    if (value.is_boolean()) {
        return value.get<bool>() ? "true" : "false";
    }
    if (value.is_object()) {
        return NormalizeTrackerObjectValue(value);
    }
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value) {
            const std::string normalized = NormalizeTrackerFieldValueDepth(item, depth + 1);
            if (!normalized.empty()) {
                parts.push_back(normalized);
            }
        }
        return JoinStrings(parts, ", ");
    }
    return SafeJsonDump(value);
}
std::string FormatWorkDurationFromSeconds(long long seconds) {
    if (seconds <= 0) {
        return std::string();
    }

    const long long minutesTotal = seconds / 60;
    const long long minutes = minutesTotal % 60;
    const long long hoursTotal = minutesTotal / 60;
    const long long hours = hoursTotal % 8;
    const long long daysTotal = hoursTotal / 8;
    const long long days = daysTotal % 5;
    const long long weeks = daysTotal / 5;

    std::string out;
    if (weeks > 0) {
        out += std::to_string(weeks) + "w ";
    }
    if (days > 0) {
        out += std::to_string(days) + "d ";
    }
    if (hours > 0 || out.empty()) {
        out += std::to_string(hours) + "h ";
    }
    if (minutes > 0) {
        out += std::to_string(minutes) + "m";
    }
    size_t start = 0;
    size_t end = out.size();
    while (start < end && (out[start] == ' ' || out[start] == '\t' || out[start] == '\n' || out[start] == '\r')) {
        ++start;
    }
    while (end > start &&
           (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\n' || out[end - 1] == '\r')) {
        --end;
    }
    return out.substr(start, end - start);
}
