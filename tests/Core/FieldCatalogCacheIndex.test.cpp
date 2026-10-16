#include <doctest/doctest.h>

#include "ConfigManager.h"
#include "FieldCatalogCache.h"
#include "TimeNowPure.h"
#include "TrackerFieldSchema.h"

#include "../support/TestEnvGuard.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <direct.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

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

TEST_CASE("FieldCatalogCache: a damaged index keeps this build's own Jira snapshots restorable") {
    // Only an older build's snapshot under an ambiguous Jira key is refused. The flag that tells this
    // build's apart rides in the blob as well as in the index, so losing the index does not lose it.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    REQUIRE(Save("Jira|https://acme.atlassian.net|", "", 16));
    REQUIRE(Save("Jira|https://acme.atlassian.net|octo/repo", "octo/repo", 16));

    std::string json = ReadCacheFile(file);
    const std::string entries = "\"entries\":[";
    const auto at = json.find(entries);
    REQUIRE(at != std::string::npos);
    const auto end = json.find(']', at);
    REQUIRE(end != std::string::npos);
    json.replace(at, end + 1 - at, "\"entries\":{}");
    WriteCacheFile(file, json);

    CHECK(Loads("Jira|https://acme.atlassian.net|"));
    CHECK(Loads("Jira|https://acme.atlassian.net|octo/repo"));
}

TEST_CASE("FieldCatalogCache: an older build's ambiguous Jira snapshot is not listed as a recent project") {
    // The load refuses it, so the pill and the picker must not offer it either: a pick would write
    // `project = octo/repo` into a Jira query.
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"Jira|https://acme.atlassian.net|octo/repo\",\"projectKey\":\"octo/repo\","
                         "\"backend\":\"Jira\",\"endpoint\":\"https://acme.atlassian.net\",\"lastUsedUnix\":5},"
                         "{\"cacheKey\":\"Jira|https://acme.atlassian.net|FOO\",\"projectKey\":\"FOO\","
                         "\"backend\":\"Jira\",\"endpoint\":\"https://acme.atlassian.net\",\"lastUsedUnix\":4}],"
                         "\"Jira|https://acme.atlassian.net|octo/repo\":" +
                             Blob("pr.base") + ",\"Jira|https://acme.atlassian.net|FOO\":" + Blob("customfield_foo") +
                             "}");
    CHECK(Listed("FOO"));
    CHECK_FALSE(Listed("octo/repo"));

    REQUIRE(Save("Jira|https://acme.atlassian.net|octo/repo", "octo/repo", 16)); // this build's own: listed
    CHECK(Listed("octo/repo"));
}

TEST_CASE("FieldCatalogCache: a snapshot that cannot load is not listed as a recent project") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-empty\",\"projectKey\":\"EMPTY\",\"backend\":\"Jira\","
                         "\"endpoint\":\"https://acme.atlassian.net\",\"lastUsedUnix\":5},"
                         "{\"cacheKey\":\"k-good\",\"projectKey\":\"GOOD\",\"backend\":\"Jira\","
                         "\"endpoint\":\"https://acme.atlassian.net\",\"lastUsedUnix\":4}],"
                         "\"k-empty\":{\"fields\":[]},\"k-good\":" +
                             Blob("customfield_good") + "}");
    CHECK_FALSE(Loads("k-empty"));
    CHECK_FALSE(Listed("EMPTY"));
    CHECK(Loads("k-good"));
    CHECK(Listed("GOOD"));
}

TEST_CASE("FieldCatalogCache: a damaged schema version 2 with an object index keeps the snapshots at root") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, "{\"schema_version\":2,\"entries\":{\"k-b\":" + Blob("b") + "},\"k-a\":" + Blob("a") + "}");
    CHECK(Loads("k-a"));
    CHECK(Loads("k-b"));
    REQUIRE(Save("k-c", "C", 16));
    CHECK(Loads("k-a"));
}

