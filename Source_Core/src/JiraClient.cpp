#include "JiraClient.h"

#include "Logger.h"
#include "NetworkUsageTracker.h"

#include <cpr/cpr.h>

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <cstddef>

// Defined below; used by timetracking formatting in the anonymous namespace.
std::string FormatWorkDurationFromSeconds(long long seconds);

namespace {
constexpr std::size_t kMaxJiraHttpBodyLogBytes = 65536;

void LogJiraHttpResult(const char* method, const std::string& url, const cpr::Response& response) {
    LOG_DEBUG("JiraClient: %s %s -> HTTP %d (%zu bytes)",
              method,
              url.c_str(),
              static_cast<int>(response.status_code),
              response.text.size());
    if (!Logger::Instance().GetLogJiraHttpBodies()) {
        return;
    }
    if (!Logger::Instance().ShouldLog(LogLevel::Trace)) {
        return;
    }
    std::string body = response.text;
    std::string suffix;
    if (body.size() > kMaxJiraHttpBodyLogBytes) {
        body.resize(kMaxJiraHttpBodyLogBytes);
        suffix = "\n[truncated…]";
    }
    Logger::Instance().Log(LogLevel::Trace,
                           std::string("JiraClient: ") + method + " response body (" + url + "):\n" + body +
                               suffix);
}

// Simple URL-encoder that is C++14 friendly.
std::string UrlEncode(const std::string& value) {
    std::ostringstream escaped;
    escaped.fill('0');
    escaped << std::hex << std::uppercase;

    for (unsigned char c : value) {
        // Unreserved characters according to RFC 3986
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            escaped << c;
        } else {
            escaped << '%' << std::setw(2) << int(c);
        }
    }
    return escaped.str();
}

// Base64 encode (RFC 4648) so we can send Authorization exactly like PowerShell/curl.
std::string Base64Encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(in[i]);
        const unsigned char b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned char c = (i + 2 < in.size()) ? static_cast<unsigned char>(in[i + 2]) : 0u;
        out += table[a >> 2];
        out += table[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? table[c & 63] : '=';
    }
    return out;
}

std::string NormalizeBaseUrl(const std::string& domain) {
    std::string base = domain;
    if (base.find("http://") != 0 && base.find("https://") != 0) {
        base = "https://" + base;
    }
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

std::string TrimCopy(const std::string& input) {
    size_t start = 0;
    size_t end = input.size();
    while (start < end && (input[start] == ' ' || input[start] == '\t' || input[start] == '\n' || input[start] == '\r')) {
        ++start;
    }
    while (end > start && (input[end - 1] == ' ' || input[end - 1] == '\t' || input[end - 1] == '\n' || input[end - 1] == '\r')) {
        --end;
    }
    return input.substr(start, end - start);
}

std::string JoinStrings(const std::vector<std::string>& items, const std::string& separator) {
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += separator;
        }
        out += items[i];
    }
    return out;
}

// nlohmann::value("k", std::string()) throws type_error.302 if "k" exists but is not a string.
std::string JsonGetStringIfString(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

std::string TruncateForLog(const std::string& s, std::size_t maxLen = 600) {
    if (s.size() <= maxLen) {
        return s;
    }
    return s.substr(0, maxLen) + "...";
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

void CollectAdfText(const nlohmann::json& node, std::vector<std::string>& out) {
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
            CollectAdfText(child, out);
        }
    }
}

void ExtractAdfTextToStream(const nlohmann::json& node, std::ostringstream& out) {
    if (node.is_array()) {
        for (const auto& child : node) {
            ExtractAdfTextToStream(child, out);
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
        ExtractAdfTextToStream(node["content"], out);
        if (nodeType == "paragraph" || nodeType == "heading" || nodeType == "listItem") {
            out << "\n";
        }
    }
}

std::string FormatDateIfIso(const std::string& value) {
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

std::string CleanCommentOutputAscii(const std::string& input) {
    std::string cleaned;
    cleaned.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(input[i]);
        if (i + 2 < input.size() &&
            c == 0xE2 &&
            static_cast<unsigned char>(input[i + 1]) == 0x80 &&
            static_cast<unsigned char>(input[i + 2]) == 0xA2) {
            cleaned += "* ";
            i += 2;
        } else {
            cleaned.push_back(static_cast<char>(c));
        }
    }
    return cleaned;
}

std::string ParseComments(const nlohmann::json& commentsArray) {
    const size_t kMaxComments = 20;
    const size_t kMaxTotalLength = 12000;
    const size_t kMaxDebugPreviewChars = 400;
    const size_t kMaxFallbackBodyChars = 4000;
    size_t emptyBodyDebugLogs = 0;

    if (!commentsArray.is_array() || commentsArray.empty()) {
        return std::string();
    }

    std::ostringstream result;
    size_t totalLength = 0;
    size_t commentCount = 0;

    for (auto it = commentsArray.rbegin(); it != commentsArray.rend() && commentCount < kMaxComments; ++it) {
        const auto& commentNode = *it;
        if (!commentNode.is_object()) {
            continue;
        }

        const std::string author = ParseCommentAuthor(commentNode);
        std::string date;
        if (commentNode.contains("created") && commentNode["created"].is_string()) {
            date = FormatDateIfIso(commentNode["created"].get<std::string>());
        }

        std::string commentText;
        const nlohmann::json* bodyNode = nullptr;
        if (commentNode.contains("body")) {
            const auto& body = commentNode["body"];
            bodyNode = &body;
            if (body.is_string()) {
                commentText = body.get<std::string>();
            } else if (body.is_object() && body.contains("content")) {
                std::ostringstream textStream;
                ExtractAdfTextToStream(body, textStream);
                commentText = TrimCopy(textStream.str());
                if (commentText.empty()) {
                    std::vector<std::string> fallbackParts;
                    CollectAdfText(body, fallbackParts);
                    commentText = JoinStrings(fallbackParts, " ");
                }
            }
        }

        if (commentText.empty()) {
            if (bodyNode != nullptr) {
                std::string bodyPreview = bodyNode->dump();
                if (bodyPreview.size() > kMaxDebugPreviewChars) {
                    bodyPreview.resize(kMaxDebugPreviewChars);
                }
                if (emptyBodyDebugLogs < 3) {
                    LOG_DEBUG("JiraClient: comment body was empty after extraction. body dump preview: %s", bodyPreview.c_str());
                    emptyBodyDebugLogs++;
                }
                if (bodyPreview.size() > kMaxFallbackBodyChars) {
                    bodyPreview.resize(kMaxFallbackBodyChars);
                }
            }
            continue;
        }

        commentText = CleanCommentOutputAscii(commentText);

        std::string entry = "[" + author + "] " + date + "\n" + commentText + "\n";

        if (totalLength + entry.length() > kMaxTotalLength) {
            break;
        }

        if (commentCount > 0) {
            result << "\n";
        }
        result << entry;
        totalLength += entry.length();
        commentCount++;
    }

    return result.str();
}

std::string ParseChangelog(const nlohmann::json& histories) {
    const size_t kMaxChangelogEntries = 60;
    const size_t kMaxValueLength = 200;
    const size_t kMaxRawChangelogChars = 16000;

    auto safeValueString = [&](const nlohmann::json& changeItem, const char* stringKey, const char* idKey) -> std::string {
        if (changeItem.contains(stringKey) && !changeItem[stringKey].is_null()) {
            const auto& value = changeItem[stringKey];
            if (value.is_string()) return value.get<std::string>();
            if (value.is_number_integer()) return std::to_string(value.get<long long>());
            if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
            if (value.is_number_float()) {
                std::ostringstream oss;
                oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
                return TrimTrailingZeros(oss.str());
            }
            if (value.is_boolean()) return value.get<bool>() ? "true" : "false";
            if (value.is_object() || value.is_array()) return value.dump();
        }

        if (changeItem.contains(idKey) && !changeItem[idKey].is_null()) {
            const auto& value = changeItem[idKey];
            if (value.is_string()) return value.get<std::string>();
            if (value.is_number_integer()) return std::to_string(value.get<long long>());
            if (value.is_number_unsigned()) return std::to_string(value.get<unsigned long long>());
            if (value.is_number_float()) {
                std::ostringstream oss;
                oss << std::setprecision(std::numeric_limits<double>::digits10 + 1) << value.get<double>();
                return TrimTrailingZeros(oss.str());
            }
        }

        return std::string();
    };

    if (!histories.is_array() || histories.empty()) {
        return std::string();
    }

    std::ostringstream formatted;
    size_t entryCount = 0;

    for (auto it = histories.begin(); it != histories.end() && entryCount < kMaxChangelogEntries; ++it) {
        const auto& history = *it;
        if (!history.is_object()) {
            continue;
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
            continue;
        }
        const auto& items = history["items"];

        for (const auto& changeItem : items) {
            if (!changeItem.is_object()) {
                continue;
            }
            std::string fieldName;
            if (changeItem.contains("field") && !changeItem["field"].is_null()) {
                const auto& f = changeItem["field"];
                fieldName = f.is_string() ? f.get<std::string>() : f.dump();
            }

            std::string fromValue = safeValueString(changeItem, "fromString", "from");
            std::string toValue = safeValueString(changeItem, "toString", "to");

            if (fromValue.size() > kMaxValueLength) {
                fromValue.resize(kMaxValueLength);
                fromValue += "...";
            }
            if (toValue.size() > kMaxValueLength) {
                toValue.resize(kMaxValueLength);
                toValue += "...";
            }

            if (entryCount > 0) {
                formatted << "\n";
            }
            formatted << "[" << author << "] " << created << "\n"
                      << fieldName << ": " << fromValue << " -> " << toValue << "\n";
            entryCount++;

            if (entryCount >= kMaxChangelogEntries) {
                break;
            }
        }
    }

    if (entryCount >= kMaxChangelogEntries) {
        formatted << "\n[... truncated ...]\n";
    }

    if (!formatted.str().empty()) {
        return formatted.str();
    }

    if (histories.is_array() && !histories.empty()) {
        std::string raw = histories.dump();
        if (raw.size() > kMaxRawChangelogChars) {
            raw.resize(kMaxRawChangelogChars);
            raw += "...";
        }
        return raw;
    }

    return std::string();
}

long long ParseWorkDurationToSeconds(const std::string& input) {
    std::string s = TrimCopy(input);
    if (s.empty()) {
        return 0;
    }

    // Support a plain number (seconds).
    bool allDigits = true;
    for (char ch : s) {
        if (!std::isdigit(static_cast<unsigned char>(ch))) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) {
        try {
            return std::stoll(s);
        } catch (...) {}
    }

    size_t pos = 0;
    long long total = 0;
    while (pos < s.size()) {
        while (pos < s.size() && s[pos] == ' ') pos++;
        if (pos >= s.size()) break;

        long long number = 0;
        if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
            size_t start = pos;
            while (pos < s.size() && std::isdigit(static_cast<unsigned char>(s[pos]))) pos++;
            try {
                number = std::stoll(s.substr(start, pos - start));
            } catch (...) { break; }
        } else {
            break;
        }

        if (pos >= s.size()) break;
        char u = s[pos++];
        if (u == 'w' || u == 'W') {
            total += number * 5 * 8 * 60 * 60;
        } else if (u == 'd' || u == 'D') {
            total += number * 8 * 60 * 60;
        } else if (u == 'h' || u == 'H') {
            total += number * 60 * 60;
        } else if (u == 'm' || u == 'M') {
            total += number * 60;
        } else {
            break;
        }
    }
    return total;
}

