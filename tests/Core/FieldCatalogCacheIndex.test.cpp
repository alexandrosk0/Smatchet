#include <doctest/doctest.h>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "TimeNowPure.h"
#include "TrackerFieldSchema.h"

#include "../support/TestEnvGuard.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
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

std::string ReadCacheFile(const CacheFileCleanup& file) {
    std::ifstream in(file.Path(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
    // Entries saved within one second tie on lastUsedUnix: the save itself must survive the cap.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::string now = std::to_string(TimeNowPure::NowUnixSeconds() + 1);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-a\",\"projectKey\":\"A\",\"lastUsedUnix\":" +
                             now +
                             "},"
                             "{\"cacheKey\":\"k-b\",\"projectKey\":\"B\",\"lastUsedUnix\":" +
                             now + "}],\"k-a\":" + Blob("a") + ",\"k-b\":" + Blob("b") + "}");

    REQUIRE(Save("k-new", "NEW", 2));
    CHECK(Loads("k-new"));
    CHECK(Loads("k-a") != Loads("k-b"));
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

TEST_CASE("FieldCatalogCache: an index entry naming a reserved key is ignored") {
    // Evicting or forgetting such an entry would erase the index or the schema version itself.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"entries\",\"projectKey\":\"E\",\"lastUsedUnix\":0},"
                         "{\"cacheKey\":\"schema_version\",\"projectKey\":\"S\",\"lastUsedUnix\":0},"
                         "{\"cacheKey\":\"k-a\",\"projectKey\":\"A\",\"backend\":\"Jira\",\"endpoint\":\"e\","
                         "\"lastUsedUnix\":100}],"
                         "\"k-a\":" +
                             Blob("a") + "}");

    REQUIRE(Save("k-new", "NEW", 1));
    CHECK(Loads("k-new"));
    CHECK_FALSE(Loads("k-a"));
    CHECK(Listed("NEW"));
    CHECK_FALSE(Listed("E"));
    CHECK_FALSE(Listed("S"));
    CHECK(Save("k-next", "NEXT", 2)); // the index and the schema version survived
    CHECK(Loads("k-new"));
}

TEST_CASE("FieldCatalogCache: wrong-typed index fields do not break the cache") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-a\",\"projectKey\":5,\"backend\":[],\"endpoint\":{},"
                         "\"lastUsedUnix\":\"soon\"},"
                         "{\"cacheKey\":7,\"projectKey\":\"X\",\"lastUsedUnix\":1}],"
                         "\"k-a\":" +
                             Blob("a") + "}");

    CHECK(Loads("k-a"));
    CHECK(Save("k-b", "B", 16));
    CHECK(Listed("B"));
    CHECK(FieldCatalogCache::ForgetProject("B", "Jira", "https://acme.atlassian.net"));
    CHECK_FALSE(Listed("B"));
    CHECK(Loads("k-a"));
}

TEST_CASE("FieldCatalogCache: a restore moves its snapshot up the LRU order at most once a day") {
    // Each move rewrites the whole file, and a restore can run on the UI thread.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::string recent = std::to_string(TimeNowPure::NowUnixSeconds() - 60);
    const std::string json = "{\"schema_version\":3,\"entries\":["
                             "{\"cacheKey\":\"k-recent\",\"projectKey\":\"R\",\"lastUsedUnix\":" +
                             recent +
                             "},"
                             "{\"cacheKey\":\"k-old\",\"projectKey\":\"O\",\"lastUsedUnix\":100}],"
                             "\"k-recent\":" +
                             Blob("r") + ",\"k-old\":" + Blob("o") + "}";
    WriteCacheFile(file, json);

    REQUIRE(Loads("k-recent"));
    CHECK(ReadCacheFile(file) == json);
    REQUIRE(Loads("k-old"));
    CHECK(ReadCacheFile(file) != json);
}

