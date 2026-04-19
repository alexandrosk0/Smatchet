#include "JiraFieldPayload.h"

#include "CompactDateFormat.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace {

void DecodeCascadingSelection(const std::string& encoded, std::string& outParentId, std::string& outChildId) {
    const size_t sep = encoded.find('\x1f');
    if (sep == std::string::npos) {
        outParentId = encoded;
        outChildId.clear();
        return;
    }
    outParentId = encoded.substr(0, sep);
    outChildId = encoded.substr(sep + 1);
}

const JiraFieldOption* FindJiraFieldOptionByIdRecursive(const std::vector<JiraFieldOption>& options,
                                                         const std::string& id) {
    for (const auto& option : options) {
        if (option.Id == id) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const JiraFieldOption* nested = FindJiraFieldOptionByIdRecursive(option.Children, id)) {
                return nested;
            }
        }
    }
    return nullptr;
}

std::string ResolveJiraFieldOptionLabelRecursive(const std::vector<JiraFieldOption>& options,
                                                  const std::string& value) {
    for (const auto& option : options) {
        if (option.Id == value || option.Value == value) {
            return option.Value;
        }
        if (!option.Children.empty()) {
            const std::string nested = ResolveJiraFieldOptionLabelRecursive(option.Children, value);
            if (!nested.empty()) {
                return nested;
            }
        }
    }
    return {};
}

/** Match option by id or display label (grid often stores names, not Jira ids). */
const JiraFieldOption* FindJiraFieldOptionByIdOrValueRecursive(const std::vector<JiraFieldOption>& options,
                                                                const std::string& idOrValue) {
    for (const auto& option : options) {
        if (option.Id == idOrValue || option.Value == idOrValue) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const JiraFieldOption* nested =
                    FindJiraFieldOptionByIdOrValueRecursive(option.Children, idOrValue)) {
                return nested;
            }
        }
    }
    return nullptr;
}

nlohmann::json MinimalPayloadForStructuredOption(const nlohmann::json& raw) {
    if (!raw.is_object()) {
        return raw;
    }
    nlohmann::json out = nlohmann::json::object();
    if (raw.contains("id") && (raw["id"].is_string() || raw["id"].is_number())) {
        out["id"] = raw["id"];
        return out;
    }
    if (raw.contains("accountId") && raw["accountId"].is_string()) {
        out["accountId"] = raw["accountId"];
        return out;
    }
    if (raw.contains("groupId") && raw["groupId"].is_string()) {
        out["groupId"] = raw["groupId"];
        if (raw.contains("name") && raw["name"].is_string()) {
            out["name"] = raw["name"];
        }
        return out;
    }
    if (raw.contains("key") && raw["key"].is_string()) {
        out["key"] = raw["key"];
        return out;
    }
    if (raw.contains("value") && raw["value"].is_string()) {
        out["value"] = raw["value"];
        return out;
    }
    if (raw.contains("name") && raw["name"].is_string()) {
        out["name"] = raw["name"];
        return out;
    }
    return raw;
}

bool TryBuildStructuredOptionPayload(const JiraFieldOption& option,
                                     const std::string& nestedChildId,
                                     nlohmann::json& outPayload) {
    if (option.PayloadJson.empty()) {
        return false;
    }
    const nlohmann::json raw = nlohmann::json::parse(option.PayloadJson, nullptr, false);
    if (raw.is_discarded()) {
        return false;
    }
    outPayload = MinimalPayloadForStructuredOption(raw);
    if (!nestedChildId.empty()) {
        const JiraFieldOption* child = FindJiraFieldOptionByIdOrValueRecursive(option.Children, nestedChildId);
        if (child == nullptr) {
            return false;
        }
        nlohmann::json childPayload;
        if (!TryBuildStructuredOptionPayload(*child, std::string(), childPayload)) {
            childPayload = nlohmann::json::object({{"id", nestedChildId}});
        }
        if (!outPayload.is_object()) {
            outPayload = nlohmann::json::object();
        }
        outPayload["child"] = std::move(childPayload);
    }
    return true;
}

bool TryBuildFieldOptionPayload(const JiraField& field,
                                const std::string& selectedValue,
                                nlohmann::json& outPayload) {
    if (field.Family == JiraFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(selectedValue, parentId, childId);
        const JiraFieldOption* option = FindJiraFieldOptionByIdOrValueRecursive(field.AllowedValueOptions, parentId);
        if (option == nullptr) {
            return false;
        }
        return TryBuildStructuredOptionPayload(*option, childId, outPayload);
    }
    const JiraFieldOption* option = FindJiraFieldOptionByIdOrValueRecursive(field.AllowedValueOptions, selectedValue);
    if (option == nullptr) {
        return false;
    }
    return TryBuildStructuredOptionPayload(*option, std::string(), outPayload);
}