bool JsonLooksLikeJiraTimetracking(const nlohmann::json& o) {
    if (!o.is_object()) {
        return false;
    }
    static const char* keys[] = {"originalEstimate",
                                 "originalEstimateSeconds",
                                 "remainingEstimate",
                                 "remainingEstimateSeconds",
                                 "timeSpent",
                                 "timeSpentSeconds"};
    for (const char* k : keys) {
        if (o.contains(k)) {
            return true;
        }
    }
    return false;
}

// Jira returns timetracking as an object with human strings and parallel *Seconds fields.
std::string FormatJiraTimetrackingDisplay(const nlohmann::json& o) {
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

std::string NormalizeJiraFieldValue(const nlohmann::json& value) {
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
        if (value.contains("comments") && value["comments"].is_array()) {
            return ParseComments(value["comments"]);
        }
        const auto typeIt = value.find("type");
        const bool isDoc = typeIt != value.end() && typeIt->is_string() &&
                           typeIt->get_ref<const std::string&>() == "doc";
        if (isDoc && value.contains("content") && value["content"].is_array()) {
            std::ostringstream out;
            ExtractAdfTextToStream(value, out);
            return TrimCopy(out.str());
        }

        if (value.contains("key") && value["key"].is_string()) {
            std::string parentKey = value["key"].get<std::string>();
            std::string parentSummary;
            if (value.contains("fields") && value["fields"].is_object()) {
                const auto& parentFields = value["fields"];
                if (parentFields.contains("summary") && parentFields["summary"].is_string()) {
                    parentSummary = parentFields["summary"].get<std::string>();
                }
            }
            if (!parentKey.empty() && !parentSummary.empty()) {
                return parentKey + " - " + parentSummary;
            }
            if (!parentKey.empty()) {
                return parentKey;
            }
            if (!parentSummary.empty()) {
                return parentSummary;
            }
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
        if (value.contains("id")) {
            if (value["id"].is_string()) {
                const std::string sid = value["id"].get<std::string>();
                if (!sid.empty()) {
                    return sid;
                }
            } else if (value["id"].is_number_integer()) {
                return std::to_string(value["id"].get<long long>());
            } else if (value["id"].is_number_unsigned()) {
                return std::to_string(value["id"].get<unsigned long long>());
            }
        }
        if (JsonLooksLikeJiraTimetracking(value)) {
            return FormatJiraTimetrackingDisplay(value);
        }
        return value.dump();
    }
    if (value.is_array()) {
        std::vector<std::string> parts;
        for (const auto& item : value) {
            const std::string normalized = NormalizeJiraFieldValue(item);
            if (!normalized.empty()) {
                parts.push_back(normalized);
            }
        }
        return JoinStrings(parts, ", ");
    }
    return value.dump();
}
} // namespace

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
    while (start < end &&
           (out[start] == ' ' || out[start] == '\t' || out[start] == '\n' || out[start] == '\r')) {
        ++start;
    }
    while (end > start && (out[end - 1] == ' ' || out[end - 1] == '\t' || out[end - 1] == '\n' ||
                           out[end - 1] == '\r')) {
        --end;
    }
    return out.substr(start, end - start);
}