TEST_CASE("FieldCatalogCache: an older build's Jira snapshot that may be GitHub's is not restored") {
    // Older builds saved a GitHub pane's catalog under the Jira site's key, with no project or the
    // repository's owner/repo: those shapes load only once this build has saved them.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":[],"
                         "\"Jira|https://acme.atlassian.net|\":" +
                             Blob("pr.head") + ",\"Jira|https://acme.atlassian.net|octo/repo\":" + Blob("pr.base") +
                             ",\"Jira|https://acme.atlassian.net|FOO\":" + Blob("customfield_foo") +
                             ",\"Plane|https://api.plane.so|ws|\":" + Blob("plane_state") + "}");

    CHECK_FALSE(Loads("Jira|https://acme.atlassian.net|"));
    CHECK_FALSE(Loads("Jira|https://acme.atlassian.net|octo/repo"));
    CHECK(Loads("Jira|https://acme.atlassian.net|FOO")); // a Jira project key: Jira's
    CHECK(Loads("Plane|https://api.plane.so|ws|"));      // another tracker's key was never shared

    REQUIRE(Save("Jira|https://acme.atlassian.net|", "", 16));
    CHECK(Loads("Jira|https://acme.atlassian.net|"));
}

TEST_CASE("FieldCatalogCache: a damaged index or schema version keeps the snapshots") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":\"three\",\"entries\":{\"k-a\":1},\"k-a\":" + Blob("a") + "}");
    CHECK(Loads("k-a"));
    REQUIRE(Save("k-b", "B", 16));
    CHECK(Loads("k-a"));
    CHECK(Loads("k-b"));
}

TEST_CASE("FieldCatalogCache: a lastUsedUnix ahead of the clock counts as used now") {
    // Left as it is, it would sort first for good and never be evicted.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-future\",\"projectKey\":\"F\",\"lastUsedUnix\":99999999999},"
                         "{\"cacheKey\":\"k-extreme\",\"projectKey\":\"X\","
                         "\"lastUsedUnix\":-9223372036854775808}],"
                         "\"k-future\":" +
                             Blob("f") + ",\"k-extreme\":" + Blob("x") + "}");
    CHECK(Loads("k-extreme")); // the once-a-day check must not overflow on it
    const std::int64_t now = TimeNowPure::NowUnixSeconds();
    for (const FieldCatalogCache::CachedProjectEntry& e : FieldCatalogCache::ListCachedProjects()) {
        CHECK(e.lastUsedUnix <= now + 1);
    }
}

TEST_CASE("FieldCatalogCache: an empty GitHub or Linear base URL keys and lists like its default") {
    // Preferences fills the default in and saves it, so the empty and the default URL are one site.
    TrackerConfig github;
    github.TrackerType = "GitHub";
    github.GitHubOwner = "octo";
    github.GitHubRepo = "repo";
    TrackerConfig githubDefault = github;
    githubDefault.GitHubBaseUrl = "https://api.github.com";
    CHECK(FieldCatalogCache::BuildFieldCatalogCacheKey(github, "") ==
          FieldCatalogCache::BuildFieldCatalogCacheKey(githubDefault, ""));
    CHECK(FieldCatalogCache::BuildFieldCatalogIndexIdentity(github).endpoint == "https://api.github.com|octo/repo");

    TrackerConfig linear;
    linear.TrackerType = "Linear";
    linear.LinearTeamId = "team";
    linear.LinearBaseUrl.clear();
    TrackerConfig linearDefault = linear;
    linearDefault.LinearBaseUrl = "https://api.linear.app/graphql";
    CHECK(FieldCatalogCache::BuildFieldCatalogCacheKey(linear, "") ==
          FieldCatalogCache::BuildFieldCatalogCacheKey(linearDefault, ""));
    CHECK(FieldCatalogCache::BuildFieldCatalogIndexIdentity(linear).endpoint == "https://api.linear.app/graphql|team");
}
