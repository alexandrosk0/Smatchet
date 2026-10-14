#include "FieldCatalogCache.h"

#include "FieldOptionsJsonPure.h"
#include "TimeNowPure.h"

#include "ConfigManager.h"
#include "Logger.h"
#include "Json/BoundedJsonParse.h"
#include "JsonParseUtil.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_set>
#include <unordered_map>

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
// A restore moves its snapshot up the LRU order on disk at most this often: each move rewrites the whole
// cache file, and a restore can run on the UI thread. Between those writes the touch stays in memory.
constexpr std::int64_t kLruTouchIntervalSeconds = 24 * 60 * 60;
// The base URLs Preferences fills in, and the clients use, when the configured one is empty.
constexpr const char* kGitHubDefaultBaseUrl = "https://api.github.com";
constexpr const char* kLinearDefaultBaseUrl = "https://api.linear.app/graphql";

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
                   [](const auto& o) { return smatchet::fieldoptions::FieldOptionToJson(o); });
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
            if (smatchet::fieldoptions::FieldOptionFromJson(el, o)) {
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
// A hand-edited or damaged index entry must cost only itself, never the whole file: read each scalar with a
// type guard (nlohmann `value()` throws on a mismatched type).
std::int64_t IntOr(const nlohmann::json& obj, const char* key, std::int64_t fallback) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_number_integer()) ? it->get<std::int64_t>() : fallback;
}

