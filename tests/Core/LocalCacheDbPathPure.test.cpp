// Pure-logic coverage for the local cache location planner (Persistence/LocalCacheDbPathPure.h):
// absolute passthrough, relative/empty resolution against the user-data directory, the legacy candidate
// list, and the lexical path comparison.

#include "Persistence/LocalCacheDbPathPure.h"

#include "SmatchetDefaults.h"

#include <doctest/doctest.h>

using smatchet::persistence::LocalCacheDbPathInputs;
using smatchet::persistence::LocalCacheDbPathPlan;
using smatchet::persistence::PlanLocalCacheDbPath;
using smatchet::persistence::SameLocalPath;

namespace {

#if defined(_WIN32)
const char* const kUdd = "C:/Users/u/AppData/Roaming/Smatchet";
const char* const kCwd = "C:/work/proj";
const char* const kAsset = "C:/Program Files/Smatchet";
const char* const kShared = "C:/ProgramData/Smatchet";
#else
const char* const kUdd = "/home/u/.local/share/Smatchet";
const char* const kCwd = "/home/u/work/proj";
const char* const kAsset = "/opt/smatchet";
const char* const kShared = "/var/lib/smatchet";
#endif

LocalCacheDbPathInputs Inputs(const std::string& configured) {
    LocalCacheDbPathInputs in;
    in.Configured = configured;
    in.UserDataDir = kUdd;
    in.Cwd = kCwd;
    in.RuntimeAssetDir = kAsset;
    in.SharedUserDataDir = kShared;
    return in;
}

std::string Join(const char* dir, const std::string& name) { return std::string(dir) + "/" + name; }

} // namespace

TEST_CASE("PlanLocalCacheDbPath: an absolute value passes through with no candidates") {
    const std::string abs = Join(kShared, "my.sqlite");
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(Inputs(abs));
    CHECK(SameLocalPath(plan.Target, abs));
    CHECK(plan.LegacyCandidates.empty());
    CHECK_FALSE(plan.BaseFellBackToCwd);
}

TEST_CASE("PlanLocalCacheDbPath: empty resolves the default name under the user-data directory") {
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(Inputs(""));
    CHECK(SameLocalPath(plan.Target, Join(kUdd, SmatchetDefaults::kDefaultDbPath)));
    REQUIRE(plan.LegacyCandidates.size() == 3);
    CHECK(SameLocalPath(plan.LegacyCandidates[0], Join(kCwd, SmatchetDefaults::kDefaultDbPath)));
    CHECK(SameLocalPath(plan.LegacyCandidates[1], Join(kAsset, SmatchetDefaults::kDefaultDbPath)));
    CHECK(SameLocalPath(plan.LegacyCandidates[2], Join(kShared, SmatchetDefaults::kDefaultDbPath)));
    CHECK_FALSE(plan.BaseFellBackToCwd);
}

TEST_CASE("PlanLocalCacheDbPath: a relative value keeps its sub-path under the user-data directory") {
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(Inputs("sub/cache.sqlite"));
    CHECK(SameLocalPath(plan.Target, Join(kUdd, "sub/cache.sqlite")));
    REQUIRE(plan.LegacyCandidates.size() == 3);
    CHECK(SameLocalPath(plan.LegacyCandidates[0], Join(kCwd, "sub/cache.sqlite")));
}

TEST_CASE("PlanLocalCacheDbPath: a candidate equal to the target is dropped") {
    LocalCacheDbPathInputs in = Inputs("");
    in.SharedUserDataDir = kUdd;
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(in);
    CHECK(plan.LegacyCandidates.size() == 2);
    for (std::size_t i = 0; i < plan.LegacyCandidates.size(); ++i) {
        CHECK_FALSE(SameLocalPath(plan.LegacyCandidates[i], plan.Target));
    }
}

TEST_CASE("PlanLocalCacheDbPath: duplicate and empty bases yield each candidate once") {
    LocalCacheDbPathInputs in = Inputs("");
    in.RuntimeAssetDir = kCwd;
    in.SharedUserDataDir.clear();
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(in);
    REQUIRE(plan.LegacyCandidates.size() == 1);
    CHECK(SameLocalPath(plan.LegacyCandidates[0], Join(kCwd, SmatchetDefaults::kDefaultDbPath)));
}

TEST_CASE("PlanLocalCacheDbPath: AllowLegacyImport=false lists no candidates") {
    LocalCacheDbPathInputs in = Inputs("");
    in.AllowLegacyImport = false;
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(in);
    CHECK(SameLocalPath(plan.Target, Join(kUdd, SmatchetDefaults::kDefaultDbPath)));
    CHECK(plan.LegacyCandidates.empty());
}

TEST_CASE("PlanLocalCacheDbPath: no user-data directory falls back to the working directory") {
    LocalCacheDbPathInputs in = Inputs("");
    in.UserDataDir.clear();
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(in);
    CHECK(plan.BaseFellBackToCwd);
    CHECK(SameLocalPath(plan.Target, Join(kCwd, SmatchetDefaults::kDefaultDbPath)));
    // The working directory is now the target, so it is no longer a legacy candidate.
    for (std::size_t i = 0; i < plan.LegacyCandidates.size(); ++i) {
        CHECK_FALSE(SameLocalPath(plan.LegacyCandidates[i], plan.Target));
    }
}

TEST_CASE("PlanLocalCacheDbPath: with no base at all the configured value is kept untouched") {
    LocalCacheDbPathInputs in;
    in.Configured = "rel.sqlite";
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(in);
    CHECK(plan.Target == "rel.sqlite");
    CHECK(plan.LegacyCandidates.empty());

    LocalCacheDbPathInputs empty;
    CHECK(PlanLocalCacheDbPath(empty).Target == SmatchetDefaults::kDefaultDbPath);
}

TEST_CASE("PlanLocalCacheDbPath: dot segments are normalized") {
    const LocalCacheDbPathPlan plan = PlanLocalCacheDbPath(Inputs("a/../b/./c.sqlite"));
    CHECK(SameLocalPath(plan.Target, Join(kUdd, "b/c.sqlite")));
    CHECK(plan.Target.find("..") == std::string::npos);
}

TEST_CASE("SameLocalPath: compares lexically") {
    CHECK(SameLocalPath("/a/b/../c/x.db", "/a/c/x.db"));
    CHECK(SameLocalPath("/a/./b/x.db", "/a/b/x.db"));
    CHECK_FALSE(SameLocalPath("/a/b/x.db", "/a/b/y.db"));
#if defined(_WIN32)
    CHECK(SameLocalPath("C:/Data/X.db", "c:/data/x.db"));
#else
    CHECK_FALSE(SameLocalPath("/a/B/x.db", "/a/b/x.db"));
#endif
}
