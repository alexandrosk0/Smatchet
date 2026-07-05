#include "TrackerFieldPayloadPure.h"

#include "CompactDateFormat.h"
#include "Json/BoundedJsonParse.h"
#include "MarkdownConvert.h"
#include "StringUtil.h"

#include <algorithm>
#include <cctype>

namespace TrackerFieldPayloadPure {

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

const TrackerFieldOption* FindOptionById(const std::vector<TrackerFieldOption>& options, const std::string& id) {
    for (const auto& option : options) {
        if (option.Id == id) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const TrackerFieldOption* nested = FindOptionById(option.Children, id)) {
                return nested;
            }
        }
    }
    return nullptr;
}

// Id-preferred resolution: scan for option.Id == value first; only fall back to
// option.Value == value when no id matches. Avoids the id/name collision where a
// component literally named "10033" shadows the option whose Id is 10033.
std::string ResolveOptionLabelByIdRecursive(const std::vector<TrackerFieldOption>& options, const std::string& value) {
    for (const auto& option : options) {
        if (option.Id == value) {
            return option.Value;
        }
        if (!option.Children.empty()) {
            const std::string nested = ResolveOptionLabelByIdRecursive(option.Children, value);
            if (!nested.empty()) {
                return nested;
            }
        }
    }
    return {};
}

std::string ResolveOptionLabelByValueRecursive(const std::vector<TrackerFieldOption>& options,
                                               const std::string& value) {
    for (const auto& option : options) {
        if (option.Value == value) {
            return option.Value;
        }
        if (!option.Children.empty()) {
            const std::string nested = ResolveOptionLabelByValueRecursive(option.Children, value);
            if (!nested.empty()) {
                return nested;
            }
        }
    }
    return {};
}

std::string ResolveOptionLabel(const std::vector<TrackerFieldOption>& options, const std::string& value) {
    const std::string byId = ResolveOptionLabelByIdRecursive(options, value);
    if (!byId.empty()) {
        return byId;
    }
    return ResolveOptionLabelByValueRecursive(options, value);
}

const TrackerFieldOption* FindOptionByValueRecursive(const std::vector<TrackerFieldOption>& options,
                                                     const std::string& value) {
    for (const auto& option : options) {
        if (option.Value == value) {
            return &option;
        }
        if (!option.Children.empty()) {
            if (const TrackerFieldOption* nested = FindOptionByValueRecursive(option.Children, value)) {
                return nested;
            }
        }
    }
    return nullptr;
}

