// Monoliths Phase A (docs/plans/active/decompose-top-20-monoliths.md) — pure-logic
// Plane field-catalog mapping helpers extracted from PlaneFieldCatalog.cpp. No cpr,
// no Logger, no PlaneClient instance state. See PlaneFieldCatalogPure.h for contract.

#include "PlaneFieldCatalogPure.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

namespace smatchet {
namespace plane {

namespace {

// Local copy of the production JsonFieldToString helper (defined in PlaneClient.cpp inside
// smatchet::plane_detail). Replicated here to keep this TU free of the production internal
// header (which pulls cpr via PlaneClient_Internal.h). Same behaviour byte-for-byte.
std::string JsonFieldToString(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || obj[key].is_null())
        return std::string();
    const auto& v = obj[key];
    if (v.is_string())
        return v.get<std::string>();
    if (v.is_number_integer())
        return std::to_string(v.get<std::int64_t>());
    if (v.is_number_float())
        return std::to_string(v.get<double>());
    if (v.is_boolean())
        return v.get<bool>() ? std::string("true") : std::string("false");
    return v.dump();
}

std::string ToUpperAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

// Sub-extraction: classify a Plane property_type into the TrackerField type/family/flag tuple.
// Pulled out of MapPlanePropertyToField so neither helper exceeds the 30-branch strict-zone cap.
// OPTION nested-option parsing stays in the caller (it needs the whole `prop` node).
void ClassifyPlanePropertyType(const std::string& pt, TrackerField& out) {
    if (pt == "NUMBER" || pt == "INTEGER" || pt == "DECIMAL" || pt == "FLOAT") {
        out.Type = "number";
        out.Family = TrackerFieldFamily::Number;
    } else if (pt == "DATE") {
        out.Type = "date";
        out.Family = TrackerFieldFamily::Date;
    } else if (pt == "DATETIME" || pt == "TIMESTAMP") {
        out.Type = "datetime";
        out.Family = TrackerFieldFamily::DateTime;
    } else if (pt == "OPTION" || pt == "DROPDOWN" || pt == "SELECT") {
        out.Type = "option";
        out.Family = TrackerFieldFamily::SelectSingle;
    } else if (pt == "MULTI_SELECT" || pt == "MULTISELECT") {
        out.Type = "array";
        out.IsArray = true;
        out.Family = TrackerFieldFamily::SelectMulti;
    } else if (pt == "MEMBER" || pt == "USER") {
        out.Type = "user";
        out.Family = TrackerFieldFamily::UserSingle;
        out.IsUserType = true;
    } else if (pt == "MULTI_MEMBER" || pt == "MULTI_USER" || pt == "USERS") {
        out.Type = "array";
        out.IsArray = true;
        out.Family = TrackerFieldFamily::UserMulti;
        out.IsUserType = true;
    } else if (pt == "BOOLEAN" || pt == "CHECKBOX") {
        out.Type = "boolean";
        out.Family = TrackerFieldFamily::Text;
    } else {
        // URL / LINK / RICH_TEXT / HTML / TEXT / empty / unknown — all collapse to the
        // string+Text default. Kept as a single branch so cppcheck doesn't flag duplicate
        // bodies; if any of these grow special handling in the future, split them back out.
        out.Type = "string";
        out.Family = TrackerFieldFamily::Text;
    }
}

// Sub-extraction: parse the OPTION/DROPDOWN/SELECT nested `options` array into AllowedValueOptions.
void AppendPlanePropertyOptions(const nlohmann::json& prop, TrackerField& out) {
    if (!prop.contains("options") || !prop["options"].is_array()) {
        return;
    }
    for (const auto& opt : prop["options"]) {
        if (!opt.is_object()) {
            continue;
        }
        TrackerFieldOption o;
        o.Id = JsonFieldToString(opt, "id");
        o.Value = JsonFieldToString(opt, "value");
        if (o.Value.empty()) {
            o.Value = JsonFieldToString(opt, "name");
        }
        if (!o.Id.empty() || !o.Value.empty()) {
            out.AllowedValueOptions.push_back(std::move(o));
        }
    }
}

} // namespace

bool MapPlanePropertyToField(const nlohmann::json& prop, TrackerField& out) {
    const std::string id = JsonFieldToString(prop, "id");
    if (id.empty()) {
        return false;
    }
    out.Id = id;
    out.Name = JsonFieldToString(prop, "display_name");
    if (out.Name.empty()) {
        out.Name = JsonFieldToString(prop, "name");
    }
    if (out.Name.empty()) {
        out.Name = out.Id;
    }
    const std::string pt = ToUpperAscii(JsonFieldToString(prop, "property_type"));
    out.IsCustom = true;
    out.ReadOnly = prop.value("is_readonly", false);
    if (prop.contains("is_required") && prop["is_required"].is_boolean()) {
        out.IsRequired = prop["is_required"].get<bool>();
    }

    ClassifyPlanePropertyType(pt, out);
    if (out.Type == "option") {
        AppendPlanePropertyOptions(prop, out);
    }

    try {
        out.RawFieldDefinitionJson = prop.dump();
    } catch (...) {
        out.RawFieldDefinitionJson.clear();
    }
    return true;
}

std::string AppendPlaneRowOption(const nlohmann::json& row, const char* idKey, const char* nameKey,
                                 TrackerField& field) {
    const std::string id = JsonFieldToString(row, idKey);
    if (id.empty()) {
        return std::string();
    }
    const std::string name = JsonFieldToString(row, nameKey);
    TrackerFieldOption opt;
    opt.Id = id;
    opt.Value = name.empty() ? id : name;
    field.AllowedValueOptions.push_back(std::move(opt));
    return id;
}

} // namespace plane
} // namespace smatchet
