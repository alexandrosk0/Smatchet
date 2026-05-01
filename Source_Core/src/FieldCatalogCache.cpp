#include "FieldCatalogCache.h"

#include "ConfigManager.h"
#include "Logger.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace {

std::string FieldCatalogCachePath() {
    const std::string& base = ConfigManager::GetFilesBaseDirectory();
    if (base.empty()) {
        return std::string("smatchet_field_catalog_cache.json");
    }
    return base + "smatchet_field_catalog_cache.json";
}

nlohmann::json OptionToJson(const TrackerFieldOption& o) {
    nlohmann::json j = nlohmann::json::object();
    j["id"] = o.Id;
    j["value"] = o.Value;
    j["secondary"] = o.SecondaryValue;
    j["payload_json"] = o.PayloadJson;
    j["disabled"] = o.Disabled;
    nlohmann::json ch = nlohmann::json::array();
    for (const auto& c : o.Children) {
        ch.push_back(OptionToJson(c));
    }
    j["children"] = std::move(ch);
    return j;
}

bool OptionFromJson(const nlohmann::json& j, TrackerFieldOption& out) {
    if (!j.is_object()) {
        return false;
    }
    out.Id = j.value("id", std::string());
    out.Value = j.value("value", std::string());
    out.SecondaryValue = j.value("secondary", std::string());
    out.PayloadJson = j.value("payload_json", std::string());
    out.Disabled = j.value("disabled", false);
    out.Children.clear();
    const auto it = j.find("children");
    if (it != j.end() && it->is_array()) {
        for (const auto& el : *it) {
            TrackerFieldOption child;
            if (OptionFromJson(el, child)) {
                out.Children.push_back(std::move(child));
            }
        }
    }
    return true;
}

nlohmann::json FieldToJson(const TrackerField& f) {
    nlohmann::json j = nlohmann::json::object();
    j["id"] = f.Id;
    j["name"] = f.Name;
    j["type"] = f.Type;
    j["schema_system"] = f.SchemaSystem;
    j["schema_custom"] = f.SchemaCustom;
    j["read_only"] = f.ReadOnly;
    j["is_array"] = f.IsArray;
    j["items_type"] = f.ItemsType;
    j["is_user_type"] = f.IsUserType;
    j["is_custom"] = f.IsCustom;
    j["family"] = static_cast<int>(f.Family);
    j["allowed_values"] = f.AllowedValues;
    nlohmann::json opts = nlohmann::json::array();
    for (const auto& o : f.AllowedValueOptions) {
        opts.push_back(OptionToJson(o));
    }
    j["allowed_value_options"] = std::move(opts);
    j["raw_field_definition_json"] = f.RawFieldDefinitionJson;
    return j;
}

bool FieldFromJson(const nlohmann::json& j, TrackerField& out) {
    if (!j.is_object()) {
        return false;
    }
    out.Id = j.value("id", std::string());
    out.Name = j.value("name", std::string());
    out.Type = j.value("type", std::string());
    out.SchemaSystem = j.value("schema_system", std::string());
    out.SchemaCustom = j.value("schema_custom", std::string());
    out.ReadOnly = j.value("read_only", false);
    out.IsArray = j.value("is_array", false);
    out.ItemsType = j.value("items_type", std::string());
    out.IsUserType = j.value("is_user_type", false);
    out.IsCustom = j.value("is_custom", false);
    const int fam = j.value("family", 0);
    out.Family = static_cast<TrackerFieldFamily>(fam);
    out.AllowedValues.clear();
    const auto av = j.find("allowed_values");
    if (av != j.end() && av->is_array()) {
        for (const auto& el : *av) {
            if (el.is_string()) {
                out.AllowedValues.push_back(el.get<std::string>());
            }
        }
    }
    out.AllowedValueOptions.clear();
    const auto ao = j.find("allowed_value_options");
    if (ao != j.end() && ao->is_array()) {
        for (const auto& el : *ao) {
            TrackerFieldOption o;
            if (OptionFromJson(el, o)) {
                out.AllowedValueOptions.push_back(std::move(o));
            }
        }
    }
    out.RawFieldDefinitionJson = j.value("raw_field_definition_json", std::string());
    return true;
}

nlohmann::json IssueTypeMetaToJson(const TrackerIssueTypeCreateMeta& m) {
    nlohmann::json j = nlohmann::json::object();
    j["project_key"] = m.ProjectKey;
    j["issue_type_id"] = m.IssueTypeId;
    j["issue_type_name"] = m.IssueTypeName;
    j["is_subtask"] = m.IsSubtask;
    nlohmann::json req = nlohmann::json::array();
    for (const auto& id : m.RequiredFieldIds) {
        req.push_back(id);
    }
    j["required_field_ids"] = std::move(req);
    return j;
}