// Id-preferred resolution: a full id-first pass before any value match. Mirrors
// ResolveOptionLabel's two-pass ordering so the payload path serializes the same
// option the display path resolves. Avoids the id/name collision where a component
// literally named "10033" shadows the option whose Id is 10033 — a single-pass
// `Id == v || Value == v` would return the wrong (Value-matched) option first.
const TrackerFieldOption* FindOptionByIdOrValue(const std::vector<TrackerFieldOption>& options,
                                                const std::string& idOrValue) {
    if (const TrackerFieldOption* byId = FindOptionById(options, idOrValue)) {
        return byId;
    }
    return FindOptionByValueRecursive(options, idOrValue);
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

Optional<nlohmann::json> BuildStructuredOptionPayload(const TrackerFieldOption& option,
                                                      const std::string& nestedChildId) {
    if (option.PayloadJson.empty()) {
        return Optional<nlohmann::json>();
    }
    // PayloadJson round-trips through the on-disk field-catalog cache, so a tampered
    // cache can plant a depth bomb inside this string leaf — bound the re-parse
    // (same class as the DetectFieldFamily fix; graceful not-structured on failure).
    const nlohmann::json raw = smatchet::json_safe::ParseBoundedOrDiscarded(option.PayloadJson);
    if (raw.is_discarded()) {
        return Optional<nlohmann::json>();
    }
    nlohmann::json outPayload = MinimalPayloadForStructuredOption(raw);
    if (!nestedChildId.empty()) {
        const TrackerFieldOption* child = FindOptionByIdOrValue(option.Children, nestedChildId);
        if (child == nullptr) {
            return Optional<nlohmann::json>();
        }
        Optional<nlohmann::json> childPayload = BuildStructuredOptionPayload(*child, std::string());
        nlohmann::json childJson = childPayload.has_value() ? std::move(childPayload.value())
                                                            : nlohmann::json::object({{"id", nestedChildId}});
        if (!outPayload.is_object()) {
            outPayload = nlohmann::json::object();
        }
        outPayload["child"] = std::move(childJson);
    }
    return Optional<nlohmann::json>(std::move(outPayload));
}

Optional<nlohmann::json> BuildFieldOptionPayload(const TrackerField& field, const std::string& selectedValue) {
    if (field.Family == TrackerFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(selectedValue, parentId, childId);
        const TrackerFieldOption* option = FindOptionByIdOrValue(field.AllowedValueOptions, parentId);
        if (option == nullptr) {
            return Optional<nlohmann::json>();
        }
        return BuildStructuredOptionPayload(*option, childId);
    }
    const TrackerFieldOption* option = FindOptionByIdOrValue(field.AllowedValueOptions, selectedValue);
    if (option == nullptr) {
        return Optional<nlohmann::json>();
    }
    return BuildStructuredOptionPayload(*option, std::string());
}

bool IsDigitsOnly(const std::string& value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

nlohmann::json FallbackPayloadForSelectableField(const TrackerField& field, const std::string& scalarValue) {
    if (field.IsUserType) {
        return nlohmann::json{{"accountId", scalarValue}};
    }
    if (field.Family == TrackerFieldFamily::Status) {
        return nlohmann::json{{"name", scalarValue}};
    }
    if (field.Family == TrackerFieldFamily::IssueType) {
        return nlohmann::json{{"id", scalarValue}};
    }
    if (field.Type == "component" || field.ItemsType == "component") {
        const std::string trimmed = TrimCopy(scalarValue);
        if (const TrackerFieldOption* opt = FindOptionByIdOrValue(field.AllowedValueOptions, trimmed)) {
            if (!opt->Id.empty()) {
                return nlohmann::json{{"id", opt->Id}};
            }
        }
        return nlohmann::json{{"name", trimmed}};
    }
    if (field.Type == "option" || !field.AllowedValueOptions.empty()) {
        return nlohmann::json{{"id", scalarValue}};
    }
    return scalarValue;
}

void BuildUserFieldPayload(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue) {
    const std::string trimmed = TrimCopy(scalarValue);
    if (trimmed.empty()) {
        outValue = nullptr;
        return;
    }
    if (trimmed.front() == '{') {
        // The scalar can arrive from grid edits, Lua automation, or a tracker-sourced
        // round-trip — bound the parse so a deep string can't crash DOM teardown.
        const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(trimmed);
        if (!j.is_discarded() && j.is_object()) {
            if (j.contains("accountId") && j["accountId"].is_string()) {
                outValue = MinimalPayloadForStructuredOption(j);
                return;
            }
            if (j.contains("displayName") && j["displayName"].is_string()) {
                const std::string dn = j["displayName"].get<std::string>();
                if (const TrackerFieldOption* opt = FindOptionByIdOrValue(field.AllowedValueOptions, dn)) {
                    if (!opt->Id.empty()) {
                        outValue = nlohmann::json{{"accountId", opt->Id}};
                        return;
                    }
                }
            }
        }
    }
    if (const TrackerFieldOption* opt = FindOptionByIdOrValue(field.AllowedValueOptions, trimmed)) {
        if (!opt->Id.empty()) {
            outValue = nlohmann::json{{"accountId", opt->Id}};
            return;
        }
    }
    // Already an account id, or catalog could not resolve (Jira validates).
    outValue = nlohmann::json{{"accountId", trimmed}};
}

Optional<nlohmann::json> ParseNumberValue(const std::string& rawValue) {
    const std::string trimmed = TrimCopy(rawValue);
    if (trimmed.empty()) {
        return Optional<nlohmann::json>(nlohmann::json(nullptr));
    }
    size_t parsedChars = 0;
    try {
        const double parsed = std::stod(trimmed, &parsedChars);
        if (parsedChars != trimmed.size()) {
            return Optional<nlohmann::json>();
        }
        return Optional<nlohmann::json>(nlohmann::json(parsed));
    } catch (...) {
        return Optional<nlohmann::json>();
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
    if (!std::all_of(value.begin() + static_cast<ptrdiff_t>(dash) + 1, value.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; })) {
        return false;
    }
    return true;
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

std::string SanitizeJiraLabelToken(std::string s) {
    s = TrimCopy(s);
    std::replace_if(s.begin(), s.end(), [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }, '-');
    return s;
}

bool FieldUsesAdfDocument(const TrackerField& field) {
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
    if (customLower.find("adf") != std::string::npos || customLower.find("atlassian-document") != std::string::npos) {
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
    if (!field.RawFieldDefinitionJson.empty()) {
        const std::string r = ToLowerAsciiCopy(field.RawFieldDefinitionJson);
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

bool IsArrayLike(const TrackerField& field) { return field.IsArray; }

bool IsSprintField(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::Sprint || field.SchemaCustom.find("gh-sprint") != std::string::npos;
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

std::string ResolveSprintIdForAgile(const TrackerField& field, const std::string& rawValue) {
    const std::string trimmed = TrimCopy(rawValue);
    if (trimmed.empty()) {
        return {};
    }
    if (const TrackerFieldOption* opt = FindOptionByIdOrValue(field.AllowedValueOptions, trimmed)) {
        if (!opt->Id.empty()) {
            return opt->Id;
        }
    }
    const bool allDigit = !trimmed.empty() && std::all_of(trimmed.begin(), trimmed.end(),
                                                          [](unsigned char c) { return std::isdigit(c) != 0; });
    return allDigit ? trimmed : std::string{};
}

std::string ResolveDisplayValueForSubmittedSelection(const TrackerField& field, const std::string& value) {
    if (field.Family == TrackerFieldFamily::CascadingSelect) {
        std::string parentId;
        std::string childId;
        DecodeCascadingSelection(value, parentId, childId);
        const TrackerFieldOption* parent = FindOptionById(field.AllowedValueOptions, parentId);
        if (parent == nullptr) {
            return value;
        }
        if (childId.empty()) {
            return parent->Value;
        }
        const TrackerFieldOption* child = FindOptionById(parent->Children, childId);
        if (child == nullptr) {
            return parent->Value;
        }
        return parent->Value + " > " + child->Value;
    }
    if (field.IsArray) {
        // Multi-valued fields (SelectMulti / StructuredMulti / components) arrive comma-joined
        // (", " separator, matching NormalizeTrackerFieldValue's JoinStrings). Resolve each element
        // individually, then re-join — otherwise the whole "10033, 10034" string never matches a
        // single option and falls through to the raw numeric id.
        const std::vector<std::string> parts = SplitAndTrim(value);
        if (parts.empty()) {
            return value;
        }
        std::vector<std::string> resolvedParts;
        resolvedParts.reserve(parts.size());
        for (const auto& part : parts) {
            const std::string resolvedPart = ResolveOptionLabel(field.AllowedValueOptions, part);
            resolvedParts.push_back(resolvedPart.empty() ? part : resolvedPart);
        }
        return JoinStrings(resolvedParts, ", ");
    }
    const std::string resolved = ResolveOptionLabel(field.AllowedValueOptions, value);
    return resolved.empty() ? value : resolved;
}

// Per-shape builders for BuildValue's array path. Each appends to an array.
namespace {

bool ArrayItemUsesStructuredFamily(const TrackerField& field) {
    return field.Family == TrackerFieldFamily::StructuredMulti || field.Family == TrackerFieldFamily::IssueType ||
           field.Family == TrackerFieldFamily::Status || field.Family == TrackerFieldFamily::CascadingSelect;
}

bool ArrayItemIsSelectable(const TrackerField& field) {
    return field.ItemsType == "option" || field.ItemsType == "component" || !field.AllowedValueOptions.empty();
}

void AppendArrayItem(const TrackerField& field, const std::string& value, nlohmann::json& outArray) {
    if (field.IsUserType) {
        nlohmann::json one;
        BuildUserFieldPayload(field, value, one);
        if (!one.is_null()) {
            outArray.push_back(std::move(one));
        }
        return;
    }
    if (ArrayItemUsesStructuredFamily(field)) {
        Optional<nlohmann::json> optionPayload = BuildFieldOptionPayload(field, value);
        if (optionPayload.has_value()) {
            outArray.push_back(std::move(optionPayload.value()));
        } else if (ArrayItemIsSelectable(field)) {
            outArray.push_back(FallbackPayloadForSelectableField(field, value));
        } else {
            outArray.push_back(value);
        }
        return;
    }
    if (ArrayItemIsSelectable(field)) {
        Optional<nlohmann::json> optionPayload = BuildFieldOptionPayload(field, value);
        if (optionPayload.has_value()) {
            outArray.push_back(std::move(optionPayload.value()));
        } else {
            outArray.push_back(FallbackPayloadForSelectableField(field, value));
        }
        return;
    }
    outArray.push_back(value);
}

void BuildLabelsValue(const std::vector<std::string>& values, nlohmann::json& outValue) {
    // Grid stores labels as comma-separated text; Jira expects an array of separate label strings.
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
}

void BuildArrayValue(const TrackerField& field, const std::vector<std::string>& values, nlohmann::json& outValue) {
    outValue = nlohmann::json::array();
    for (const auto& value : values) {
        AppendArrayItem(field, value, outValue);
    }
}

// Scalar dispatch table: each per-type builder produces byte-identical output
// to the original inline branch. A builder returns true when it claims the
// field (writing outValue / outError); false to fall through to the next.
using ScalarBuilderFn = bool (*)(const TrackerField&, const std::string&, nlohmann::json&, std::string&);

bool BuildParentScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                       std::string& /*outError*/) {
    if (field.Id != "parent") {
        return false;
    }
    const std::string parentKey = ExtractIssueKey(scalarValue);
    outValue = parentKey.empty() ? nlohmann::json(scalarValue) : nlohmann::json{{"key", parentKey}};
    return true;
}

bool BuildProjectScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                        std::string& /*outError*/) {
    if (field.Id != "project") {
        return false;
    }
    // Create payload: project identified by key (Jira also accepts id).
    outValue = nlohmann::json{{"key", scalarValue}};
    return true;
}

bool BuildUserScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                     std::string& /*outError*/) {
    if (!field.IsUserType) {
        return false;
    }
    BuildUserFieldPayload(field, scalarValue, outValue);
    return true;
}

bool BuildStructuredSelectScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                                 std::string& /*outError*/) {
    if (!(field.Family == TrackerFieldFamily::StructuredSingle || field.Family == TrackerFieldFamily::IssueType ||
          field.Family == TrackerFieldFamily::Status || field.Family == TrackerFieldFamily::CascadingSelect ||
          field.Family == TrackerFieldFamily::SelectSingle)) {
        return false;
    }
    Optional<nlohmann::json> payload = BuildFieldOptionPayload(field, scalarValue);
    outValue = payload.has_value() ? std::move(payload.value()) : FallbackPayloadForSelectableField(field, scalarValue);
    return true;
}

bool BuildOptionComponentScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                                std::string& /*outError*/) {
    if (!(field.Type == "option" || field.Type == "component" || !field.AllowedValueOptions.empty())) {
        return false;
    }
    outValue = nlohmann::json{{"id", scalarValue}};
    return true;
}

// REST v3 expects structured objects for priority/securitylevel; id when digits-only else name.
bool BuildPriorityLikeScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                             std::string& /*outError*/) {
    const std::string typeLower = ToLowerAsciiCopy(field.Type);
    if (!(typeLower == "priority" || field.Id == "priority" || typeLower == "securitylevel")) {
        return false;
    }
    Optional<nlohmann::json> optPayload = BuildFieldOptionPayload(field, scalarValue);
    if (optPayload.has_value()) {
        outValue = std::move(optPayload.value());
        return true;
    }
    const std::string t = TrimCopy(scalarValue);
    outValue = IsDigitsOnly(t) ? nlohmann::json{{"id", t}} : nlohmann::json{{"name", t}};
    return true;
}

// version / resolution: structured option if catalogued, else {name: trimmed}.
bool BuildNamedOptionScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                            std::string& /*outError*/) {
    const std::string typeLower = ToLowerAsciiCopy(field.Type);
    if (!(typeLower == "version" || typeLower == "resolution")) {
        return false;
    }
    Optional<nlohmann::json> optPayload = BuildFieldOptionPayload(field, scalarValue);
    if (optPayload.has_value()) {
        outValue = std::move(optPayload.value());
        return true;
    }
    outValue = nlohmann::json{{"name", TrimCopy(scalarValue)}};
    return true;
}

bool BuildGroupScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                      std::string& /*outError*/) {
    if (ToLowerAsciiCopy(field.Type) != "group") {
        return false;
    }
    outValue = nlohmann::json{{"name", TrimCopy(scalarValue)}};
    return true;
}

