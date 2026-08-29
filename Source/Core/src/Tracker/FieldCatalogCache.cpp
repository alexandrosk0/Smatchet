#include "FieldCatalogCache.h"

#include "TimeNowPure.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "Json/BoundedJsonParse.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>

namespace {
// FieldCatalogCache exposes free functions and reads/writes a single JSON file under the user data
// directory. An `entries` index and per-read lastUsedUnix touches exist; we serialize all
// file-modifying paths so concurrent UI/HTTP/offline-replay threads don't race on read-modify-write.
std::mutex& FieldCatalogCacheFileMutex() {
    static std::mutex m;
    return m;
}

constexpr int kFieldCatalogCacheSchemaVersion = 3;
constexpr int kDefaultMaxCachedProjects = 16;

} // namespace

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
    std::transform(o.Children.begin(), o.Children.end(), std::back_inserter(ch),
                   [](const auto& c) { return OptionToJson(c); });
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
    std::transform(f.AllowedValueOptions.begin(), f.AllowedValueOptions.end(), std::back_inserter(opts),
                   [](const auto& o) { return OptionToJson(o); });
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
    std::transform(fields.begin(), fields.end(), std::back_inserter(jf), [](const auto& f) { return FieldToJson(f); });
    entry["fields"] = std::move(jf);
    nlohmann::json jc = nlohmann::json::array();
    std::transform(components.begin(), components.end(), std::back_inserter(jc),
                   [](const auto& c) { return nlohmann::json{{"id", c.Id}, {"name", c.Name}}; });
    entry["components"] = std::move(jc);
    nlohmann::json jm = nlohmann::json::array();
    std::transform(issueTypeMeta.begin(), issueTypeMeta.end(), std::back_inserter(jm),
                   [](const auto& m) { return IssueTypeMetaToJson(m); });
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

