#include <doctest/doctest.h>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"

#include "../support/TestEnvGuard.h"

#include <cstdint>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// Regression coverage for the Fix C plumbing:
//   the Jira `components` field renders as plain text in the ticket grid because the
//   project-scoped catalog (which carries Phase-3 component options) was persisted under
//   the UNSCOPED ("") cache key and lost. The fix saves + reloads the catalog under the
//   project-scoped key. These tests pin that the scoped key is the carrier of the options
//   and that the unscoped key does NOT see them — i.e. scope actually matters.

namespace {

TrackerField MakeComponentsField(bool withOptions) {
    TrackerField f;
    f.Id = "components";
    f.Name = "Components";
    f.IsArray = true; // Jira components schema is type=array, items=component
    f.ItemsType = "component";
    f.Family = TrackerFieldFamily::SelectMulti; // classified opts-independently
    if (withOptions) {
        TrackerFieldOption opt;
        opt.Id = "10001";
        opt.Value = "Backend";
        f.AllowedValueOptions.push_back(opt);
        TrackerFieldOption opt2;
        opt2.Id = "10002";
        opt2.Value = "Frontend";
        f.AllowedValueOptions.push_back(opt2);
    }
    return f;
}

const TrackerField* FindField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const auto& f : fields) {
        if (f.Id == id) {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("FieldCatalogCache: scoped components catalog round-trips under the project key") {
    smatchet_tests::TestEnvGuard env;

    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://example.atlassian.net";

    const std::string projectKey = "BLOOP";
    const std::string scopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, projectKey);
    const std::string unscopedKey = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, std::string());
    REQUIRE(scopedKey != unscopedKey);

    std::vector<TrackerField> fields;
    fields.push_back(MakeComponentsField(/*withOptions=*/true));

    std::string saveErr;
    REQUIRE(FieldCatalogCache::SaveFieldCatalogSnapshot(scopedKey, "Jira", cfg.Domain, projectKey,
                                                        /*maxProjects=*/16, fields, {}, {}, saveErr));
    CHECK(saveErr.empty());

    SUBCASE("loading under the scoped key preserves component options") {
        std::vector<TrackerField> outFields;
        std::vector<TrackerComponent> outComponents;
        std::vector<TrackerIssueTypeCreateMeta> outMeta;
        std::string loadErr;
        REQUIRE(FieldCatalogCache::TryLoadFieldCatalogSnapshot(scopedKey, outFields, outComponents, outMeta, loadErr));
        const TrackerField* comp = FindField(outFields, "components");
        REQUIRE(comp != nullptr);
        CHECK(comp->IsArray);
        CHECK(comp->ItemsType == "component");
        CHECK(comp->Family == TrackerFieldFamily::SelectMulti);
        REQUIRE(comp->AllowedValueOptions.size() == 2);
        CHECK(comp->AllowedValueOptions[0].Value == "Backend");
        CHECK(comp->AllowedValueOptions[1].Value == "Frontend");
    }

    SUBCASE("loading under the unscoped key does NOT see the scoped options (this was the bug)") {
        std::vector<TrackerField> outFields;
        std::vector<TrackerComponent> outComponents;
        std::vector<TrackerIssueTypeCreateMeta> outMeta;
        std::string loadErr;
        // No snapshot was ever saved under the unscoped key, so the load must miss — proving the
        // startup unscoped load (pre-fix) could never surface the component options.
        CHECK_FALSE(
            FieldCatalogCache::TryLoadFieldCatalogSnapshot(unscopedKey, outFields, outComponents, outMeta, loadErr));
    }
}

TEST_CASE("FieldCatalogCache: the save time is stored on save and a read does not move it") {
    smatchet_tests::TestEnvGuard env;
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://example.atlassian.net";
    const std::string key = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, "SAVED");
    CHECK(FieldCatalogCache::FieldCatalogSnapshotSavedUnix(key) == 0); // nothing saved yet

    std::vector<TrackerField> fields;
    fields.push_back(MakeComponentsField(/*withOptions=*/false));
    std::string err;
    REQUIRE(FieldCatalogCache::SaveFieldCatalogSnapshot(key, "Jira", cfg.Domain, "SAVED", 16, fields, {}, {}, err));
    const std::int64_t savedAt = FieldCatalogCache::FieldCatalogSnapshotSavedUnix(key);
    CHECK(savedAt > 0);

    std::vector<TrackerField> outFields;
    std::vector<TrackerComponent> outComponents;
    std::vector<TrackerIssueTypeCreateMeta> outMeta;
    REQUIRE(FieldCatalogCache::TryLoadFieldCatalogSnapshot(key, outFields, outComponents, outMeta, err));
    CHECK(FieldCatalogCache::FieldCatalogSnapshotSavedUnix(key) == savedAt); // reading is not refreshing
}