nlohmann::json FallbackPayloadForSelectableField(const JiraField& field, const std::string& scalarValue) {
    if (field.IsUserType) {
        return nlohmann::json{{"accountId", scalarValue}};
    }
    if (field.Family == JiraFieldFamily::Status) {
        return nlohmann::json{{"name", scalarValue}};
    }
    if (field.Family == JiraFieldFamily::IssueType) {
        return nlohmann::json{{"id", scalarValue}};
    }
    if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
        return nlohmann::json{{"id", scalarValue}};
    }
    return scalarValue;
}

bool TryBuildUserFieldPayload(const JiraField& field,
                              const std::string& scalarValue,
                              nlohmann::json& outValue,
                              std::string& outError) {
    outError.clear();
    const std::string trimmed = TrimCopy(scalarValue);
    if (trimmed.empty()) {
        outValue = nullptr;
        return true;
    }
    if (!trimmed.empty() && trimmed.front() == '{') {
        const nlohmann::json j = nlohmann::json::parse(trimmed, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            if (j.contains("accountId") && j["accountId"].is_string()) {
                outValue = MinimalPayloadForStructuredOption(j);
                return true;
            }
            if (j.contains("displayName") && j["displayName"].is_string()) {
                const std::string dn = j["displayName"].get<std::string>();
                if (const JiraFieldOption* opt =
                        FindJiraFieldOptionByIdOrValueRecursive(field.AllowedValueOptions, dn)) {
                    if (!opt->Id.empty()) {
                        outValue = nlohmann::json{{"accountId", opt->Id}};
                        return true;
                    }
                }
            }
        }
    }
    if (const JiraFieldOption* opt = FindJiraFieldOptionByIdOrValueRecursive(field.AllowedValueOptions, trimmed)) {
        if (!opt->Id.empty()) {
            outValue = nlohmann::json{{"accountId", opt->Id}};
            return true;
        }
    }
    // Already an account id, or catalog could not resolve (Jira validates).
    outValue = nlohmann::json{{"accountId", trimmed}};
    return true;
}

bool TryParseNumberValue(const std::string& rawValue, nlohmann::json& outValue) {
    const std::string trimmed = TrimCopy(rawValue);
    if (trimmed.empty()) {
        outValue = nullptr;
        return true;
    }
    size_t parsedChars = 0;
    try {
        const double parsed = std::stod(trimmed, &parsedChars);
        if (parsedChars != trimmed.size()) {
            return false;
        }
        outValue = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool LooksLikeIssueKey(const std::string& value) {
    const size_t dash = value.find('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= value.size()) {
        return false;
    }
    for (size_t i = 0; i < dash; ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (!(std::isupper(c) != 0 || std::isdigit(c) != 0 || c == '_')) {
            return false;
        }
    }
    for (size_t i = dash + 1; i < value.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(value[i])) == 0) {
            return false;
        }
    }
    return true;
}

