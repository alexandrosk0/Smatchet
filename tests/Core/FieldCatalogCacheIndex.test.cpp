#include <doctest/doctest.h>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "TrackerFieldSchema.h"

#include "../support/TestEnvGuard.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// The snapshot cache's `entries` index: Preferences' "Recently used projects" list and its Forget button
// read it, and the LRU cap evicts through it. It must survive a reload.

namespace {

const char* const kCacheFileName = "smatchet_field_catalog_cache.json";

/// Removes the cache file before the TestEnvGuard (declared first) removes its directory.
class CacheFileCleanup {
  public:
    explicit CacheFileCleanup(const smatchet_tests::TestEnvGuard& env) : path_(env.DirWithSep() + kCacheFileName) {}
    ~CacheFileCleanup() { std::remove(path_.c_str()); }
    CacheFileCleanup(const CacheFileCleanup&) = delete;
    CacheFileCleanup& operator=(const CacheFileCleanup&) = delete;

    const std::string& Path() const { return path_; }

  private:
    std::string path_;
};

std::vector<TrackerField> OneField(const std::string& id) {
    TrackerField f;
    f.Id = id;
    f.Name = "Field " + id;
    return std::vector<TrackerField>(1, f);
}

bool Save(const std::string& cacheKey, const std::string& project, int maxProjects) {
    std::string err;
    return FieldCatalogCache::SaveFieldCatalogSnapshot(cacheKey, "Jira", "https://acme.atlassian.net", project,
                                                       maxProjects, OneField("customfield_" + project), {}, {}, err);
}

bool Loads(const std::string& cacheKey) {
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> meta;
    std::string err;
    return FieldCatalogCache::TryLoadFieldCatalogSnapshot(cacheKey, fields, components, meta, err);
}

bool Listed(const std::string& project) {
    for (const FieldCatalogCache::CachedProjectEntry& e : FieldCatalogCache::ListCachedProjects()) {
        if (e.projectKey == project) {
            return true;
        }
    }
    return false;
}

std::string Blob(const std::string& id) { return "{\"fields\":[{\"id\":\"" + id + "\",\"name\":\"x\"}]}"; }

void WriteCacheFile(const CacheFileCleanup& file, const std::string& json) {
    std::ofstream out(file.Path(), std::ios::binary | std::ios::trunc);
    out << json;
}

} // namespace

TEST_CASE("FieldCatalogCache: the index survives a reload, so every saved project is listed") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    REQUIRE(Save("Jira|https://acme.atlassian.net|AAA", "AAA", 16));
    REQUIRE(Save("Jira|https://acme.atlassian.net|BBB", "BBB", 16));

    CHECK(Listed("AAA"));
    CHECK(Listed("BBB"));
    const std::vector<FieldCatalogCache::CachedProjectEntry> entries = FieldCatalogCache::ListCachedProjects();
    REQUIRE(entries.size() == 2u);
    CHECK(entries[0].backend == "Jira");
    CHECK(entries[0].endpoint == "https://acme.atlassian.net");

    CHECK(FieldCatalogCache::ForgetProject("AAA", "Jira", "https://acme.atlassian.net"));
    CHECK_FALSE(Listed("AAA"));
    CHECK_FALSE(Loads("Jira|https://acme.atlassian.net|AAA"));
    CHECK(Loads("Jira|https://acme.atlassian.net|BBB"));
}

TEST_CASE("FieldCatalogCache: the LRU cap evicts the least recently used snapshot") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-old\",\"projectKey\":\"OLD\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":100},"
                         "{\"cacheKey\":\"k-mid\",\"projectKey\":\"MID\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":200}],"
                         "\"k-old\":" +
                             Blob("old") + ",\"k-mid\":" + Blob("mid") + "}");

    REQUIRE(Save("k-new", "NEW", 2));
    CHECK_FALSE(Loads("k-old"));
    CHECK(Loads("k-mid"));
    CHECK(Loads("k-new"));
}

TEST_CASE("FieldCatalogCache: a snapshot just saved is never the one evicted") {
    // Entries saved within one second tie on lastUsedUnix (and a clock step can put the others ahead):
    // the save itself must survive the cap.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-a\",\"projectKey\":\"A\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":4102444800},"
                         "{\"cacheKey\":\"k-b\",\"projectKey\":\"B\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":4102444801}],"
                         "\"k-a\":" +
                             Blob("a") + ",\"k-b\":" + Blob("b") + "}");

    REQUIRE(Save("k-new", "NEW", 2));
    CHECK(Loads("k-new"));
    CHECK(Loads("k-b"));
    CHECK_FALSE(Loads("k-a"));
}

TEST_CASE("FieldCatalogCache: a snapshot the index lost is indexed and evicted first") {
    // Earlier builds dropped the index on every load, so their snapshots were never evicted.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-kept\",\"projectKey\":\"KEPT\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":200}],"
                         "\"k-kept\":" +
                             Blob("kept") + ",\"k-lost\":" + Blob("lost") + "}");

    REQUIRE(Save("k-new", "NEW", 2));
    CHECK_FALSE(Loads("k-lost"));
    CHECK(Loads("k-kept"));
    CHECK(Loads("k-new"));
}