bool IssueTypeMetaFromJson(const nlohmann::json& j, TrackerIssueTypeCreateMeta& out) {
    if (!j.is_object()) {
        return false;
    }
    out.ProjectKey = j.value("project_key", std::string());
    out.IssueTypeId = j.value("issue_type_id", std::string());
    out.IssueTypeName = j.value("issue_type_name", std::string());
    out.IsSubtask = j.value("is_subtask", false);
    out.RequiredFieldIds.clear();
    const auto it = j.find("required_field_ids");
    if (it != j.end() && it->is_array()) {
        for (const auto& el : *it) {
            if (el.is_string()) {
                out.RequiredFieldIds.insert(el.get<std::string>());
            }
        }
    }
    return true;
}

} // namespace

namespace FieldCatalogCache {

bool SaveFieldCatalogSnapshot(const std::vector<TrackerField>& fields, const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError) {
    outError.clear();
    try {
        nlohmann::json root = nlohmann::json::object();
        root["schema_version"] = 1;
        nlohmann::json jf = nlohmann::json::array();
        for (const auto& f : fields) {
            jf.push_back(FieldToJson(f));
        }
        root["fields"] = std::move(jf);
        nlohmann::json jc = nlohmann::json::array();
        for (const auto& c : components) {
            jc.push_back(nlohmann::json{{"id", c.Id}, {"name", c.Name}});
        }
        root["components"] = std::move(jc);
        nlohmann::json jm = nlohmann::json::array();
        for (const auto& m : issueTypeMeta) {
            jm.push_back(IssueTypeMetaToJson(m));
        }
        root["issue_type_meta"] = std::move(jm);
        const std::string text = root.dump();
        if (!ConfigManager::AtomicWriteTextFile(FieldCatalogCachePath(), text)) {
            outError = "Failed to write field catalog cache file.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = ex.what();
        LOG_ERROR("FieldCatalogCache::SaveFieldCatalogSnapshot failed: %s", ex.what());
        return false;
    } catch (...) {
        outError = "Unknown error while saving field catalog cache.";
        LOG_ERROR("FieldCatalogCache::SaveFieldCatalogSnapshot failed: unknown exception");
        return false;
    }
}

bool TryLoadFieldCatalogSnapshot(std::vector<TrackerField>& outFields, std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError) {
    outError.clear();
    outFields.clear();
    outComponents.clear();
    outIssueTypeMeta.clear();
    const std::string path = FieldCatalogCachePath();
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        outError = "Field catalog cache file not found.";
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    if (text.empty()) {
        outError = "Field catalog cache file is empty.";
        return false;
    }
    try {
        const nlohmann::json root = nlohmann::json::parse(text);
        const int ver = root.value("schema_version", 0);
        if (ver != 1) {
            outError = "Unsupported field catalog cache schema version.";
            return false;
        }
        const auto jf = root.find("fields");
        if (jf == root.end() || !jf->is_array()) {
            outError = "Invalid field catalog cache: missing fields array.";
            return false;
        }
        for (const auto& el : *jf) {
            TrackerField f;
            if (FieldFromJson(el, f)) {
                outFields.push_back(std::move(f));
            }
        }
        const auto jc = root.find("components");
        if (jc != root.end() && jc->is_array()) {
            for (const auto& el : *jc) {
                if (!el.is_object()) {
                    continue;
                }
                TrackerComponent c;
                c.Id = el.value("id", std::string());
                c.Name = el.value("name", std::string());
                outComponents.push_back(std::move(c));
            }
        }
        const auto jm = root.find("issue_type_meta");
        if (jm != root.end() && jm->is_array()) {
            for (const auto& el : *jm) {
                TrackerIssueTypeCreateMeta m;
                if (IssueTypeMetaFromJson(el, m)) {
                    outIssueTypeMeta.push_back(std::move(m));
                }
            }
        }
        if (outFields.empty()) {
            outError = "Field catalog cache contained no fields.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = ex.what();
        LOG_ERROR("FieldCatalogCache::TryLoadFieldCatalogSnapshot parse failed: %s", ex.what());
        return false;
    } catch (...) {
        outError = "Unknown parse error for field catalog cache.";
        LOG_ERROR("FieldCatalogCache::TryLoadFieldCatalogSnapshot parse failed: unknown exception");
        return false;
    }
}

} // namespace FieldCatalogCache
