// BACKLOG_CODE_REVIEW.md §C4 — Plane custom-property serialization. See
// PlaneCustomPropertyPure.h for the contract.

#include "PlaneCustomPropertyPure.h"

#include "TrackerFieldPayloadPure.h"

#include <algorithm>
#include <cctype>

namespace smatchet {
namespace plane {

namespace {

// Grid cells often store the display label rather than the option UUID; prefer the
// catalog option's Id when either matches, otherwise pass the raw token through so
// the server can still judge values the cached catalog doesn't know yet.
std::string ResolveOptionId(const TrackerField& field, const std::string& raw) {
    const TrackerFieldOption* opt = TrackerFieldPayloadPure::FindOptionByIdOrValue(field.AllowedValueOptions, raw);
    return opt != nullptr ? opt->Id : raw;
}

bool ParseBooleanToken(std::string raw, bool& outValue) {
    std::transform(raw.begin(), raw.end(), raw.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (raw == "true" || raw == "1" || raw == "yes") {
        outValue = true;
        return true;
    }
    if (raw == "false" || raw == "0" || raw == "no") {
        outValue = false;
        return true;
    }
    return false;
}

const std::string& FieldDisplayName(const TrackerField& field) { return field.Name.empty() ? field.Id : field.Name; }

} // namespace

Result<nlohmann::json> BuildPlaneCustomProperties(const std::unordered_map<std::string, std::string>& fieldValues,
                                                  const std::vector<TrackerField>& catalog) {
    nlohmann::json out = nlohmann::json::object();

    for (const TrackerField& field : catalog) {
        if (!field.IsCustom || field.ReadOnly) {
            continue;
        }
        auto it = fieldValues.find(field.Id);
        if (it == fieldValues.end() || it->second.empty()) {
            continue;
        }
        const std::string& raw = it->second;

        nlohmann::json value;
        const bool multi = field.IsArray || field.Family == TrackerFieldFamily::SelectMulti ||
                           field.Family == TrackerFieldFamily::UserMulti;
        if (multi) {
            value = nlohmann::json::array();
            for (const std::string& token : TrackerFieldPayloadPure::SplitCommaSeparatedTrimmed(raw)) {
                value.push_back(ResolveOptionId(field, token));
            }
            if (value.empty()) {
                continue;
            }
        } else if (field.Family == TrackerFieldFamily::Number) {
            Optional<nlohmann::json> parsed = TrackerFieldPayloadPure::ParseNumberValue(raw);
            if (!parsed.has_value()) {
                return Result<nlohmann::json>::Err("Field '" + FieldDisplayName(field) + "': '" + raw +
                                                   "' is not a number.");
            }
            if (parsed.value().is_null()) {
                continue;
            }
            value = parsed.value();
        } else if (field.Type == "boolean") {
            bool parsed = false;
            if (!ParseBooleanToken(raw, parsed)) {
                return Result<nlohmann::json>::Err("Field '" + FieldDisplayName(field) + "': '" + raw +
                                                   "' is not a boolean (use true/false).");
            }
            value = parsed;
        } else if (field.Family == TrackerFieldFamily::SelectSingle || field.Family == TrackerFieldFamily::UserSingle) {
            value = ResolveOptionId(field, raw);
        } else {
            // Text / Date / DateTime / unknown families: the raw string is already the shape
            // Plane stores for these property types.
            value = raw;
        }

        out[field.Id] = std::move(value);
    }

    return Result<nlohmann::json>::Ok(std::move(out));
}

} // namespace plane
} // namespace smatchet