// Split parse/normalize of the on-disk root so Save/Load/List/Forget share migration logic.
// Schema 1: flat top-level "fields"/"components"/"issue_type_meta" (Jira only, pre-multi-backend).
// Schema 2: { "schema_version": 2, "entries": { "<cacheKey>": { fields, components, issue_type_meta } } }.
// Schema 3: { "schema_version": 3, "entries": [ { cacheKey, projectKey, backend, endpoint, lastUsedUnix } ... ],
//             "<cacheKey>": <entry blob>, ... }.
// MigrateOnDiskRootToV3 produces a v3-shaped root (entries-array index + per-cacheKey blobs at root).
// Missing metadata (backend/endpoint/projectKey from a v1/v2 entry) is left empty in the index entry
// and populated on the next Save for that cacheKey.
nlohmann::json MigrateOnDiskRootToV3(const nlohmann::json& rootOnDisk) {
    nlohmann::json out = nlohmann::json::object();
    out["schema_version"] = kFieldCatalogCacheSchemaVersion;
    nlohmann::json indexArr = nlohmann::json::array();

    const int oldVer = rootOnDisk.is_object() ? rootOnDisk.value("schema_version", 0) : 0;
    const std::int64_t now = TimeNowPure::NowUnixSeconds();

    auto appendIndexEntry = [&](const std::string& cacheKey, const std::string& projectKey, const std::string& backend,
                                const std::string& endpoint, std::int64_t lastUsed) {
        nlohmann::json e = nlohmann::json::object();
        e["cacheKey"] = cacheKey;
        e["projectKey"] = projectKey;
        e["backend"] = backend;
        e["endpoint"] = endpoint;
        e["lastUsedUnix"] = lastUsed;
        indexArr.push_back(std::move(e));
    };

    if (oldVer >= 3 && rootOnDisk.contains("entries") && rootOnDisk["entries"].is_array()) {
        // v3 → v3: preserve the index as-is.
        for (const auto& idx : rootOnDisk["entries"]) {
            if (!idx.is_object())
                continue;
            const std::string cacheKey = idx.value("cacheKey", std::string());
            if (cacheKey.empty())
                continue;
            appendIndexEntry(cacheKey, idx.value("projectKey", std::string()), idx.value("backend", std::string()),
                             idx.value("endpoint", std::string()), idx.value("lastUsedUnix", now));
        }
        // Preserve per-cacheKey blobs at root (everything except schema_version + entries).
        for (auto it = rootOnDisk.begin(); it != rootOnDisk.end(); ++it) {
            if (it.key() == "schema_version" || it.key() == "entries")
                continue;
            out[it.key()] = it.value();
        }
        return out;
    }

    if (oldVer == 2 && rootOnDisk.contains("entries") && rootOnDisk["entries"].is_object()) {
        // v2 → v3: hoist each cacheKey blob from entries-object to root, add an index entry per blob
        // with empty backend/endpoint/projectKey (next Save backfills them). lastUsedUnix = now so a
        // newly-upgraded cache doesn't evict good entries on the first write.
        for (auto it = rootOnDisk["entries"].begin(); it != rootOnDisk["entries"].end(); ++it) {
            const std::string& cacheKey = it.key();
            if (!it.value().is_object())
                continue;
            out[cacheKey] = it.value();
            appendIndexEntry(cacheKey, std::string(), std::string(), std::string(), now);
        }
        out["entries"] = std::move(indexArr);
        return out;
    }

    if (rootOnDisk.is_object() && rootOnDisk.contains("fields") && rootOnDisk["fields"].is_array()) {
        // v1 → v3: legacy flat layout was Jira-only; store under the historical "Jira_legacy_v1" key
        // (TryLoadFieldCatalogSnapshot below substitutes this when a "Jira|..." key isn't found).
        nlohmann::json legacyEntry = nlohmann::json::object();
        legacyEntry["fields"] = rootOnDisk["fields"];
        legacyEntry["components"] =
            rootOnDisk.contains("components") ? rootOnDisk["components"] : nlohmann::json::array();
        legacyEntry["issue_type_meta"] =
            rootOnDisk.contains("issue_type_meta") ? rootOnDisk["issue_type_meta"] : nlohmann::json::array();
        out["Jira_legacy_v1"] = std::move(legacyEntry);
        appendIndexEntry("Jira_legacy_v1", std::string(), "Jira", std::string(), now);
    }

    out["entries"] = std::move(indexArr);
    return out;
}

// Load the on-disk JSON, migrate to v3 in memory. On parse failure returns a fresh empty v3 root
// and logs a WARN — matches the corrupt-cache wipe pattern.
nlohmann::json LoadAndMigrateRootLocked() {
    const std::string path = FieldCatalogCachePath();
    nlohmann::json rootOnDisk = nlohmann::json::object();
    std::ifstream inf(path, std::ios::binary);
    if (inf) {
        std::string text((std::istreambuf_iterator<char>(inf)), std::istreambuf_iterator<char>());
        if (!text.empty()) {
            std::string parseErr;
            rootOnDisk = smatchet::json_safe::ParseBounded(text, parseErr);
            if (!parseErr.empty()) {
                LOG_WARN("FieldCatalogCache: ignoring unreadable cache, wiping to fresh v%d: %s",
                         kFieldCatalogCacheSchemaVersion, parseErr.c_str());
                rootOnDisk = nlohmann::json::object();
            }
        }
    }
    return MigrateOnDiskRootToV3(rootOnDisk);
}

// Locate the index array entry for `cacheKey`. Returns end() if not found.
nlohmann::json::iterator FindIndexEntry(nlohmann::json& indexArr, const std::string& cacheKey) {
    for (auto it = indexArr.begin(); it != indexArr.end(); ++it) {
        if (it->is_object() && it->value("cacheKey", std::string()) == cacheKey) {
            return it;
        }
    }
    return indexArr.end();
}