bool BuildAdfScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                    std::string& /*outError*/) {
    if (!FieldUsesAdfDocument(field)) {
        return false;
    }
    // The long-text modal editor produces Markdown for ADF fields (description, environment,
    // textarea / wiki-renderer customfields). MarkdownToAdf preserves headings, lists, code
    // blocks, links, and inline emphasis. Plain-text input still works (no Markdown features =
    // a single paragraph). See RICH_TEXT_EDITING_V2_PLAN.md.
    outValue = MarkdownConvert::MarkdownToAdf(scalarValue);
    return true;
}

bool BuildDateScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                     std::string& outError) {
    if (!(field.Type == "date" || field.Type == "datetime")) {
        return false;
    }
    ParsedJiraDateTime parsed;
    if (!TryParseJiraDateTime(scalarValue, parsed)) {
        outError = "Invalid date/datetime value: " + scalarValue;
        return true;
    }
    outValue = FormatJiraDateOrDateTimeForApi(field.Type == "date", parsed);
    return true;
}

bool BuildNumberScalar(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                       std::string& outError) {
    if (field.Type != "number") {
        return false;
    }
    Optional<nlohmann::json> parsed = ParseNumberValue(scalarValue);
    if (parsed.has_value()) {
        outValue = std::move(parsed.value());
    } else {
        outError = "Invalid numeric value: " + scalarValue;
    }
    return true;
}

