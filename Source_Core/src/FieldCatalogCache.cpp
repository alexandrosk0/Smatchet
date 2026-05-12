#include "FieldCatalogCache.h"

#include "ConfigManager.h"
#include "Logger.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {

void TrimAsciiWs(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.erase(0, 1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.pop_back();
    }
}

std::string NormalizeEndpointForCache(std::string value) {
    TrimAsciiWs(value);
    while (!value.empty() && (value.back() == '/' || value.back() == '\\')) {
        value.pop_back();
    }
    const auto scheme = value.find("://");
    if (scheme == std::string::npos) {
        return value;
    }
    const size_t hostStart = scheme + 3;
    size_t hostEnd = value.find('/', hostStart);
    if (hostEnd == std::string::npos) {
        hostEnd = value.size();
    }
    std::string host = value.substr(hostStart, hostEnd - hostStart);
    std::string portSuffix;
    const size_t colon = host.find(':');
    if (colon != std::string::npos) {
        portSuffix = host.substr(colon);
        host.resize(colon);
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::string schemePrefix = value.substr(0, hostStart);
    if (host == "app.plane.so") {
        return schemePrefix + "api.plane.so" + portSuffix;
    }
    return schemePrefix + host + portSuffix;
}

std::string FieldCatalogCachePath() {
    const std::string& base = ConfigManager::GetUserDataDirectory();
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
    std::transform(o.Children.begin(), o.Children.end(), std::back_inserter(ch), [](const auto& c) {
        return OptionToJson(c);
    });
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
    std::transform(f.AllowedValueOptions.begin(), f.AllowedValueOptions.end(), std::back_inserter(opts), [](const auto& o) {
        return OptionToJson(o);
    });
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
    std::copy(m.RequiredFieldIds.begin(), m.RequiredFieldIds.end(), std::back_inserter(req));
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

nlohmann::json BuildEntryJson(const std::vector<TrackerField>& fields, const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta) {
    nlohmann::json entry = nlohmann::json::object();
    nlohmann::json jf = nlohmann::json::array();
    std::transform(fields.begin(), fields.end(), std::back_inserter(jf), [](const auto& f) {
        return FieldToJson(f);
    });
    entry["fields"] = std::move(jf);
    nlohmann::json jc = nlohmann::json::array();
    std::transform(components.begin(), components.end(), std::back_inserter(jc), [](const auto& c) {
        return nlohmann::json{{"id", c.Id}, {"name", c.Name}};
    });
    entry["components"] = std::move(jc);
    nlohmann::json jm = nlohmann::json::array();
    std::transform(issueTypeMeta.begin(), issueTypeMeta.end(), std::back_inserter(jm), [](const auto& m) {
        return IssueTypeMetaToJson(m);
    });
    entry["issue_type_meta"] = std::move(jm);
    return entry;
}

bool ParseCatalogEntryObject(const nlohmann::json& entryRoot, std::vector<TrackerField>& outFields,
                             std::vector<TrackerComponent>& outComponents,
                             std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError) {
    outError.clear();
    outFields.clear();
    outComponents.clear();
    outIssueTypeMeta.clear();
    const auto jf = entryRoot.find("fields");
    if (jf == entryRoot.end() || !jf->is_array()) {
        outError = "Invalid field catalog cache entry: missing fields array.";
        return false;
    }
    for (const auto& el : *jf) {
        TrackerField f;
        if (FieldFromJson(el, f)) {
            outFields.push_back(std::move(f));
        }
    }
    const auto jc = entryRoot.find("components");
    if (jc != entryRoot.end() && jc->is_array()) {
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
    const auto jm = entryRoot.find("issue_type_meta");
    if (jm != entryRoot.end() && jm->is_array()) {
        for (const auto& el : *jm) {
            TrackerIssueTypeCreateMeta m;
            if (IssueTypeMetaFromJson(el, m)) {
                outIssueTypeMeta.push_back(std::move(m));
            }
        }
    }
    if (outFields.empty()) {
        outError = "Field catalog cache entry contained no fields.";
        return false;
    }
    return true;
}

} // namespace

namespace FieldCatalogCache {

std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey) {
    const std::string bk = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (bk == "Plane") {
        return std::string("Plane|") + NormalizeEndpointForCache(cfg.PlaneUrl) + "|" + cfg.PlaneWorkspaceSlug + "|" +
               projectKey;
    }
    return std::string("Jira|") + NormalizeEndpointForCache(cfg.Domain) + "|" + projectKey;
}

bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError) {
    outError.clear();
    try {
        const std::string path = FieldCatalogCachePath();
        nlohmann::json rootOnDisk = nlohmann::json::object();
        {
            std::ifstream inf(path, std::ios::binary);
            if (inf) {
                std::string text((std::istreambuf_iterator<char>(inf)), std::istreambuf_iterator<char>());
                if (!text.empty()) {
                    try {
                        rootOnDisk = nlohmann::json::parse(text);
                    } catch (const std::exception& ex) {
                        LOG_WARN("FieldCatalogCache::SaveFieldCatalogSnapshot: ignoring unreadable cache: %s",
                                 ex.what());
                        rootOnDisk = nlohmann::json::object();
                    }
                }
            }
        }

        nlohmann::json out = nlohmann::json::object();
        out["schema_version"] = 2;
        nlohmann::json entries = nlohmann::json::object();

        const int oldVer = rootOnDisk.value("schema_version", 0);
        if (oldVer == 2 && rootOnDisk.contains("entries") && rootOnDisk["entries"].is_object()) {
            entries = rootOnDisk["entries"];
        } else if (rootOnDisk.contains("fields") && rootOnDisk["fields"].is_array()) {
            nlohmann::json legacyEntry = nlohmann::json::object();
            legacyEntry["fields"] = rootOnDisk["fields"];
            legacyEntry["components"] =
                rootOnDisk.contains("components") ? rootOnDisk["components"] : nlohmann::json::array();
            legacyEntry["issue_type_meta"] =
                rootOnDisk.contains("issue_type_meta") ? rootOnDisk["issue_type_meta"] : nlohmann::json::array();
            entries["Jira_legacy_v1"] = std::move(legacyEntry);
        }

        entries[cacheKey] = BuildEntryJson(fields, components, issueTypeMeta);
        out["entries"] = std::move(entries);
        const std::string text = out.dump();
        if (!ConfigManager::AtomicWriteTextFile(path, text)) {
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

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
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
        if (ver == 2 && root.contains("entries") && root["entries"].is_object()) {
            const nlohmann::json& entries = root["entries"];
            nlohmann::json::const_iterator it = entries.find(cacheKey);
            if (it == entries.end() && cacheKey.rfind("Jira|", 0) == 0) {
                it = entries.find("Jira_legacy_v1");
            }
            if (it == entries.end() || !it->is_object()) {
                outError = "No field catalog cache entry for this tracker context.";
                return false;
            }
            return ParseCatalogEntryObject(*it, outFields, outComponents, outIssueTypeMeta, outError);
        }
        if (root.contains("fields") && root["fields"].is_array()) {
            if (cacheKey.rfind("Plane|", 0) == 0) {
                outError = "Legacy field catalog cache is Jira-only; Plane snapshot not available.";
                return false;
            }
            if (cacheKey.rfind("Jira|", 0) != 0) {
                outError = "Unsupported cache key for legacy field catalog file.";
                return false;
            }
            return ParseCatalogEntryObject(root, outFields, outComponents, outIssueTypeMeta, outError);
        }
        outError = "Unsupported or empty field catalog cache format.";
        return false;
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