bool JiraClient::FetchUsers(const JiraConfig& cfg,
                            std::vector<JiraUser>& outUsers,
                            std::string& outError) {
    outUsers.clear();
    outError.clear();

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string usersUrl = base + "/rest/api/3/users/search?maxResults=1000";
    auto usersResponse = cpr::Get(cpr::Url{usersUrl}, headers, redirect);
    LogJiraHttpResult("GET", usersUrl, usersResponse);
    if (usersResponse.status_code != 200) {
        outError = "Failed to fetch users: HTTP " + std::to_string(usersResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        auto usersJson = nlohmann::json::parse(usersResponse.text);
        if (!usersJson.is_array()) {
            outError = "Invalid users response format.";
            return false;
        }

        std::set<std::string> seenAccountIds;
        std::set<std::string> seenDisplayNames;
        for (const auto& user : usersJson) {
            JiraUser jiraUser;
            jiraUser.AccountId = user.value("accountId", std::string());
            jiraUser.DisplayName = user.value("displayName", std::string());
            jiraUser.EmailAddress = user.value("emailAddress", std::string());
            jiraUser.Active = user.value("active", true);

            if (jiraUser.DisplayName.empty() ||
                jiraUser.AccountId.empty() ||
                !jiraUser.Active ||
                !seenAccountIds.insert(jiraUser.AccountId).second) {
                continue;
            }

            if (!seenDisplayNames.insert(jiraUser.DisplayName).second) {
                const std::string suffix = jiraUser.AccountId.substr(0, std::min<size_t>(4, jiraUser.AccountId.size()));
                jiraUser.DisplayName += "_" + suffix;
            }

            outUsers.push_back(std::move(jiraUser));
        }

        std::sort(outUsers.begin(), outUsers.end(), [](const JiraUser& a, const JiraUser& b) {
            const std::string& lhs = a.DisplayName;
            const std::string& rhs = b.DisplayName;
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(),
                rhs.begin(), rhs.end(),
                [](unsigned char ca, unsigned char cb) {
                    return std::tolower(ca) < std::tolower(cb);
                });
        });
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse users response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::FetchIssueWatchers(const JiraConfig& cfg,
                                    const std::string& issueKey,
                                    std::vector<JiraUser>& outWatchers,
                                    std::string& outError) {
    outWatchers.clear();
    outError.clear();

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/watchers";
    auto response = cpr::Get(cpr::Url{url}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, response);
    LogJiraHttpResult("GET", url, response);
    if (response.status_code != 200) {
        outError = "Failed to fetch watchers: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(response.text);
        if (!j.is_object()) {
            outError = "Invalid watchers response format.";
            return false;
        }
        const auto watchers = j.value("watchers", nlohmann::json::array());
        if (!watchers.is_array()) {
            outError = "Invalid watchers array in response.";
            return false;
        }
        for (const auto& w : watchers) {
            if (!w.is_object()) {
                continue;
            }
            JiraUser u;
            u.AccountId = w.value("accountId", std::string());
            u.DisplayName = w.value("displayName", std::string());
            u.EmailAddress = w.value("emailAddress", std::string());
            u.Active = w.value("active", true);
            if (!u.AccountId.empty() || !u.DisplayName.empty()) {
                outWatchers.push_back(std::move(u));
            }
        }
        std::sort(outWatchers.begin(), outWatchers.end(), [](const JiraUser& a, const JiraUser& b) {
            const std::string& lhs = a.DisplayName.empty() ? a.AccountId : a.DisplayName;
            const std::string& rhs = b.DisplayName.empty() ? b.AccountId : b.DisplayName;
            return std::lexicographical_compare(
                lhs.begin(), lhs.end(),
                rhs.begin(), rhs.end(),
                [](unsigned char ca, unsigned char cb) {
                    return std::tolower(ca) < std::tolower(cb);
                });
        });
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse watchers response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::FetchIssueVotes(const JiraConfig& cfg,
                                 const std::string& issueKey,
                                 std::vector<JiraUser>& outVoters,
                                 std::string& outError,
                                 int* outVoteCount,
                                 bool* outHasVoted,
                                 bool* outVotersArrayInResponse) {
    outVoters.clear();
    outError.clear();
    if (outVoteCount) {
        *outVoteCount = 0;
    }
    if (outHasVoted) {
        *outHasVoted = false;
    }
    if (outVotersArrayInResponse) {
        *outVotersArrayInResponse = false;
    }

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/votes";
    auto response = cpr::Get(cpr::Url{url}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, response);
    LogJiraHttpResult("GET", url, response);
    if (response.status_code != 200) {
        outError = "Failed to fetch votes: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        const auto j = nlohmann::json::parse(response.text);
        if (!j.is_object()) {
            outError = "Invalid votes response format.";
            return false;
        }

        if (outVoteCount && j.contains("votes")) {
            const auto& vc = j["votes"];
            if (vc.is_number_integer()) {
                *outVoteCount = static_cast<int>(vc.get<long long>());
            } else if (vc.is_number_unsigned()) {
                *outVoteCount = static_cast<int>(vc.get<unsigned long long>());
            } else if (vc.is_number_float()) {
                *outVoteCount = static_cast<int>(vc.get<double>());
            } else if (vc.is_string()) {
                try {
                    *outVoteCount = std::stoi(vc.get<std::string>());
                } catch (...) {}
            }
            if (*outVoteCount < 0) {
                *outVoteCount = 0;
            }
        }

        if (outHasVoted && j.contains("hasVoted")) {
            const auto& hv = j["hasVoted"];
            if (hv.is_boolean()) {
                *outHasVoted = hv.get<bool>();
            } else if (hv.is_number_integer()) {
                *outHasVoted = (hv.get<long long>() != 0);
            }
        }

        if (j.contains("voters")) {
            if (outVotersArrayInResponse) {
                *outVotersArrayInResponse = j["voters"].is_array();
            }
            const auto voters = j.value("voters", nlohmann::json::array());
            if (voters.is_array()) {
                for (const auto& w : voters) {
                    if (!w.is_object()) {
                        continue;
                    }
                    JiraUser u;
                    u.AccountId = w.value("accountId", std::string());
                    u.DisplayName = w.value("displayName", std::string());
                    u.EmailAddress = w.value("emailAddress", std::string());
                    u.Active = w.value("active", true);
                    if (!u.AccountId.empty() || !u.DisplayName.empty()) {
                        outVoters.push_back(std::move(u));
                    }
                }
                std::sort(outVoters.begin(), outVoters.end(), [](const JiraUser& a, const JiraUser& b) {
                    const std::string& lhs = a.DisplayName.empty() ? a.AccountId : a.DisplayName;
                    const std::string& rhs = b.DisplayName.empty() ? b.AccountId : b.DisplayName;
                    return std::lexicographical_compare(
                        lhs.begin(), lhs.end(),
                        rhs.begin(), rhs.end(),
                        [](unsigned char ca, unsigned char cb) {
                            return std::tolower(ca) < std::tolower(cb);
                        });
                });
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse votes response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    return true;
}

bool JiraClient::UpdateIssueFields(const std::string& issueId,
                                  const nlohmann::json& fields,
                                  std::string& outError) {
    outError.clear();

    JiraConfig cfg = ConfigManager::Load();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        return false;
    }
    if (!fields.is_object() || fields.empty()) {
        outError = "Update fields payload is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);

    cpr::Header headers{
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const auto iequals = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    };

    // Jira requires status updates via transitions endpoint, not field PUT.
    if (fields.is_object() && fields.size() == 1 && fields.contains("status")) {
        std::string targetStatusId;
        std::string targetStatusName;
        const auto& statusValue = fields["status"];
        if (statusValue.is_object()) {
            if (statusValue.contains("id")) {
                if (statusValue["id"].is_string()) {
                    targetStatusId = statusValue["id"].get<std::string>();
                } else if (statusValue["id"].is_number_integer()) {
                    targetStatusId = std::to_string(statusValue["id"].get<long long>());
                } else if (statusValue["id"].is_number_unsigned()) {
                    targetStatusId = std::to_string(statusValue["id"].get<unsigned long long>());
                }
            }
            if (statusValue.contains("name") && statusValue["name"].is_string()) {
                targetStatusName = statusValue["name"].get<std::string>();
            }
        } else if (statusValue.is_string()) {
            targetStatusName = statusValue.get<std::string>();
        }

        if (targetStatusId.empty() && targetStatusName.empty()) {
            outError = "Missing target status for transition.";
            return false;
        }

        const std::string transitionsUrl =
            base + "/rest/api/3/issue/" + UrlEncode(issueId) + "/transitions";
        auto transitionsResp = cpr::Get(cpr::Url{transitionsUrl}, headers, redirect);
        NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
            NetworkUsageTracker::kEstimatedGetUploadBytes, transitionsResp);
        LogJiraHttpResult("GET", transitionsUrl, transitionsResp);
        if (transitionsResp.status_code != 200) {
            outError = "Failed to fetch issue transitions: HTTP " + std::to_string(transitionsResp.status_code);
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return false;
        }

        std::string transitionId;
        try {
            auto transitionsJson = nlohmann::json::parse(transitionsResp.text);
            if (!transitionsJson.contains("transitions") || !transitionsJson["transitions"].is_array()) {
                outError = "Invalid transitions response format.";
                return false;
            }

            for (const auto& transition : transitionsJson["transitions"]) {
                if (!transition.is_object()) {
                    continue;
                }
                std::string thisTransitionId;
                if (transition.contains("id")) {
                    if (transition["id"].is_string()) {
                        thisTransitionId = transition["id"].get<std::string>();
                    } else if (transition["id"].is_number_integer()) {
                        thisTransitionId = std::to_string(transition["id"].get<long long>());
                    } else if (transition["id"].is_number_unsigned()) {
                        thisTransitionId = std::to_string(transition["id"].get<unsigned long long>());
                    }
                }
                if (thisTransitionId.empty()) {
                    continue;
                }

                std::string transitionName = transition.value("name", std::string());
                std::string toStatusId;
                std::string toStatusName;
                if (transition.contains("to") && transition["to"].is_object()) {
                    const auto& to = transition["to"];
                    if (to.contains("id")) {
                        if (to["id"].is_string()) {
                            toStatusId = to["id"].get<std::string>();
                        } else if (to["id"].is_number_integer()) {
                            toStatusId = std::to_string(to["id"].get<long long>());
                        } else if (to["id"].is_number_unsigned()) {
                            toStatusId = std::to_string(to["id"].get<unsigned long long>());
                        }
                    }
                    toStatusName = to.value("name", std::string());
                }

                if ((!targetStatusId.empty() && toStatusId == targetStatusId) ||
                    (!targetStatusName.empty() && iequals(toStatusName, targetStatusName)) ||
                    (!targetStatusName.empty() && iequals(transitionName, targetStatusName))) {
                    transitionId = thisTransitionId;
                    break;
                }
            }
        } catch (const std::exception& ex) {
            outError = std::string("Failed to parse transitions response: ") + ex.what();
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return false;
        }

        if (transitionId.empty()) {
            outError = "No valid transition found for requested status.";
            return false;
        }

        nlohmann::json transitionBody = {
            {"transition", {{"id", transitionId}}}
        };
        const std::string transitionBodyStr = transitionBody.dump();
        auto transitionResp = cpr::Post(
            cpr::Url{transitionsUrl},
            headers,
            cpr::Body{transitionBodyStr},
            redirect
        );
        NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
            static_cast<std::uint64_t>(transitionBodyStr.size()), transitionResp);
        LogJiraHttpResult("POST", transitionsUrl, transitionResp);
        if (transitionResp.status_code != 204 && transitionResp.status_code != 200) {
            outError = "Failed to transition issue status: HTTP " + std::to_string(transitionResp.status_code);
            LOG_ERROR("JiraClient: %s", outError.c_str());
            return false;
        }

        LOG_INFO("JiraClient: transitioned Jira issue %s status successfully.", issueId.c_str());
        return true;
    }

    // Jira expects time-tracking fields as integer seconds, not strings.
    static const std::set<std::string> kTimeTrackingFieldIds = {
        "timeoriginalestimate", "timeestimate", "timespent",
        "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"
    };
    nlohmann::json fieldsNormalized = fields;
    for (const std::string& fieldId : kTimeTrackingFieldIds) {
        if (!fieldsNormalized.contains(fieldId)) continue;
        const auto& v = fieldsNormalized[fieldId];
        if (v.is_string()) {
            std::string s = TrimCopy(v.get<std::string>());
            if (s.empty()) {
                fieldsNormalized[fieldId] = nullptr;
            } else {
                long long sec = ParseWorkDurationToSeconds(s);
                fieldsNormalized[fieldId] = sec;
            }
        } else if (v.is_null()) {
            fieldsNormalized[fieldId] = nullptr;
        }
        // else already number, leave as-is
    }

    nlohmann::json body = nlohmann::json::object();
    body["fields"] = fieldsNormalized;
    const std::string updateUrl = base + "/rest/api/3/issue/" + UrlEncode(issueId);
    const std::string putBodyStr = body.dump();
    auto response = cpr::Put(
        cpr::Url{updateUrl},
        headers,
        cpr::Body{putBodyStr},
        redirect
    );
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        static_cast<std::uint64_t>(putBodyStr.size()), response);
    LogJiraHttpResult("PUT", updateUrl, response);

    if (response.status_code != 204 && response.status_code != 200) {
        outError = "Failed to update issue fields: HTTP " + std::to_string(response.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        LOG_DEBUG("JiraClient: update payload:\n%s", body.dump(2).c_str());
        return false;
    }

    LOG_INFO("JiraClient: updated Jira issue %s fields successfully.", issueId.c_str());
    return true;
}

bool JiraClient::FetchFieldCatalog(const JiraConfig& cfg,
                                  std::vector<JiraField>& outFields,
                                  std::vector<JiraComponent>& outComponents,
                                  std::string& outError) {
    outFields.clear();
    outComponents.clear();
    outError.clear();

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string fieldsListUrl = base + "/rest/api/3/field";
    auto fieldsResponse = cpr::Get(cpr::Url{fieldsListUrl}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, fieldsResponse);
    LogJiraHttpResult("GET", fieldsListUrl, fieldsResponse);
    if (fieldsResponse.status_code != 200) {
        outError = "Failed to fetch fields: HTTP " + std::to_string(fieldsResponse.status_code);
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    try {
        auto response = nlohmann::json::parse(fieldsResponse.text);
        if (!response.is_array()) {
            outError = "Unexpected /field response shape.";
            return false;
        }

        for (const auto& field : response) {
            if (!field.contains("id") || !field.contains("name") ||
                !field["id"].is_string() || !field["name"].is_string()) {
                continue;
            }

            const std::string fieldId = field["id"].get<std::string>();

            JiraField jiraField;
            jiraField.Id = fieldId;
            jiraField.Name = field["name"].get<std::string>();
            jiraField.Type = "unknown";
            jiraField.ReadOnly = field.value("isLocked", false);

            if (field.contains("schema") && field["schema"].is_object()) {
                const auto& schema = field["schema"];
                if (schema.contains("type") && schema["type"].is_string()) {
                    jiraField.Type = schema["type"].get<std::string>();
                }

                jiraField.IsArray = (jiraField.Type == "array");
                if (jiraField.IsArray && schema.contains("items")) {
                    if (schema["items"].is_string()) {
                        jiraField.ItemsType = schema["items"].get<std::string>();
                    } else if (schema["items"].is_object() &&
                               schema["items"].contains("type") &&
                               schema["items"]["type"].is_string()) {
                        jiraField.ItemsType = schema["items"]["type"].get<std::string>();
                    }
                }
                jiraField.IsUserType =
                    (jiraField.Type == "user") || (jiraField.IsArray && jiraField.ItemsType == "user");
            }

            jiraField.IsCustom = (fieldId.find("customfield_") == 0);
            try {
                jiraField.RestFieldDefinitionJson = field.dump(2);
            } catch (...) {
                jiraField.RestFieldDefinitionJson.clear();
            }
            outFields.push_back(std::move(jiraField));
        }
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse /field response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        return false;
    }

    if (cfg.ProjectKey.empty()) {
        LOG_INFO("JiraClient: skipping createmeta enrichment because project key is empty.");
        return true;
    }

    std::unordered_map<std::string, size_t> fieldIndexById;
    for (size_t i = 0; i < outFields.size(); ++i) {
        fieldIndexById[outFields[i].Id] = i;
    }

    const std::string metaUrl = base +
        "/rest/api/3/issue/createmeta?projectKeys=" + UrlEncode(cfg.ProjectKey) +
        "&expand=projects.issuetypes.fields";
    auto metaResponse = cpr::Get(cpr::Url{metaUrl}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, metaResponse);
    LogJiraHttpResult("GET", metaUrl, metaResponse);
    std::set<std::string> uniqueIssueTypes;
    if (metaResponse.status_code == 200) {
        try {
            auto metaJson = nlohmann::json::parse(metaResponse.text);
            std::set<std::string> uniqueComponentIds;

        if (metaJson.contains("projects") && metaJson["projects"].is_array()) {
            for (const auto& project : metaJson["projects"]) {
                if (!project.contains("issuetypes") || !project["issuetypes"].is_array()) {
                    continue;
                }

                for (const auto& issueType : project["issuetypes"]) {
                    if (issueType.contains("name") && issueType["name"].is_string()) {
                        uniqueIssueTypes.insert(issueType["name"].get<std::string>());
                    }

                    if (!issueType.contains("fields") || !issueType["fields"].is_object()) {
                        continue;
                    }

                    for (auto it = issueType["fields"].begin(); it != issueType["fields"].end(); ++it) {
                        const std::string fieldId = it.key();
                        const auto& fieldObj = it.value();

                        if (fieldId == "components" &&
                            fieldObj.contains("allowedValues") &&
                            fieldObj["allowedValues"].is_array()) {
                            for (const auto& val : fieldObj["allowedValues"]) {
                                if (val.contains("id") && val.contains("name") &&
                                    val["id"].is_string() && val["name"].is_string()) {
                                    const std::string componentId = val["id"].get<std::string>();
                                    if (uniqueComponentIds.insert(componentId).second) {
                                        JiraComponent component;
                                        component.Id = componentId;
                                        component.Name = val["name"].get<std::string>();
                                        outComponents.push_back(component);
                                    }
                                }
                            }
                        }

                        if (!fieldObj.contains("allowedValues") || !fieldObj["allowedValues"].is_array()) {
                            continue;
                        }

                        const auto indexIt = fieldIndexById.find(fieldId);
                        if (indexIt == fieldIndexById.end()) {
                            continue;
                        }

                        // issuetype allowedValues in createmeta are per-screen (e.g. Epic may only list Epic).
                        // Use the full project issue type name set after this loop instead.
                        if (fieldId == "issuetype") {
                            continue;
                        }

                        JiraField& targetField = outFields[indexIt->second];
                        if (!targetField.AllowedValues.empty()) {
                            continue;
                        }

                        for (const auto& val : fieldObj["allowedValues"]) {
                            std::string optionId;
                            std::string optionValue;

                            if (val.is_string()) {
                                optionValue = val.get<std::string>();
                                optionId = optionValue;
                            } else if (val.is_object()) {
                                if (val.contains("id")) {
                                    if (val["id"].is_string()) {
                                        optionId = val["id"].get<std::string>();
                                    } else if (val["id"].is_number_integer()) {
                                        optionId = std::to_string(val["id"].get<long long>());
                                    } else if (val["id"].is_number_unsigned()) {
                                        optionId = std::to_string(val["id"].get<unsigned long long>());
                                    }
                                }

                                if (val.contains("value") && val["value"].is_string()) {
                                    optionValue = val["value"].get<std::string>();
                                } else if (val.contains("name") && val["name"].is_string()) {
                                    optionValue = val["name"].get<std::string>();
                                }

                                if (optionValue.empty() && !optionId.empty()) {
                                    optionValue = optionId;
                                }
                            }

                            if (!optionValue.empty()) {
                                targetField.AllowedValues.push_back(optionValue);
                                JiraFieldOption option;
                                option.Id = optionId.empty() ? optionValue : optionId;
                                option.Value = optionValue;
                                targetField.AllowedValueOptions.push_back(option);
                            }
                        }
                    }
                }
            }
        }
        } catch (const std::exception& ex) {
            LOG_WARN("JiraClient: createmeta parse failed: %s", ex.what());
        }
    } else {
        LOG_WARN("JiraClient: createmeta enrichment failed. HTTP %d", metaResponse.status_code);
    }

    // Issue type: createmeta often lists only one type (e.g. Epic). Prefer GET /project/{key}
    // issueTypes (full list + real ids for PUT). Fall back to createmeta names if needed.
    {
        const auto issueTypeIt = fieldIndexById.find("issuetype");
        if (issueTypeIt != fieldIndexById.end()) {
            JiraField& issueTypeField = outFields[issueTypeIt->second];
            bool filledFromProject = false;
            try {
                const std::string projectUrl = base + "/rest/api/3/project/" + UrlEncode(cfg.ProjectKey);
                auto projectResp = cpr::Get(cpr::Url{projectUrl}, headers, redirect);
                NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
                    NetworkUsageTracker::kEstimatedGetUploadBytes, projectResp);
                LogJiraHttpResult("GET", projectUrl, projectResp);
                if (projectResp.status_code == 200) {
                    auto projectJson = nlohmann::json::parse(projectResp.text);
                    if (projectJson.contains("issueTypes") && projectJson["issueTypes"].is_array()) {
                        std::vector<JiraFieldOption> opts;
                        for (const auto& it : projectJson["issueTypes"]) {
                            if (!it.is_object()) {
                                continue;
                            }
                            std::string tid;
                            if (it.contains("id")) {
                                if (it["id"].is_string()) {
                                    tid = it["id"].get<std::string>();
                                } else if (it["id"].is_number_integer()) {
                                    tid = std::to_string(it["id"].get<long long>());
                                } else if (it["id"].is_number_unsigned()) {
                                    tid = std::to_string(it["id"].get<unsigned long long>());
                                }
                            }
                            const std::string tname = it.value("name", std::string());
                            if (tid.empty() || tname.empty()) {
                                continue;
                            }
                            JiraFieldOption option;
                            option.Id = std::move(tid);
                            option.Value = tname;
                            opts.push_back(std::move(option));
                        }
                        if (!opts.empty()) {
                            std::sort(opts.begin(), opts.end(),
                                      [](const JiraFieldOption& a, const JiraFieldOption& b) {
                                          return a.Value < b.Value;
                                      });
                            issueTypeField.AllowedValueOptions = std::move(opts);
                            issueTypeField.AllowedValues.clear();
                            for (const auto& o : issueTypeField.AllowedValueOptions) {
                                issueTypeField.AllowedValues.push_back(o.Value);
                            }
                            filledFromProject = true;
                        }
                    }
                } else {
                    LOG_WARN("JiraClient: project fetch for issue types failed. HTTP %d",
                             projectResp.status_code);
                }
            } catch (const std::exception& ex) {
                LOG_WARN("JiraClient: project issueTypes parse failed: %s", ex.what());
            }

            if (!filledFromProject && !uniqueIssueTypes.empty()) {
                issueTypeField.AllowedValues.assign(uniqueIssueTypes.begin(), uniqueIssueTypes.end());
                std::sort(issueTypeField.AllowedValues.begin(), issueTypeField.AllowedValues.end());
                issueTypeField.AllowedValueOptions.clear();
                for (const auto& issueTypeName : issueTypeField.AllowedValues) {
                    JiraFieldOption option;
                    option.Id = issueTypeName;
                    option.Value = issueTypeName;
                    issueTypeField.AllowedValueOptions.push_back(option);
                }
            }
        }
    }

    // Enrich status options so the UI renders a dropdown.
    try {
        const auto statusFieldIt = fieldIndexById.find("status");
        if (statusFieldIt != fieldIndexById.end()) {
            const std::string statusCatalogUrl = base + "/rest/api/3/status";
            auto statusResp = cpr::Get(cpr::Url{statusCatalogUrl}, headers, redirect);
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
                NetworkUsageTracker::kEstimatedGetUploadBytes, statusResp);
            LogJiraHttpResult("GET", statusCatalogUrl, statusResp);
            if (statusResp.status_code == 200) {
                auto statusJson = nlohmann::json::parse(statusResp.text);
                if (statusJson.is_array()) {
                    JiraField& statusField = outFields[statusFieldIt->second];
                    statusField.AllowedValues.clear();
                    statusField.AllowedValueOptions.clear();
                    std::set<std::string> seenIds;
                    for (const auto& statusObj : statusJson) {
                        if (!statusObj.is_object()) {
                            continue;
                        }

                        std::string statusId;
                        if (statusObj.contains("id")) {
                            if (statusObj["id"].is_string()) {
                                statusId = statusObj["id"].get<std::string>();
                            } else if (statusObj["id"].is_number_integer()) {
                                statusId = std::to_string(statusObj["id"].get<long long>());
                            } else if (statusObj["id"].is_number_unsigned()) {
                                statusId = std::to_string(statusObj["id"].get<unsigned long long>());
                            }
                        }
                        const std::string statusName = statusObj.value("name", std::string());
                        if (statusId.empty() || statusName.empty() || !seenIds.insert(statusId).second) {
                            continue;
                        }

                        statusField.AllowedValues.push_back(statusName);
                        JiraFieldOption option;
                        option.Id = statusId;
                        option.Value = statusName;
                        statusField.AllowedValueOptions.push_back(std::move(option));
                    }
                }
            } else {
                LOG_WARN("JiraClient: status catalog enrichment failed. HTTP %d", statusResp.status_code);
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("JiraClient: status catalog parse failed: %s", ex.what());
    }

    return true;
}

std::vector<CachedTicket> JiraClient::FetchIssues(bool* outFullSyncCompleted,
                                                  const JiraConfig* configOverride,
                                                  const ViewsStore* viewsOverride) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }

    JiraConfig cfgStorage;
    const JiraConfig* cfgPtr = configOverride;
    if (!cfgPtr) {
        cfgStorage = ConfigManager::Load();
        cfgPtr = &cfgStorage;
    }
    const JiraConfig& cfg = *cfgPtr;

    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        LOG_WARN("JiraClient: missing API token or domain; skipping FetchIssues.");
        return {};
    }

    std::string base = NormalizeBaseUrl(cfg.Domain);

    std::string jqlRaw = cfg.JqlQuery.empty() ? std::string("assignee=currentUser()") : cfg.JqlQuery;
    // Trim leading/trailing whitespace so JQL matches what you'd paste in the browser.
    while (!jqlRaw.empty() && (jqlRaw.front() == ' ' || jqlRaw.front() == '\t')) jqlRaw.erase(0, 1);
    while (!jqlRaw.empty() && (jqlRaw.back() == ' ' || jqlRaw.back() == '\t')) jqlRaw.pop_back();
    const std::string jqlEncoded = UrlEncode(jqlRaw);
    std::vector<std::string> fieldsList{
        "summary", "description", "status", "assignee", "priority", "issuetype", "parent", "comment", "changelog"
    };
    std::unordered_set<std::string> seenFields(fieldsList.begin(), fieldsList.end());
    std::vector<std::string> selectedFields;
    std::unordered_set<std::string> seenSelectedFields;

    // Prefer the active view's field list as the single source of truth.
    ViewsStore viewsStorage;
    const ViewsStore* viewsPtr = viewsOverride;
    if (!viewsPtr) {
        viewsStorage = ConfigManager::LoadViewsOrBootstrap(cfg);
        viewsPtr = &viewsStorage;
    }
    const ViewsStore& viewStore = *viewsPtr;

    const ViewDefinition* activeViewDef = nullptr;
    for (const auto& view : viewStore.Views) {
        if (view.Id == viewStore.ActiveViewId) {
            activeViewDef = &view;
            break;
        }
    }
    if (!activeViewDef && !viewStore.Views.empty()) {
        activeViewDef = &viewStore.Views.front();
    }
    if (activeViewDef) {
        for (const auto& rawField : activeViewDef->Fields) {
            const std::string field = TrimCopy(rawField);
            if (field.empty()) {
                continue;
            }
            if (field == "history") {
                if (seenSelectedFields.insert(field).second) {
                    selectedFields.push_back(field);
                }
                continue;
            }
            if (!seenSelectedFields.insert(field).second) {
                continue;
            }
            if (seenFields.insert(field).second) {
                fieldsList.push_back(field);
                selectedFields.push_back(field);
            } else {
                selectedFields.push_back(field);
            }
        }
    }
    // Fallback for cases where the active view has no explicit fields:
    // default to the Basic Fields set so the grid has useful values.
    if (selectedFields.empty()) {
        const char* kBasicDefaults[] = {
            "summary",
            "assignee",
            "priority",
            "status",
            "created",
            "updated"
        };
        for (const char* basicId : kBasicDefaults) {
            if (seenFields.insert(basicId).second) {
                fieldsList.push_back(basicId);
            }
            if (seenSelectedFields.insert(basicId).second) {
                selectedFields.emplace_back(basicId);
            }
        }
    }
    const std::string fields = JoinStrings(fieldsList, ",");

    // Use the new Jira Cloud search endpoint as per Atlassian migration guidance.
    std::string baseSearchUrl = base +
        "/rest/api/3/search/jql?jql=" + jqlEncoded +
        "&maxResults=100&fields=" + fields +
        "&expand=changelog";

    // Build Basic auth exactly as PowerShell/curl: Authorization header sent on every request.
    std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);
    auto fetchIssueComments = [&](const std::string& issueKey, nlohmann::json& outComments) -> bool {
        outComments = nlohmann::json::array();
        int startAt = 0;
        const int maxResults = 100;
        const int maxPages = 20;

        for (int page = 0; page < maxPages; ++page) {
            const std::string commentsUrl =
                base + "/rest/api/3/issue/" + UrlEncode(issueKey) +
                "/comment?startAt=" + std::to_string(startAt) +
                "&maxResults=" + std::to_string(maxResults);
            auto commentsResp = cpr::Get(cpr::Url{commentsUrl}, headers, redirect);
            NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
                NetworkUsageTracker::kEstimatedGetUploadBytes, commentsResp);
            LogJiraHttpResult("GET", commentsUrl, commentsResp);
            if (commentsResp.status_code != 200) {
                LOG_WARN("JiraClient: failed to fetch comments for issue %s. HTTP %d",
                         issueKey.c_str(), commentsResp.status_code);
                return !outComments.empty();
            }

            try {
                auto commentsJson = nlohmann::json::parse(commentsResp.text);
                if (!commentsJson.contains("comments") || !commentsJson["comments"].is_array()) {
                    LOG_WARN("JiraClient: comments endpoint for %s missing comments array.", issueKey.c_str());
                    return !outComments.empty();
                }

                const auto& pageComments = commentsJson["comments"];
                for (const auto& c : pageComments) {
                    outComments.push_back(c);
                }

                const int total = commentsJson.value("total", static_cast<int>(outComments.size()));
                const int pageCount = static_cast<int>(pageComments.size());
                const int reportedMaxResults = commentsJson.value("maxResults", pageCount);
                startAt += (reportedMaxResults > 0 ? reportedMaxResults : pageCount);

                if (static_cast<int>(outComments.size()) >= total || pageCount == 0) {
                    break;
                }
            } catch (const std::exception& ex) {
                LOG_WARN("JiraClient: failed to parse comments for issue %s: %s", issueKey.c_str(), ex.what());
                return !outComments.empty();
            }
        }
        return true;
    };

    std::vector<CachedTicket> results;
    std::string nextPageToken;
    std::string lastResponseBody;
    const int kMaxPages = 50;
    int fetchedPages = 0;
    bool syncEndedCleanly = false;

    for (int page = 1; page <= kMaxPages; ++page) {
        std::string pageUrl = baseSearchUrl;
        if (!nextPageToken.empty()) {
            pageUrl += "&nextPageToken=" + UrlEncode(nextPageToken);
        }
        LOG_DEBUG("JiraClient: fetching issues page %d from URL: %s", page, pageUrl.c_str());

        auto response = cpr::Get(cpr::Url{pageUrl}, headers, redirect);
        NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
            NetworkUsageTracker::kEstimatedGetUploadBytes, response);
        LogJiraHttpResult("GET", pageUrl, response);
        lastResponseBody = response.text;
        if (response.status_code != 200) {
            LOG_ERROR("JiraClient: failed to fetch issues page %d. HTTP %d, error code %d.",
                      page, response.status_code, static_cast<int>(response.error.code));
            if (response.status_code == 401 || response.status_code == 403 ||
                (response.text.find("authenticated") != std::string::npos)) {
                LOG_WARN("JiraClient: login failed. Check Email and API token in Settings -> Jira Credentials.");
            }
            LOG_DEBUG("JiraClient: response error message: %s", response.error.message.c_str());
            break;
        }

        fetchedPages++;
        try {
            auto json = nlohmann::json::parse(response.text);
            if (!json.contains("issues")) {
                LOG_WARN("JiraClient: page %d response has no 'issues' key. Full response.text:\n%s",
                         page, response.text.c_str());
                syncEndedCleanly = false;
                break;
            }

            const size_t before = results.size();
            for (auto& issue : json["issues"]) {
                try {
                CachedTicket ticket;
                ticket.id = JsonGetStringIfString(issue, "key");

                nlohmann::json issueFields = issue.value("fields", nlohmann::json::object());
                const auto stringifyForGrid = [&](const nlohmann::json& v) -> std::string {
                    if (v.is_object() || v.is_array()) {
                        return v.dump();
                    }
                    return NormalizeJiraFieldValue(v);
                };

                for (const auto& fieldKey : selectedFields) {
                    if (fieldKey == "history") {
                        const nlohmann::json changelog = issue.value("changelog", nlohmann::json::object());
                        const nlohmann::json histories = changelog.value("histories", nlohmann::json::array());
                        ticket.fieldValues["history"] = ParseChangelog(histories);
                        continue;
                    }
                    if (fieldKey == "watchers") {
                        if (issueFields.contains("watchers")) {
                            ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watchers"]);
                        } else if (issueFields.contains("watches")) {
                            ticket.fieldValues["watchers"] = stringifyForGrid(issueFields["watches"]);
                        } else {
                            ticket.fieldValues["watchers"] = std::string();
                        }
                        continue;
                    }
                    if (issueFields.contains(fieldKey)) {
                        const auto& rawValue = issueFields[fieldKey];
                        if (fieldKey == "comment" &&
                            rawValue.is_object() &&
                            rawValue.contains("comments")) {
                            const auto& commentObj = rawValue;
                            const auto& commentsArray = commentObj["comments"];
                            const int totalComments = commentObj.value(
                                "total", static_cast<int>(commentsArray.size()));
                            if (commentsArray.is_array() && !commentsArray.empty()) {
                                ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                            } else if (totalComments > 0) {
                                nlohmann::json fetchedComments = nlohmann::json::array();
                                if (fetchIssueComments(ticket.id, fetchedComments) && fetchedComments.is_array()) {
                                    ticket.fieldValues[fieldKey] = ParseComments(fetchedComments);
                                } else {
                                    ticket.fieldValues[fieldKey] = ParseComments(commentsArray);
                                }
                            } else {
                                ticket.fieldValues[fieldKey] = std::string();
                            }
                        } else if (fieldKey == "timetracking" || fieldKey == "aggregatetimetracking") {
                            if (rawValue.is_object()) {
                                ticket.fieldValues[fieldKey] = FormatJiraTimetrackingDisplay(rawValue);
                            } else {
                                ticket.fieldValues[fieldKey] = NormalizeJiraFieldValue(rawValue);
                            }
                        } else if (fieldKey == "timeoriginalestimate" ||
                                   fieldKey == "timeestimate" ||
                                   fieldKey == "timespent" ||
                                   fieldKey == "aggregatetimeoriginalestimate" ||
                                   fieldKey == "aggregatetimeestimate" ||
                                   fieldKey == "aggregatetimespent") {
                            long long seconds = 0;
                            if (rawValue.is_number_integer()) {
                                seconds = rawValue.get<long long>();
                            } else if (rawValue.is_number_unsigned()) {
                                seconds = static_cast<long long>(rawValue.get<unsigned long long>());
                            }
                            ticket.fieldValues[fieldKey] = FormatWorkDurationFromSeconds(seconds);
                        } else {
                            ticket.fieldValues[fieldKey] = NormalizeJiraFieldValue(rawValue);
                        }
                    } else {
                        ticket.fieldValues[fieldKey] = std::string();
                    }
                }
                // Always persist issuetype when Jira returned it (search includes it in `fields=` even if
                // the active view did not list it in selectedFields, so maps stay consistent for the grid).
                if (issueFields.contains("issuetype")) {
                    ticket.fieldValues["issuetype"] = NormalizeJiraFieldValue(issueFields["issuetype"]);
                }
                results.push_back(std::move(ticket));
                } catch (const std::exception& ex) {
                    const std::string issueKey = issue.is_object() ? JsonGetStringIfString(issue, "key") : std::string();
                    LOG_ERROR("JiraClient: JSON error on page %d while parsing issue %s: %s",
                              page,
                              issueKey.empty() ? "(unknown key)" : issueKey.c_str(),
                              ex.what());
                    syncEndedCleanly = false;
                    continue;
                }
            }
            const size_t added = results.size() - before;
            LOG_INFO("JiraClient: fetched page %d with %zu issues (total=%zu).", page, added, results.size());

            const bool isLast = json.value("isLast", true);
            std::string newToken = JsonGetStringIfString(json, "nextPageToken");
            if (json.contains("nextPageToken") && !json["nextPageToken"].is_null() &&
                !json["nextPageToken"].is_string()) {
                LOG_WARN("JiraClient: page %d nextPageToken is not a string (JSON type: %s); pagination may stop.",
                         page,
                         json["nextPageToken"].type_name());
            }
            if (isLast) {
                syncEndedCleanly = true;
                break;
            }
            if (newToken.empty()) {
                LOG_WARN("JiraClient: page %d indicates more pages but nextPageToken is empty. Stopping pagination.", page);
                syncEndedCleanly = false;
                break;
            }
            nextPageToken = newToken;
        } catch (const std::exception& ex) {
            LOG_ERROR("JiraClient: JSON parse error on page %d: %s | response excerpt: %s",
                      page,
                      ex.what(),
                      TruncateForLog(lastResponseBody).c_str());
            syncEndedCleanly = false;
            break;
        }
    }

    if (fetchedPages >= kMaxPages && !nextPageToken.empty()) {
        LOG_WARN("JiraClient: reached pagination safety limit (%d pages). Results may be incomplete.", kMaxPages);
    }

    if (outFullSyncCompleted) {
        *outFullSyncCompleted = syncEndedCleanly && fetchedPages > 0;
    }

    if (results.empty()) {
        // Confirm which account the API sees. This distinguishes authenticated-vs-anonymous 200 responses.
        std::string who = cfg.Email;
        bool verifiedIdentity = false;
        std::string myselfUrl = base + "/rest/api/3/myself";
        auto myselfResp = cpr::Get(cpr::Url{myselfUrl}, headers, redirect);
        NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
            NetworkUsageTracker::kEstimatedGetUploadBytes, myselfResp);
        LogJiraHttpResult("GET", myselfUrl, myselfResp);
        if (myselfResp.status_code == 200) {
            try {
                auto me = nlohmann::json::parse(myselfResp.text);
                std::string name = me.value("displayName", std::string());
                std::string emailAddr = me.value("emailAddress", std::string());
                who = name.empty() ? emailAddr : (name + " <" + emailAddr + ">");
                verifiedIdentity = true;
            } catch (...) {}
        }

        if (verifiedIdentity) {
            LOG_WARN("JiraClient: API returned 0 issues for your JQL after %d page(s). Verified identity: %s. "
                     "If the same URL in the browser shows issues, compare browser account/permissions with this API identity.",
                     fetchedPages, who.c_str());
        } else {
            LOG_WARN("JiraClient: API returned 0 issues and /myself was not authenticated (HTTP %d). "
                     "This usually means the app sent invalid credentials (often a truncated API token). "
                     "Re-open Settings -> Jira Credentials and paste the full API token.",
                     myselfResp.status_code);
        }
    }
    return results;
}