// Ordered scalar dispatch table — first builder to claim the field wins, exactly
// mirroring the original top-to-bottom if/else tower's precedence.
const ScalarBuilderFn kScalarBuilders[] = {
    &BuildParentScalar,
    &BuildProjectScalar,
    &BuildUserScalar,
    &BuildStructuredSelectScalar,
    &BuildOptionComponentScalar,
    &BuildPriorityLikeScalar,
    &BuildNamedOptionScalar,
    &BuildGroupScalar,
    &BuildAdfScalar,
    &BuildDateScalar,
    &BuildNumberScalar,
};

bool BuildScalarValue(const TrackerField& field, const std::string& scalarValue, nlohmann::json& outValue,
                      std::string& outError) {
    for (ScalarBuilderFn builder : kScalarBuilders) {
        if (builder(field, scalarValue, outValue, outError)) {
            return outError.empty();
        }
    }
    outValue = scalarValue;
    return true;
}

} // namespace

Result<nlohmann::json> BuildValue(const TrackerField& field, const std::vector<std::string>& rawValues) {
    std::vector<std::string> values;
    values.reserve(rawValues.size());
    std::copy_if(rawValues.begin(), rawValues.end(), std::back_inserter(values),
                 [](const std::string& value) { return !value.empty(); });

    nlohmann::json outValue;

    if (field.Id == "labels" || field.Family == TrackerFieldFamily::Labels) {
        BuildLabelsValue(values, outValue);
        return Result<nlohmann::json>::Ok(std::move(outValue));
    }
    if (field.IsArray) {
        BuildArrayValue(field, values, outValue);
        return Result<nlohmann::json>::Ok(std::move(outValue));
    }

    const std::string scalarValue = values.empty() ? std::string() : values.front();
    if (scalarValue.empty()) {
        return Result<nlohmann::json>::Ok(nlohmann::json(nullptr));
    }
    std::string outError;
    if (!BuildScalarValue(field, scalarValue, outValue, outError)) {
        return Result<nlohmann::json>::Err(std::move(outError));
    }
    return Result<nlohmann::json>::Ok(std::move(outValue));
}

} // namespace TrackerFieldPayloadPure