std::string StrOr(const nlohmann::json& obj, const char* key) {
    const auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// "Last used" times recorded by loads. A load must not rewrite the cache file (it runs on the UI thread at
// startup and from the render-path error handler), so the touch waits here and reaches disk with the next
// save / forget. Guarded by FieldCatalogCacheFileMutex().
std::unordered_map<std::string, std::int64_t>& PendingTouches() {
    static std::unordered_map<std::string, std::int64_t> touches;
    return touches;
}

// Folds the pending touches into `root`'s index (and forgets them when `consume`).
void ApplyPendingTouches(nlohmann::json& root, bool consume) {
    std::unordered_map<std::string, std::int64_t>& pending = PendingTouches();
    if (pending.empty()) {
        return;
    }
    if (root.contains("entries") && root["entries"].is_array()) {
        for (nlohmann::json& e : root["entries"]) {
            if (!e.is_object()) {
                continue;
            }
            const auto it = pending.find(StrOr(e, "cacheKey"));
            if (it != pending.end() && it->second > IntOr(e, "lastUsedUnix", 0)) {
                e["lastUsedUnix"] = it->second;
            }
        }
    }
    if (consume) {
        pending.clear();
    }
}

// Schema 3: { "schema_version": 3, "entries": [ { cacheKey, projectKey, backend, endpoint, lastUsedUnix } ... ],
//             "<cacheKey>": <entry blob>, ... }.
// MigrateOnDiskRootToV3 produces a v3-shaped root (entries-array index + per-cacheKey blobs at root).
// Missing metadata (backend/endpoint/projectKey from a v1/v2 entry) is left empty in the index entry
// and populated on the next Save for that cacheKey.
// The root keys that are not snapshot blobs. An index entry naming one would let eviction or Forget erase
// the index or the schema version itself.
bool IsReservedRootKey(const std::string& key) { return key == "schema_version" || key == "entries"; }

// Typed reads that fall back instead of throwing: a hand-edited or corrupt field must not make every
// later load, save, listing or Forget fail.
bool JsonBoolOr(const nlohmann::json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}

// One v3 index entry. A time ahead of the clock (a clock moved back, a hand edit) would pin the entry first
// in the LRU order for good: it counts as used now.
void AppendIndexEntry(nlohmann::json& indexArr, std::int64_t now, const std::string& cacheKey,
                      const std::string& projectKey, const std::string& backend, const std::string& endpoint,
                      std::int64_t lastUsed, bool kindKeyed, std::int64_t savedUnix = 0) {
    nlohmann::json e = nlohmann::json::object();
    e["cacheKey"] = cacheKey;
    e["projectKey"] = projectKey;
    e["backend"] = backend;
    e["endpoint"] = endpoint;
    e["lastUsedUnix"] = lastUsed > now ? now : lastUsed;
    e["kindKeyed"] = kindKeyed;
    if (savedUnix > 0) {
        e["savedUnix"] = savedUnix > now ? now : savedUnix;
    }
    indexArr.push_back(std::move(e));
}

// Carries a v3 root's index into `indexArr`, minus entries that name no blob, a reserved key or a key twice.
// Returns the cache keys it indexed.
std::unordered_set<std::string> CarryV3Index(const nlohmann::json& rootOnDisk, nlohmann::json& indexArr,
                                             std::int64_t now) {
    std::unordered_set<std::string> indexed;
    const auto entriesIt = rootOnDisk.find("entries");
    if (entriesIt == rootOnDisk.end() || !entriesIt->is_array())
        return indexed;
    for (const auto& idx : *entriesIt) {
        if (!idx.is_object())
            continue;
        const std::string cacheKey = JsonStringFieldOr(idx, "cacheKey");
        if (cacheKey.empty() || IsReservedRootKey(cacheKey) || !rootOnDisk.contains(cacheKey) ||
            !rootOnDisk[cacheKey].is_object() || !indexed.insert(cacheKey).second)
            continue;
        AppendIndexEntry(indexArr, now, cacheKey, JsonStringFieldOr(idx, "projectKey"),
                         JsonStringFieldOr(idx, "backend"), JsonStringFieldOr(idx, "endpoint"),
                         ParseJsonInt64FieldLoose(idx, "lastUsedUnix", now), JsonBoolOr(idx, "kindKeyed"),
                         ParseJsonInt64FieldLoose(idx, "savedUnix", 0));
    }
    return indexed;
}

nlohmann::json MigrateOnDiskRootToV3(const nlohmann::json& rootOnDisk) {
    nlohmann::json out = nlohmann::json::object();
    out["schema_version"] = kFieldCatalogCacheSchemaVersion;
    nlohmann::json indexArr = nlohmann::json::array();

    const std::int64_t oldVer = rootOnDisk.is_object() ? ParseJsonInt64FieldLoose(rootOnDisk, "schema_version", 0) : 0;
    const std::int64_t now = TimeNowPure::NowUnixSeconds();

    auto appendIndexEntry = [&](const std::string& cacheKey, const std::string& projectKey, const std::string& backend,
                                const std::string& endpoint, std::int64_t lastUsed, bool kindKeyed) {
        AppendIndexEntry(indexArr, now, cacheKey, projectKey, backend, endpoint, lastUsed, kindKeyed);
    };

    // Preserves per-cacheKey blobs at root (every object but the reserved keys) not already in `out`. A
    // blob the index does not name (earlier builds dropped the index on every load, so no blob was ever
    // evicted) is indexed with no metadata and lastUsedUnix 0: the LRU cap evicts it first, and a save
    // under its key fills the metadata in. The blob's own kindKeyed survives a lost index.
    auto keepRootBlobs = [&](std::unordered_set<std::string>& indexed) {
        for (auto it = rootOnDisk.begin(); it != rootOnDisk.end(); ++it) {
            if (IsReservedRootKey(it.key()) || !it.value().is_object() || out.contains(it.key()))
                continue;
            out[it.key()] = it.value();
            if (indexed.insert(it.key()).second) {
                appendIndexEntry(it.key(), std::string(), std::string(), std::string(), std::int64_t{0},
                                 JsonBoolOr(it.value(), "kindKeyed"));
            }
        }
    };

    const bool v2 = oldVer == 2 && rootOnDisk.contains("entries") && rootOnDisk["entries"].is_object();
    const bool v1 = !v2 && rootOnDisk.is_object() && rootOnDisk.contains("fields") && rootOnDisk["fields"].is_array();
    if (rootOnDisk.is_object() && !v1 && !v2) {
        // v3 → v3: preserve the index, minus entries that name no blob, a reserved key or a key twice. A
        // root whose index or schema version is damaged still keeps its blobs (indexed as below).
        std::unordered_set<std::string> indexed = CarryV3Index(rootOnDisk, indexArr, now);
        keepRootBlobs(indexed);
        out["entries"] = std::move(indexArr);
        return out;
    }

    if (v2) {
        // v2 → v3: hoist each cacheKey blob from entries-object to root, add an index entry per blob
        // with empty backend/endpoint/projectKey (next Save backfills them). lastUsedUnix = now so a
        // newly-upgraded cache doesn't evict good entries on the first write. A v3 file damaged into
        // this shape (schema_version 2, an object index) keeps the blobs it holds at root.
        std::unordered_set<std::string> indexed;
        for (auto it = rootOnDisk["entries"].begin(); it != rootOnDisk["entries"].end(); ++it) {
            const std::string& cacheKey = it.key();
            if (!it.value().is_object())
                continue;
            if (IsReservedRootKey(cacheKey))
                continue;
            out[cacheKey] = it.value();
            indexed.insert(cacheKey);
            appendIndexEntry(cacheKey, std::string(), std::string(), std::string(), now, false);
        }
        keepRootBlobs(indexed);
        out["entries"] = std::move(indexArr);
        return out;
    }

    if (v1) {
        // v1 → v3: legacy flat layout was Jira-only; store under the historical "Jira_legacy_v1" key
        // (TryLoadFieldCatalogSnapshot below substitutes this when a "Jira|..." key isn't found).
        nlohmann::json legacyEntry = nlohmann::json::object();
        legacyEntry["fields"] = rootOnDisk["fields"];
        legacyEntry["components"] =
            rootOnDisk.contains("components") ? rootOnDisk["components"] : nlohmann::json::array();
        legacyEntry["issue_type_meta"] =
            rootOnDisk.contains("issue_type_meta") ? rootOnDisk["issue_type_meta"] : nlohmann::json::array();
        out["Jira_legacy_v1"] = std::move(legacyEntry);
        appendIndexEntry("Jira_legacy_v1", std::string(), "Jira", std::string(), now, false);
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
        if (it->is_object() && StrOr(*it, "cacheKey") == cacheKey) {
            return it;
        }
    }
    return indexArr.end();
}

// Sort the index array by lastUsedUnix descending and drop everything past `cap`, also freeing the
// per-cacheKey blobs at root. cap is clamped to a minimum of 1. `keepKey` (the entry just saved) sorts
// first: entries saved within the same second tie on lastUsedUnix, and it must never be the victim.
void EvictLeastRecentlyUsedIfOverCap(nlohmann::json& root, int cap, const std::string& keepKey) {
    if (cap < 1)
        cap = 1;
    if (!root.contains("entries") || !root["entries"].is_array())
        return;
    nlohmann::json& indexArr = root["entries"];
    std::stable_sort(indexArr.begin(), indexArr.end(), [&keepKey](const nlohmann::json& a, const nlohmann::json& b) {
        const bool aKept = StrOr(a, "cacheKey") == keepKey;
        const bool bKept = StrOr(b, "cacheKey") == keepKey;
        if (aKept != bKept) {
            return aKept;
        }
        return IntOr(a, "lastUsedUnix", 0) > IntOr(b, "lastUsedUnix", 0);
    });
    while (static_cast<int>(indexArr.size()) > cap) {
        const nlohmann::json& victim = indexArr.back();
        const std::string victimKey = StrOr(victim, "cacheKey");
        const std::string victimProject = StrOr(victim, "projectKey");
        const std::int64_t victimLast = IntOr(victim, "lastUsedUnix", 0);
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

std::string EffectiveGitHubBaseUrl(const TrackerConfig& cfg) {
    return cfg.GitHubBaseUrl.empty() ? std::string(kGitHubDefaultBaseUrl) : cfg.GitHubBaseUrl;
}

std::string EffectiveLinearBaseUrl(const TrackerConfig& cfg) {
    return cfg.LinearBaseUrl.empty() ? std::string(kLinearDefaultBaseUrl) : cfg.LinearBaseUrl;
}

} // namespace

namespace FieldCatalogCache {

std::string BuildFieldCatalogCacheKey(const TrackerConfig& cfg, const std::string& projectKey) {
    const std::string bk = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (bk == "Plane") {
        return std::string("Plane|") + NormalizeEndpointForCache(cfg.PlaneUrl) + "|" + cfg.PlaneWorkspaceSlug + "|" +
               projectKey;
    }
    if (bk == "GitHub") {
        return std::string("GitHub|") + NormalizeEndpointForCache(EffectiveGitHubBaseUrl(cfg)) + "|" + cfg.GitHubOwner +
               "/" + cfg.GitHubRepo + "|" + projectKey;
    }
    if (bk == "Linear") {
        return std::string("Linear|") + NormalizeEndpointForCache(EffectiveLinearBaseUrl(cfg)) + "|" +
               cfg.LinearTeamId + "|" + projectKey;
    }
    return std::string("Jira|") + NormalizeEndpointForCache(cfg.Domain) + "|" + projectKey;
}

FieldCatalogIndexIdentity BuildFieldCatalogIndexIdentity(const TrackerConfig& cfg) {
    FieldCatalogIndexIdentity id;
    id.backend = ConfigManager::NormalizeViewsBackendKey(cfg.TrackerType);
    if (id.backend == "Plane") {
        id.endpoint = cfg.PlaneUrl + "|" + cfg.PlaneWorkspaceSlug;
    } else if (id.backend == "GitHub") {
        id.endpoint = EffectiveGitHubBaseUrl(cfg) + "|" + cfg.GitHubOwner + "/" + cfg.GitHubRepo;
    } else if (id.backend == "Linear") {
        id.endpoint = EffectiveLinearBaseUrl(cfg) + "|" + cfg.LinearTeamId;
    } else {
        id.endpoint = cfg.Domain;
    }
    return id;
}

bool SaveFieldCatalogSnapshot(const std::string& cacheKey, const std::string& backend, const std::string& endpoint,
                              const std::string& projectKey, int maxProjects, const std::vector<TrackerField>& fields,
                              const std::vector<TrackerComponent>& components,
                              const std::vector<TrackerIssueTypeCreateMeta>& issueTypeMeta, std::string& outError) {
    outError.clear();
    if (cacheKey.empty() || IsReservedRootKey(cacheKey)) {
        outError = "Invalid field catalog cache key.";
        return false;
    }
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();
        root[cacheKey] = BuildEntryJson(fields, components, issueTypeMeta);
        // The blob carries the flag too, so a damaged index cannot make this build's own snapshot read as
        // an older build's.
        root[cacheKey]["kindKeyed"] = true;

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
            e["kindKeyed"] = true;
            e["savedUnix"] = now; // when the catalog was fetched; lastUsedUnix also moves on a read
            indexArr.push_back(std::move(e));
        } else {
            (*it)["projectKey"] = projectKey;
            (*it)["backend"] = backend;
            (*it)["endpoint"] = endpoint;
            (*it)["lastUsedUnix"] = now;
            (*it)["kindKeyed"] = true;
            (*it)["savedUnix"] = now;
        }

        ApplyPendingTouches(root, /*consume=*/true); // the write is happening anyway: loads' touches ride along
        const int cap = maxProjects > 0 ? maxProjects : kDefaultMaxCachedProjects;
        EvictLeastRecentlyUsedIfOverCap(root, cap, cacheKey);

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

namespace {

// Builds before per-tracker keys saved a GitHub pane's catalog under the Jira site's key, with the pane's
// project: none, or the repository's "owner/repo". A Jira key of that shape saved by such a build may
// hold GitHub's catalog, so it is not restored; one this build saved (kindKeyed) is Jira's.
bool IsAmbiguousLegacyJiraKey(const std::string& cacheKey) {
    if (cacheKey.rfind("Jira|", 0) != 0) {
        return false;
    }
    const std::string project = cacheKey.substr(cacheKey.rfind('|') + 1);
    return project.empty() || project.find('/') != std::string::npos;
}

// A snapshot under an ambiguous older-build Jira key that this build never saved: neither its index entry
// nor its blob carries kindKeyed. It is neither restored nor listed as a recent project.
bool IsUnclaimedLegacyJiraSnapshot(const nlohmann::json& root, const nlohmann::json& indexEntry,
                                   const std::string& cacheKey) {
    if (!IsAmbiguousLegacyJiraKey(cacheKey) || (indexEntry.is_object() && JsonBoolOr(indexEntry, "kindKeyed"))) {
        return false;
    }
    const auto blob = root.find(cacheKey);
    return blob == root.end() || !blob->is_object() || !JsonBoolOr(*blob, "kindKeyed");
}

// Whether a load of this blob would succeed: a damaged blob (no fields, a non-object) is not listed as a
// recent project, since picking it would only fail.
bool SnapshotBlobLoads(const nlohmann::json& root, const std::string& cacheKey) {
    const auto blob = root.find(cacheKey);
    if (blob == root.end() || !blob->is_object()) {
        return false;
    }
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    return ParseCatalogEntryObject(*blob, fields, components, issueTypeMeta, error);
}

} // namespace

bool TryLoadFieldCatalogSnapshot(const std::string& cacheKey, std::vector<TrackerField>& outFields,
                                 std::vector<TrackerComponent>& outComponents,
                                 std::vector<TrackerIssueTypeCreateMeta>& outIssueTypeMeta, std::string& outError,
                                 std::int64_t* outSavedUnix) {
    outError.clear();
    if (outSavedUnix != nullptr) {
        *outSavedUnix = 0;
    }
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
        nlohmann::json& indexArr = root["entries"]; // the migration always leaves an array here
        const auto indexIt = FindIndexEntry(indexArr, resolvedKey);
        if (resolvedKey == cacheKey &&
            IsUnclaimedLegacyJiraSnapshot(root, indexIt == indexArr.end() ? nlohmann::json() : *indexIt, cacheKey)) {
            outError = "The cached field catalog for this tracker context predates per-tracker keys.";
            return false;
        }
        const bool ok =
            ParseCatalogEntryObject(root[resolvedKey], outFields, outComponents, outIssueTypeMeta, outError);
        if (!ok) {
            return false;
        }

        // Record the read for the LRU order and report the save time, both from this one parse. The touch stays in
        // memory (PendingTouches) and reaches disk with the next save; only an entry unused for a whole interval
        // (or re-indexed with no time) is rewritten here, so the file is not rewritten on every load.
        if (indexIt != indexArr.end()) {
            const std::int64_t now = TimeNowPure::NowUnixSeconds();
            if (outSavedUnix != nullptr) {
                *outSavedUnix = IntOr(*indexIt, "savedUnix", 0); // 0 for an entry saved before the stamp existed
            }
            if (IntOr(*indexIt, "lastUsedUnix", 0) <= now - kLruTouchIntervalSeconds) {
                (*indexIt)["lastUsedUnix"] = now;
                ApplyPendingTouches(root, /*consume=*/true);
                std::string writeErr;
                if (!PersistRootLocked(root, writeErr)) {
                    // Best-effort: the caller still gets the loaded data; the cache is a snapshot, not the source
                    // of truth.
                    LOG_WARN("FieldCatalogCache::TryLoadFieldCatalogSnapshot: failed to touch LRU index: %s",
                             writeErr.c_str());
                }
            } else {
                PendingTouches()[resolvedKey] = now;
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

std::int64_t FieldCatalogSnapshotSavedUnix(const std::string& cacheKey) {
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        const nlohmann::json root = LoadAndMigrateRootLocked();
        if (!root.contains("entries") || !root["entries"].is_array()) {
            return 0;
        }
        for (const auto& e : root["entries"]) {
            if (e.is_object() && StrOr(e, "cacheKey") == cacheKey) {
                return IntOr(e, "savedUnix", 0); // 0 for an entry saved before the stamp existed
            }
        }
    } catch (const std::exception& ex) {
        LOG_WARN("FieldCatalogCache::FieldCatalogSnapshotSavedUnix failed: %s", ex.what());
    } catch (...) { // catch-all-ok: logged; an unknown age means the caller fetches live
        LOG_WARN("FieldCatalogCache::FieldCatalogSnapshotSavedUnix failed: unknown exception");
    }
    return 0;
}

std::vector<CachedProjectEntry> ListCachedProjects() {
    std::vector<CachedProjectEntry> out;
    try {
        std::lock_guard<std::mutex> lk(FieldCatalogCacheFileMutex());
        nlohmann::json root = LoadAndMigrateRootLocked();
        if (!root.contains("entries") || !root["entries"].is_array()) {
            return out;
        }
        ApplyPendingTouches(root, /*consume=*/false); // reads since the last write count for the order shown
        for (const auto& e : root["entries"]) {
            if (!e.is_object())
                continue;
            const std::string cacheKey = JsonStringFieldOr(e, "cacheKey");
            if (IsUnclaimedLegacyJiraSnapshot(root, e, cacheKey) || !SnapshotBlobLoads(root, cacheKey))
                continue;
            CachedProjectEntry cpe;
            cpe.projectKey = StrOr(e, "projectKey");
            cpe.backend = StrOr(e, "backend");
            cpe.endpoint = StrOr(e, "endpoint");
            cpe.lastUsedUnix = IntOr(e, "lastUsedUnix", 0);
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
            const bool matches = it->is_object() && StrOr(*it, "projectKey") == projectKey &&
                                 StrOr(*it, "backend") == backend && StrOr(*it, "endpoint") == endpoint;
            if (matches) {
                const std::string victimKey = StrOr(*it, "cacheKey");
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
        ApplyPendingTouches(root, /*consume=*/true);
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