namespace {

nlohmann::json AdfDocumentFromPlainText(const std::string& plainText) {
    nlohmann::json content = nlohmann::json::array();
    std::string line;
    std::istringstream iss(plainText);
    while (std::getline(iss, line)) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        nlohmann::json inner = nlohmann::json::array();
        nlohmann::json textNode = nlohmann::json::object();
        textNode["type"] = "text";
        textNode["text"] = line;
        inner.push_back(std::move(textNode));
        para["content"] = std::move(inner);
        content.push_back(std::move(para));
    }
    if (content.empty()) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", ""}}});
        content.push_back(std::move(para));
    }
    return nlohmann::json{{"type", "doc"}, {"version", 1}, {"content", std::move(content)}};
}

void ParseJiraUserObject(const nlohmann::json& user, JiraUser& out) {
    out.AccountId = user.value("accountId", std::string());
    out.DisplayName = user.value("displayName", std::string());
    out.EmailAddress = user.value("emailAddress", std::string());
    out.Active = user.value("active", true);
}

} // namespace

bool JiraClient::SearchUsersByQuery(const JiraConfig& cfg,
                                    const std::string& query,
                                    std::vector<JiraUser>& outUsers,
                                    std::string& outError) {
    outUsers.clear();
    outError.clear();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (query.empty()) {
        return true;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string url =
        base + "/rest/api/3/user/search?query=" + UrlEncode(query) + "&maxResults=100";
    auto resp = cpr::Get(cpr::Url{url}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, resp);
    LogJiraHttpResult("GET", url, resp);
    if (resp.status_code != 200) {
        outError = "user/search failed: HTTP " + std::to_string(resp.status_code);
        return false;
    }

    try {
        auto arr = nlohmann::json::parse(resp.text);
        if (!arr.is_array()) {
            outError = "user/search: expected array.";
            return false;
        }
        std::unordered_set<std::string> seen;
        for (const auto& node : arr) {
            if (!node.is_object()) {
                continue;
            }
            JiraUser u;
            ParseJiraUserObject(node, u);
            if (u.AccountId.empty() || !u.Active || !seen.insert(u.AccountId).second) {
                continue;
            }
            outUsers.push_back(std::move(u));
        }
    } catch (const std::exception& ex) {
        outError = std::string("user/search parse error: ") + ex.what();
        return false;
    }
    return true;
}

bool JiraClient::AddIssueCommentPlain(const JiraConfig& cfg,
                                      const std::string& issueKey,
                                      const std::string& plainText,
                                      std::string& outError) {
    outError.clear();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    nlohmann::json body = nlohmann::json::object();
    body["body"] = AdfDocumentFromPlainText(plainText);
    const std::string postUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/comment";
    const std::string bodyStr = body.dump();
    auto response = cpr::Post(cpr::Url{postUrl}, headers, cpr::Body{bodyStr}, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        static_cast<std::uint64_t>(bodyStr.size()), response);
    LogJiraHttpResult("POST", postUrl, response);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Add comment failed: HTTP " + std::to_string(response.status_code);
        return false;
    }
    return true;
}