nlohmann::json AdfDocumentFromPlainText(const std::string& plainText) {
    nlohmann::json content = nlohmann::json::array();
    std::string line;
    std::istringstream iss(plainText);
    while (std::getline(iss, line)) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", line}}});
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

std::vector<std::string> SplitCommaSeparatedTrimmed(const std::string& input) {
    std::vector<std::string> segments;
    std::string cur;
    for (char ch : input) {
        if (ch == ',') {
            const std::string t = TrimCopy(cur);
            if (!t.empty()) {
                segments.push_back(t);
            }
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    const std::string t = TrimCopy(cur);
    if (!t.empty()) {
        segments.push_back(t);
    }
    return segments;
}

/** Jira label names cannot contain spaces; normalize for inherited / pasted CSV text. */
std::string SanitizeJiraLabelToken(std::string s) {
    s = TrimCopy(s);
    for (char& c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            c = '-';
        }
    }
    return s;
}

} // namespace

namespace JiraFieldPayload {

bool FieldUsesAdfDocument(const JiraField& field) {
    const std::string idLower = ToLowerAsciiCopy(field.Id);
    if (idLower == "description" || idLower == "environment") {
        return true;
    }
    const std::string typeLower = ToLowerAsciiCopy(field.Type);
    if (typeLower == "doc") {
        return true;
    }
    const std::string customLower = ToLowerAsciiCopy(field.SchemaCustom);
    if (customLower.empty()) {
        return false;
    }
    if (customLower.find("adf") != std::string::npos ||
        customLower.find("atlassian-document") != std::string::npos) {
        return true;
    }
    // Multiline / wiki-style custom fields on Jira Cloud typically reject plain strings on v3 create/edit.
    if (customLower.find("textarea") != std::string::npos) {
        return true;
    }
    if (customLower.find("wiki-renderer") != std::string::npos ||
        customLower.find("jira-wiki-renderer") != std::string::npos) {
        return true;
    }
    if (!field.RestFieldDefinitionJson.empty()) {
        const std::string r = ToLowerAsciiCopy(field.RestFieldDefinitionJson);
        if (r.find("atlassian-document-format") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ExtractIssueKey(const std::string& value) {
    const std::string trimmed = TrimCopy(value);
    if (LooksLikeIssueKey(trimmed)) {
        return trimmed;
    }
    const size_t sep = trimmed.find(" - ");
    if (sep == std::string::npos) {
        return std::string();
    }
    const std::string key = TrimCopy(trimmed.substr(0, sep));
    return LooksLikeIssueKey(key) ? key : std::string();
}

bool IsArrayLike(const JiraField& field) {
    return field.IsArray;
}

bool BuildValue(const JiraField& field,
                const std::vector<std::string>& rawValues,
                nlohmann::json& outValue,
                std::string& outError) {
    outError.clear();
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    for (const auto& value : rawValues) {
        if (!value.empty()) {
            values.push_back(value);
        }
    }

    // Grid stores labels as comma-separated text; Jira expects an array of separate label strings.
    if (field.Id == "labels" || field.Family == JiraFieldFamily::Labels) {
        outValue = nlohmann::json::array();
        for (const auto& value : values) {
            for (const std::string& seg : SplitCommaSeparatedTrimmed(value)) {
                const std::string fixed = SanitizeJiraLabelToken(seg);
                if (!fixed.empty()) {
                    outValue.push_back(fixed);
                }
            }
        }
        if (outValue.empty()) {
            outValue = nullptr;
        }
        return true;
    }

    if (field.IsArray) {
        outValue = nlohmann::json::array();
        for (const auto& value : values) {
            if (field.IsUserType) {
                nlohmann::json one;
                std::string uerr;
                if (!TryBuildUserFieldPayload(field, value, one, uerr)) {
                    outError = uerr;
                    return false;
                }
                if (!one.is_null()) {
                    outValue.push_back(std::move(one));
                }
            } else if (field.Family == JiraFieldFamily::StructuredMulti ||
                       field.Family == JiraFieldFamily::IssueType ||
                       field.Family == JiraFieldFamily::Status ||
                       field.Family == JiraFieldFamily::CascadingSelect) {
                nlohmann::json optionPayload;
                if (TryBuildFieldOptionPayload(field, value, optionPayload)) {
                    outValue.push_back(std::move(optionPayload));
                } else if (field.ItemsType == "option" || field.ItemsType == "component" ||
                           !field.AllowedValueOptions.empty()) {
                    outValue.push_back(FallbackPayloadForSelectableField(field, value));
                } else {
                    outValue.push_back(value);
                }
            } else if (field.ItemsType == "option" || field.ItemsType == "component" ||
                       !field.AllowedValueOptions.empty()) {
                outValue.push_back(nlohmann::json{{"id", value}});
            } else {
                outValue.push_back(value);
            }
        }
        return true;
    }

    const std::string scalarValue = values.empty() ? std::string() : values.front();
    if (scalarValue.empty()) {
        outValue = nullptr;
        return true;
    }
    if (field.Id == "parent") {
        const std::string parentKey = ExtractIssueKey(scalarValue);
        outValue = parentKey.empty() ? nlohmann::json(scalarValue) : nlohmann::json{{"key", parentKey}};
        return true;
    }
    if (field.Id == "project") {
        // Create payload: project identified by key (Jira also accepts id).
        outValue = nlohmann::json{{"key", scalarValue}};
        return true;
    }
    if (field.IsUserType) {
        return TryBuildUserFieldPayload(field, scalarValue, outValue, outError);
    }
    if (field.Family == JiraFieldFamily::StructuredSingle ||
        field.Family == JiraFieldFamily::IssueType ||
        field.Family == JiraFieldFamily::Status ||
        field.Family == JiraFieldFamily::CascadingSelect ||
        field.Family == JiraFieldFamily::SelectSingle) {
        if (!TryBuildFieldOptionPayload(field, scalarValue, outValue)) {
            outValue = FallbackPayloadForSelectableField(field, scalarValue);
        }
        return true;
    }
    if (field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty()) {
        outValue = nlohmann::json{{"id", scalarValue}};
        return true;
    }

    // REST v3 expects structured objects for these schema types; plain strings 400.
    const std::string typeLower = ToLowerAsciiCopy(field.Type);
    if (typeLower == "priority" || field.Id == "priority") {
        nlohmann::json optPayload;
        if (TryBuildFieldOptionPayload(field, scalarValue, optPayload)) {
            outValue = std::move(optPayload);
            return true;
        }
        const std::string t = TrimCopy(scalarValue);
        const bool digitsOnly =
            !t.empty() &&
            std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        outValue = digitsOnly ? nlohmann::json{{"id", t}} : nlohmann::json{{"name", t}};
        return true;
    }
    if (typeLower == "version") {
        nlohmann::json optPayload;
        if (TryBuildFieldOptionPayload(field, scalarValue, optPayload)) {
            outValue = std::move(optPayload);
            return true;
        }
        outValue = nlohmann::json{{"name", TrimCopy(scalarValue)}};
        return true;
    }
    if (typeLower == "resolution") {
        nlohmann::json optPayload;
        if (TryBuildFieldOptionPayload(field, scalarValue, optPayload)) {
            outValue = std::move(optPayload);
            return true;
        }
        outValue = nlohmann::json{{"name", TrimCopy(scalarValue)}};
        return true;
    }
    if (typeLower == "securitylevel") {
        nlohmann::json optPayload;
        if (TryBuildFieldOptionPayload(field, scalarValue, optPayload)) {
            outValue = std::move(optPayload);
            return true;
        }
        const std::string t = TrimCopy(scalarValue);
        const bool digitsOnly =
            !t.empty() &&
            std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
        outValue = digitsOnly ? nlohmann::json{{"id", t}} : nlohmann::json{{"name", t}};
        return true;
    }
    if (typeLower == "group") {
        outValue = nlohmann::json{{"name", TrimCopy(scalarValue)}};
        return true;
    }

    if (FieldUsesAdfDocument(field)) {
        outValue = AdfDocumentFromPlainText(scalarValue);
        return true;
    }
    if (field.Type == "date" || field.Type == "datetime") {
        ParsedJiraDateTime parsed;
        if (!TryParseJiraDateTime(scalarValue, parsed)) {
            outError = "Invalid date/datetime value: " + scalarValue;
            return false;
        }
        outValue = FormatJiraDateOrDateTimeForApi(field.Type == "date", parsed);
        return true;
    }
    if (field.Type == "number") {
        if (!TryParseNumberValue(scalarValue, outValue)) {
            outError = "Invalid numeric value: " + scalarValue;
            return false;
        }
        return true;
    }
    outValue = scalarValue;
    return true;
}

std::vector<std::string> SplitCommaSeparatedValues(const std::string& input) {
    std::vector<std::string> segments;
    std::string cur;
    for (char ch : input) {
        if (ch == ',') {
            const std::string t = TrimCopy(cur);
            if (!t.empty()) {
                segments.push_back(t);
            }
            cur.clear();
        } else {
            cur.push_back(ch);
        }
    }
    const std::string t = TrimCopy(cur);
    if (!t.empty()) {
        segments.push_back(t);
    }
    return segments;
}

bool IsSprintField(const JiraField& field) {
    return field.Family == JiraFieldFamily::Sprint ||
           field.SchemaCustom.find("gh-sprint") != std::string::npos;
}

std::string ResolveSprintIdForAgile(const JiraField& field, const std::string& rawValue) {
    const std::string trimmed = TrimCopy(rawValue);
    if (trimmed.empty()) {
        return {};
    }
    if (const JiraFieldOption* opt = FindJiraFieldOptionByIdOrValueRecursive(field.AllowedValueOptions, trimmed)) {
        if (!opt->Id.empty()) {
            return opt->Id;
        }
    }
    const bool allDigit =
        !trimmed.empty() &&
        std::all_of(trimmed.begin(), trimmed.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    return allDigit ? trimmed : std::string{};
}

std::string ResolveDisplayValueForSubmittedSelection(const JiraField& field, const std::string& value) {
    if (field.Family == JiraFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(value, parentId, childId);
        const JiraFieldOption* parent = FindJiraFieldOptionByIdRecursive(field.AllowedValueOptions, parentId);
        if (parent == nullptr) {
            return value;
        }
        if (childId.empty()) {
            return parent->Value;
        }
        const JiraFieldOption* child = FindJiraFieldOptionByIdRecursive(parent->Children, childId);
        if (child == nullptr) {
            return parent->Value;
        }
        return parent->Value + " > " + child->Value;
    }
    const std::string resolved = ResolveJiraFieldOptionLabelRecursive(field.AllowedValueOptions, value);
    return resolved.empty() ? value : resolved;
}

} // namespace JiraFieldPayload