TEST_CASE("FieldCatalogCache: either copy of this build's flag keeps its Jira snapshot restorable") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::string key = "Jira|https://acme.atlassian.net|octo/repo";
    const std::string flagged = "\"kindKeyed\":true";
    const std::string cleared = "\"kindKeyed\":false";
    // Clears the flag in the index only (index = true) or in the blob only (index = false).
    const auto clearFlag = [&](bool index) {
        std::string json = ReadCacheFile(file);
        const auto entriesAt = json.find("\"entries\":[");
        REQUIRE(entriesAt != std::string::npos);
        const auto entriesEnd = json.find(']', entriesAt);
        const auto at = index ? json.find(flagged, entriesAt) : json.find(flagged);
        REQUIRE(at != std::string::npos);
        REQUIRE((at < entriesEnd && at > entriesAt) == index);
        json.replace(at, flagged.size(), cleared);
        WriteCacheFile(file, json);
    };

    REQUIRE(Save(key, "octo/repo", 16));
    clearFlag(/*index=*/true); // the index lost it, the blob still has it
    CHECK(Loads(key));

    REQUIRE(Save(key, "octo/repo", 16));
    std::string json = ReadCacheFile(file);
    const auto entriesAt = json.find("\"entries\":[");
    json.replace(entriesAt, json.find(']', entriesAt) + 1 - entriesAt, "\"entries\":{}");
    WriteCacheFile(file, json); // the index is lost: the first load re-indexes the blob with its flag
    CHECK(Loads(key));
    clearFlag(/*index=*/false); // then the blob loses it, the rebuilt index still has it
    CHECK(Loads(key));

    json = ReadCacheFile(file);
    for (auto at = json.find(flagged); at != std::string::npos; at = json.find(flagged)) {
        json.replace(at, flagged.size(), cleared);
    }
    WriteCacheFile(file, json); // both copies gone: what an older build rewriting the file leaves
    CHECK_FALSE(Loads(key));
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

namespace {

/// Makes every write of the cache file fail (a directory sits where the temp file goes) until released.
class WriteBlocker {
  public:
    explicit WriteBlocker(const CacheFileCleanup& file) : dir_(file.Path() + ".tmp") {
#if defined(_WIN32)
        REQUIRE(::_mkdir(dir_.c_str()) == 0);
#else
        REQUIRE(::mkdir(dir_.c_str(), 0755) == 0);
#endif
    }
    ~WriteBlocker() { Release(); }
    WriteBlocker(const WriteBlocker&) = delete;
    WriteBlocker& operator=(const WriteBlocker&) = delete;
    void Release() {
        if (released_) {
            return;
        }
        released_ = true;
#if defined(_WIN32)
        ::_rmdir(dir_.c_str());
#else
        ::rmdir(dir_.c_str());
#endif
    }

  private:
    std::string dir_;
    bool released_ = false;
};

/// The lastUsedUnix the cache FILE holds for `project` (-1 when absent). Unlike ListCachedProjects() this ignores
/// touches still queued in memory, so it proves what actually reached the disk.
std::int64_t PersistedLastUsedOf(const CacheFileCleanup& file, const std::string& project) {
    const nlohmann::json doc = nlohmann::json::parse(ReadCacheFile(file), nullptr, /*allow_exceptions=*/false);
    if (!doc.is_object() || !doc.contains("entries") || !doc["entries"].is_array()) {
        return -1;
    }
    for (const nlohmann::json& e : doc["entries"]) {
        if (e.value("projectKey", std::string()) == project) {
            return e.value("lastUsedUnix", static_cast<std::int64_t>(-1));
        }
    }
    return -1;
}

} // namespace

TEST_CASE("FieldCatalogCache: a failed touch write keeps the queued touches for the next write") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::int64_t recentUnix = TimeNowPure::NowUnixSeconds() - 60;
    const std::string recent = std::to_string(recentUnix);
    WriteCacheFile(file, "{\"schema_version\":3,\"entries\":["
                         "{\"cacheKey\":\"k-recent\",\"projectKey\":\"R\",\"lastUsedUnix\":" +
                             recent +
                             "},"
                             "{\"cacheKey\":\"k-old\",\"projectKey\":\"O\",\"lastUsedUnix\":100}],"
                             "\"k-recent\":" +
                             Blob("r") + ",\"k-old\":" + Blob("o") + "}");

    REQUIRE(Loads("k-recent")); // used within the day: the touch waits in memory

    {
        // A directory where the temp file goes makes the atomic write fail on every platform.
        const WriteBlocker blocker(file);
        CHECK(Loads("k-old")); // stale: tries to write, fails, still loads
        const auto touchedAfterFailure = [&]() {
            for (const FieldCatalogCache::CachedProjectEntry& e : FieldCatalogCache::ListCachedProjects()) {
                if (e.projectKey == "R") {
                    return e.lastUsedUnix > recentUnix;
                }
            }
            return false;
        };
        CHECK(touchedAfterFailure()); // the queued touch survived the failed write
    }

    REQUIRE(Loads("k-old"));                            // the write works now and carries the touch to disk
    CHECK(PersistedLastUsedOf(file, "R") > recentUnix); // the file itself now holds the touch the failed write kept
}