bool JiraClient::AddIssueCommentBlameContext(const JiraConfig& cfg,
                                             const std::string& issueKey,
                                             const std::string& p4User,
                                             const std::string& functionName,
                                             const std::string& filePath,
                                             const int lineNumber,
                                             const std::string& changelist,
                                             const std::string& date,
                                             const bool approximated,
                                             const std::string& codeSnippet,
                                             std::string& outError) {
    outError.clear();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    auto para = [](const std::string& text) {
        nlohmann::json p = nlohmann::json::object();
        p["type"] = "paragraph";
        p["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}});
        return p;
    };

    nlohmann::json content = nlohmann::json::array();
    std::string head = "Blame Analysis — " + p4User + " | " + functionName + " | " + filePath + ":" +
                       std::to_string(lineNumber) + " | CL " + changelist + " | " + date;
    content.push_back(para(head));
    if (approximated) {
        content.push_back(para("Note: blame is approximated (exact line not found in annotate)."));
    }
    std::string blockBody = "L" + std::to_string(lineNumber) + "  CL:" + changelist + "  " + p4User + "\n" + codeSnippet;
    nlohmann::json codeBlock = nlohmann::json::object();
    codeBlock["type"] = "codeBlock";
    codeBlock["attrs"] = nlohmann::json::object();
    codeBlock["attrs"]["language"] = "cpp";
    codeBlock["content"] =
        nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", blockBody}}});
    content.push_back(std::move(codeBlock));

    nlohmann::json doc = nlohmann::json::object();
    doc["type"] = "doc";
    doc["version"] = 1;
    doc["content"] = std::move(content);

    nlohmann::json body = nlohmann::json::object();
    body["body"] = std::move(doc);
    const std::string postUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/comment";
    const std::string bodyStr = body.dump();
    auto response = cpr::Post(cpr::Url{postUrl}, headers, cpr::Body{bodyStr}, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        static_cast<std::uint64_t>(bodyStr.size()), response);
    LogJiraHttpResult("POST", postUrl, response);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Add blame comment failed: HTTP " + std::to_string(response.status_code);
        return false;
    }
    return true;
}