// Sort the index array by lastUsedUnix descending and drop everything past `cap-1`, also freeing
// the per-cacheKey blobs at root. cap is clamped to a minimum of 1.
void EvictLeastRecentlyUsedIfOverCap(nlohmann::json& root, int cap) {
    if (cap < 1)
        cap = 1;
    if (!root.contains("entries") || !root["entries"].is_array())
        return;
    nlohmann::json& indexArr = root["entries"];
    std::sort(indexArr.begin(), indexArr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("lastUsedUnix", std::int64_t{0}) > b.value("lastUsedUnix", std::int64_t{0});
    });
    while (static_cast<int>(indexArr.size()) > cap) {
        const nlohmann::json& victim = indexArr.back();
        const std::string victimKey = victim.value("cacheKey", std::string());
        const std::string victimProject = victim.value("projectKey", std::string());
        const std::int64_t victimLast = victim.value("lastUsedUnix", std::int64_t{0});
        if (!victimKey.empty() && root.contains(victimKey)) {
            root.erase(victimKey);
        }
        LOG_INFO("FieldCatalogCache LRU evicted project='%s' (cap=%d, lastUsed=%lld)", victimProject.c_str(), cap,
                 static_cast<long long>(victimLast));
        indexArr.erase(indexArr.end() - 1);
    }
}

bool PersistRootLocked(const nlohmann::json& root, std::string& outError) {
    try {
        const std::string path = FieldCatalogCachePath();
        const std::string text = root.dump();
        if (!ConfigManager::AtomicWriteTextFile(path, text)) {
            outError = "Failed to write field catalog cache file.";
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        outError = ex.what();
        return false;
    } catch (...) {
        LOG_ERROR("FieldCatalogCache::PersistFieldCatalog: unknown exception");
        outError = "Unknown error while persisting field catalog cache.";
        return false;
    }
}

} // namespace

namespace FieldCatalogCache {

std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey) {
    const std::string bk = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (bk == "Plane") {
        return std::string("Plane|") + NormalizeEndpointForCache(cfg.PlaneUrl) + "|" + cfg.PlaneWorkspaceSlug + "|" +
               projectKey;
    }
    if (bk == "Linear") {
        return std::string("Linear|") + NormalizeEndpointForCache(cfg.LinearBaseUrl) + "|" + cfg.LinearTeamId + "|" +
               projectKey;
    }
    return std::string("Jira|") + NormalizeEndpointForCache(cfg.Domain) + "|" + projectKey;
}

bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::string& backend, const std::string& endpoint,
                              const std::string& projectKey, int maxProjects, const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError) {
    outError.clear();
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();
        root[cacheKey] = BuildEntryJson(fields, components, issueTypeMeta);

        // Upsert the index entry — backfills (backend, endpoint, projectKey) for entries that came
        // from v2/v1 migration with empty metadata.
        if (!root.contains("entries") || !root["entries"].is_array()) {
            root["entries"] = nlohmann::json::array();
        }
        nlohmann::json& indexArr = root["entries"];
        const std::int64_t now = TimeNowPure::NowUnixSeconds();
        auto it = FindIndexEntry(indexArr, cacheKey);
        if (it == indexArr.end()) {
            nlohmann::json e = nlohmann::json::object();
            e["cacheKey"] = cacheKey;
            e["projectKey"] = projectKey;
            e["backend"] = backend;
            e["endpoint"] = endpoint;
            e["lastUsedUnix"] = now;
            indexArr.push_back(std::move(e));
        } else {
            (*it)["projectKey"] = projectKey;
            (*it)["backend"] = backend;
            (*it)["endpoint"] = endpoint;
            (*it)["lastUsedUnix"] = now;
        }

        const int cap = maxProjects > 0 ? maxProjects : kDefaultMaxCachedProjects;
        EvictLeastRecentlyUsedIfOverCap(root, cap);

        return PersistRootLocked(root, outError);
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
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();

        // Resolve the blob: prefer exact cacheKey match; fall back to "Jira_legacy_v1" for Jira keys
        // (mirrors the previous v2 behavior so a fresh upgrade keeps reading the v1 snapshot).
        std::string resolvedKey = cacheKey;
        if (!root.contains(resolvedKey) || !root[resolvedKey].is_object()) {
            if (cacheKey.rfind("Jira|", 0) == 0 && root.contains("Jira_legacy_v1") &&
                root["Jira_legacy_v1"].is_object()) {
                resolvedKey = "Jira_legacy_v1";
            } else {
                outError = "No field catalog cache entry for this tracker context.";
                return false;
            }
        }
        const bool ok =
            ParseCatalogEntryObject(root[resolvedKey], outFields, outComponents, outIssueTypeMeta, outError);
        if (!ok) {
            return false;
        }

        // Touch lastUsedUnix on the index entry (under the resolved key, so legacy reads also bump
        // the Jira_legacy_v1 entry's timestamp). Best-effort: a write failure here is logged but the
        // caller still gets the loaded data — the cache is a snapshot, not the source of truth.
        if (root.contains("entries") && root["entries"].is_array()) {
            auto it = FindIndexEntry(root["entries"], resolvedKey);
            if (it != root["entries"].end()) {
                (*it)["lastUsedUnix"] = TimeNowPure::NowUnixSeconds();
                std::string writeErr;
                if (!PersistRootLocked(root, writeErr)) {
                    LOG_WARN("FieldCatalogCache::TryLoadFieldCatalogSnapshot: failed to touch LRU index: %s",
                             writeErr.c_str());
                }
            }
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

std::vector<CachedProjectEntry> ListCachedProjects() {
    std::vector<CachedProjectEntry> out;
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();
        if (!root.contains("entries") || !root["entries"].is_array()) {
            return out;
        }
        for (const auto& e : root["entries"]) {
            if (!e.is_object())
                continue;
            CachedProjectEntry cpe;
            cpe.projectKey = e.value("projectKey", std::string());
            cpe.backend = e.value("backend", std::string());
            cpe.endpoint = e.value("endpoint", std::string());
            cpe.lastUsedUnix = e.value("lastUsedUnix", std::int64_t{0});
            out.push_back(std::move(cpe));
        }
        std::sort(out.begin(), out.end(), [](const CachedProjectEntry& a, const CachedProjectEntry& b) {
            return a.lastUsedUnix > b.lastUsedUnix;
        });
    } catch (const std::exception& ex) {
        LOG_WARN("FieldCatalogCache::ListCachedProjects failed: %s", ex.what());
    } catch (...) {
        LOG_WARN("FieldCatalogCache::ListCachedProjects failed: unknown exception");
    }
    return out;
}

bool ForgetProject(const std::string& projectKey, const std::string& backend, const std::string& endpoint) {
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();
        if (!root.contains("entries") || !root["entries"].is_array()) {
            return true;
        }
        nlohmann::json& indexArr = root["entries"];
        bool removed = false;
        for (auto it = indexArr.begin(); it != indexArr.end();) {
            const bool matches = it->is_object() && it->value("projectKey", std::string()) == projectKey &&
                                 it->value("backend", std::string()) == backend &&
                                 it->value("endpoint", std::string()) == endpoint;
            if (matches) {
                const std::string victimKey = it->value("cacheKey", std::string());
                if (!victimKey.empty() && root.contains(victimKey)) {
                    root.erase(victimKey);
                }
                it = indexArr.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
        if (!removed) {
            return true; // Not-found is success — caller doesn't need to distinguish.
        }
        std::string writeErr;
        const bool ok = PersistRootLocked(root, writeErr);
        if (!ok) {
            LOG_WARN("FieldCatalogCache::ForgetProject: persist failed: %s", writeErr.c_str());
        }
        return ok;
    } catch (const std::exception& ex) {
        LOG_WARN("FieldCatalogCache::ForgetProject failed: %s", ex.what());
        return false;
    } catch (...) {
        LOG_WARN("FieldCatalogCache::ForgetProject failed: unknown exception");
        return false;
    }
}

} // namespace FieldCatalogCache