namespace {

/// A cache holding a fresh entry (touched a minute ago: its load only queues a touch) and a stale one.
std::string TwoEntryCache(const std::string& freshUnix) {
    return "{\"schema_version\":3,\"entries\":["
           "{\"cacheKey\":\"k-fresh\",\"projectKey\":\"R\",\"backend\":\"Jira\",\"endpoint\":\"https://"
           "acme.atlassian.net\","
           "\"lastUsedUnix\":" +
           freshUnix +
           "},"
           "{\"cacheKey\":\"k-stale\",\"projectKey\":\"O\",\"backend\":\"Jira\",\"endpoint\":\"https://"
           "acme.atlassian.net\","
           "\"lastUsedUnix\":100}],"
           "\"k-fresh\":" +
           Blob("r") + ",\"k-stale\":" + Blob("o") + "}";
}

} // namespace

TEST_CASE("FieldCatalogCache: a failed save keeps the queued touches for the next write") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::int64_t freshUnix = TimeNowPure::NowUnixSeconds() - 60;
    WriteCacheFile(file, TwoEntryCache(std::to_string(freshUnix)));
    REQUIRE(Loads("k-fresh")); // used within the day: the touch waits in memory

    {
        const WriteBlocker blocker(file);
        CHECK_FALSE(Save("k-new", "NEW", 16));
    }
    REQUIRE(Save("k-other", "OTHER", 16));             // the write works now and carries the touch to disk
    CHECK(PersistedLastUsedOf(file, "R") > freshUnix); // read from the file: pending in-memory touches don't count
}

TEST_CASE("FieldCatalogCache: a failed forget keeps the queued touches for the next write") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    const std::int64_t freshUnix = TimeNowPure::NowUnixSeconds() - 60;
    WriteCacheFile(file, TwoEntryCache(std::to_string(freshUnix)));
    REQUIRE(Loads("k-fresh"));

    {
        const WriteBlocker blocker(file);
        CHECK_FALSE(FieldCatalogCache::ForgetProject("O", "Jira", "https://acme.atlassian.net"));
    }
    REQUIRE(FieldCatalogCache::ForgetProject("O", "Jira", "https://acme.atlassian.net"));
    CHECK(PersistedLastUsedOf(file, "R") > freshUnix);
}

TEST_CASE("FieldCatalogCache: a load whose touch write fails keeps its own touch queued") {
    smatchet_tests::TestEnvGuard env;
    CacheFileCleanup file(env);
    WriteCacheFile(file, TwoEntryCache(std::to_string(TimeNowPure::NowUnixSeconds() - 60)));

    {
        const WriteBlocker blocker(file);
        CHECK(Loads("k-stale")); // stale: tries to write, fails, still loads
    }
    REQUIRE(Save("k-other", "OTHER", 16));
    CHECK(PersistedLastUsedOf(file, "O") > 100); // this load's own touch rode along with the next write
}

TEST_CASE("FieldCatalogCache: a touch queued against one data directory never folds into another directory's file") {
    const std::int64_t freshUnix = TimeNowPure::NowUnixSeconds() - 60;
    {
        smatchet_tests::TestEnvGuard firstEnv;
        CacheFileCleanup firstFile(firstEnv);
        WriteCacheFile(firstFile, TwoEntryCache(std::to_string(freshUnix)));
        REQUIRE(Loads("k-fresh")); // used within the day: the touch waits in memory, keyed to this directory's file
    }
    smatchet_tests::TestEnvGuard secondEnv;
    CacheFileCleanup secondFile(secondEnv);
    WriteCacheFile(secondFile, TwoEntryCache(std::to_string(freshUnix)));

    REQUIRE(Save("k-other", "OTHER", 16));                    // a write into the second directory's file
    CHECK(PersistedLastUsedOf(secondFile, "R") == freshUnix); // the first directory's touch did not ride along
}