bool JiraClient::FetchUserGroupNames(const JiraConfig& cfg,
                                     const std::string& accountId,
                                     std::vector<std::string>& outGroupNames,
                                     std::string& outError) {
    outGroupNames.clear();
    outError.clear();
    if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
        outError = "Missing Jira domain or API token.";
        return false;
    }
    if (accountId.empty()) {
        return true;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);
    cpr::Header headers{
        {"Accept", "application/json"},
        {"Authorization", authHeader},
        {"User-Agent", "Smatchet/1.0 Jira-Client"}
    };
    cpr::Redirect redirect(true, true);

    const std::string url =
        base + "/rest/api/3/user?accountId=" + UrlEncode(accountId) + "&expand=groups,applicationRoles";
    auto resp = cpr::Get(cpr::Url{url}, headers, redirect);
    NetworkUsageTracker::Instance().Record(HttpTrafficKind::Jira,
        NetworkUsageTracker::kEstimatedGetUploadBytes, resp);
    LogJiraHttpResult("GET", url, resp);
    if (resp.status_code != 200) {
        outError = "user lookup failed: HTTP " + std::to_string(resp.status_code);
        return false;
    }

    try {
        auto j = nlohmann::json::parse(resp.text);
        if (j.contains("groups") && j["groups"].is_object()) {
            const auto& g = j["groups"];
            if (g.contains("items") && g["items"].is_array()) {
                for (const auto& item : g["items"]) {
                    if (item.is_object() && item.contains("name") && item["name"].is_string()) {
                        outGroupNames.push_back(item["name"].get<std::string>());
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        outError = std::string("user parse error: ") + ex.what();
        return false;
    }
    return true;
}