namespace {
std::string ReadWholeFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
} // namespace

TEST_CASE("FieldCatalogCache: a load reports the save time and never writes the cache file") {
    smatchet_tests::TestEnvGuard env;
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://example.atlassian.net";
    const std::string key = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, "NOWRITE");
    std::vector<TrackerField> fields;
    fields.push_back(MakeComponentsField(/*withOptions=*/false));
    std::string err;
    REQUIRE(FieldCatalogCache::SaveFieldCatalogSnapshot(key, "Jira", cfg.Domain, "NOWRITE", 16, fields, {}, {}, err));
    const std::string path = ConfigManager::GetUserDataDirectory() + "smatchet_field_catalog_cache.json";
    const std::string before = ReadWholeFile(path);
    REQUIRE_FALSE(before.empty());

    std::vector<TrackerField> outFields;
    std::vector<TrackerComponent> outComponents;
    std::vector<TrackerIssueTypeCreateMeta> outMeta;
    std::int64_t savedUnix = -1;
    for (int i = 0; i < 3; ++i) {
        REQUIRE(
            FieldCatalogCache::TryLoadFieldCatalogSnapshot(key, outFields, outComponents, outMeta, err, &savedUnix));
    }

    CHECK(savedUnix == FieldCatalogCache::FieldCatalogSnapshotSavedUnix(key));
    CHECK(savedUnix > 0);
    CHECK(ReadWholeFile(path) == before); // byte-identical: the LRU touch stays in memory
    // The touch is not lost: the next save carries it and the listing already shows it.
    CHECK(FieldCatalogCache::ListCachedProjects().size() == 1);
}

TEST_CASE("FieldCatalogCache: one malformed index entry is skipped, not the whole file") {
    smatchet_tests::TestEnvGuard env;
    TrackerConfig cfg;
    cfg.TrackerType = "Jira";
    cfg.Domain = "https://example.atlassian.net";
    const std::string good = FieldCatalogCache::BuildFieldCatalogCacheKey(cfg, "GOOD");
    std::vector<TrackerField> fields;
    fields.push_back(MakeComponentsField(/*withOptions=*/false));
    std::string err;
    REQUIRE(FieldCatalogCache::SaveFieldCatalogSnapshot(good, "Jira", cfg.Domain, "GOOD", 16, fields, {}, {}, err));

    // Damage a sibling index entry: wrong types for the timestamps and the key.
    const std::string path = ConfigManager::GetUserDataDirectory() + "smatchet_field_catalog_cache.json";
    nlohmann::json root = nlohmann::json::parse(ReadWholeFile(path));
    nlohmann::json bad = nlohmann::json::object();
    bad["cacheKey"] = 7;
    bad["projectKey"] = "BAD";
    bad["lastUsedUnix"] = "yesterday";
    bad["savedUnix"] = nlohmann::json::array();
    root["entries"].push_back(bad);
    nlohmann::json badTimes = nlohmann::json::object();
    badTimes["cacheKey"] = "Jira|other|X";
    badTimes["lastUsedUnix"] = "soon";
    root["entries"].push_back(badTimes);
    root["Jira|other|X"] = root[good]; // an index entry that names no snapshot is dropped, so give it one
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << root.dump();
    }

    std::vector<TrackerField> outFields;
    std::vector<TrackerComponent> outComponents;
    std::vector<TrackerIssueTypeCreateMeta> outMeta;
    std::int64_t savedUnix = 0;
    CHECK(FieldCatalogCache::TryLoadFieldCatalogSnapshot(good, outFields, outComponents, outMeta, err, &savedUnix));
    CHECK(savedUnix > 0);
    CHECK(FieldCatalogCache::FieldCatalogSnapshotSavedUnix(good) == savedUnix);
    CHECK(FieldCatalogCache::ListCachedProjects().size() >= 2); // the good entry and the readable-enough ones
}
