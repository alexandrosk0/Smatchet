// ProjectListLookup — the project picker's "All projects" read keeps an offline copy (Quality Pillar 6):
// a live listing is saved, offline no request is sent and the saved list is served, and a failed listing
// falls back to the saved list with its error kind intact. Fakes only.

#include "ProjectListLookup.h"

#include "FakeLookupCache.h"
#include "ITrackerConnectivity.h"
#include "LookupPayloadsPure.h"

#include <doctest/doctest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using smatchet::projects::LoadProjectList;
using smatchet::projects::ProjectListOutcome;

namespace {

RemoteProject Project(const std::string& key, const std::string& name) {
    RemoteProject p;
    p.id = "id-" + key;
    p.key = key;
    p.displayName = name;
    return p;
}

// Connectivity whose ListProjectsTyped is scripted; counts every listing.
class ScriptedConnectivity : public ITrackerConnectivity {
  public:
    std::string GetTrackerType() const override { return "Jira"; }
    TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& /*cfg*/) override {
        return TrackerReachabilityProbeResult{TrackerReachabilityProbeKind::AuthenticatedReachable, ""};
    }
    ProjectListResult ListProjectsTyped() override {
        ++Calls;
        if (Throws) {
            throw std::runtime_error("listing exploded");
        }
        if (!Error.IsOk()) {
            return ProjectListResult::Err(Error);
        }
        return ProjectListResult::Ok(Projects);
    }

    std::vector<RemoteProject> Projects;
    TrackerError Error;
    bool Throws = false;
    int Calls = 0;
};

class ThrowingReadLookupCache : public FakeLookupCache {
  public:
    bool TryGetLookup(const std::string& /*backendKey*/, const std::string& /*kind*/, const std::string& /*cacheKey*/,
                      LookupCacheRow& /*out*/) override {
        throw std::runtime_error("disk read failed");
    }
};

class ThrowingWriteLookupCache : public FakeLookupCache {
  public:
    bool UpsertLookup(const std::string& /*backendKey*/, const std::string& /*kind*/, const std::string& /*cacheKey*/,
                      const std::string& /*payloadJson*/) override {
        throw std::runtime_error("disk full");
    }
};

void SaveList(FakeLookupCache& store, const std::string& backendKey, const std::vector<RemoteProject>& projects) {
    store.UpsertLookup(backendKey, smatchet::lookup::kProjectsKind, smatchet::lookup::kProjectsKey,
                       smatchet::lookup::SerializeProjects(projects));
}

} // namespace

TEST_SUITE("ProjectListLookup") {

    TEST_CASE("a live listing is returned and saved for the next offline session") {
        ScriptedConnectivity conn;
        conn.Projects = {Project("OFF", "Offline First"), Project("SIDE", "Side")};
        auto store = std::make_shared<FakeLookupCache>();

        const ProjectListOutcome live = LoadProjectList(conn, store, "Jira", /*offline=*/false);
        CHECK_FALSE(live.Failed);
        CHECK_FALSE(live.FromSaved);
        REQUIRE(live.Projects.size() == 2u);
        CHECK(store->UpsertCalls.load() == 1);

        // The next session starts offline: no request, the saved list comes back.
        ScriptedConnectivity nextSession;
        const ProjectListOutcome offline = LoadProjectList(nextSession, store, "Jira", /*offline=*/true);
        CHECK(nextSession.Calls == 0);
        CHECK(offline.Failed);
        CHECK(offline.FromSaved);
        CHECK(offline.Error.Kind == TrackerErrorKind::Transport);
        REQUIRE(offline.Projects.size() == 2u);
        CHECK(offline.Projects[0].key == "OFF");
        CHECK(offline.Projects[1].displayName == "Side");
    }

    TEST_CASE("a failed listing falls back to the saved list and keeps its error kind") {
        auto store = std::make_shared<FakeLookupCache>();
        SaveList(*store, "Jira", {Project("OFF", "Offline First")});
        ScriptedConnectivity conn;
        conn.Error = TrackerErrorTransport("HTTP 0 (Couldn't connect)");

        const ProjectListOutcome outcome = LoadProjectList(conn, store, "Jira", /*offline=*/false);
        CHECK(conn.Calls == 1);
        CHECK(outcome.Failed);
        CHECK(outcome.FromSaved);
        CHECK(outcome.Error.Kind == TrackerErrorKind::Transport);
        CHECK(outcome.Error.Detail == "HTTP 0 (Couldn't connect)");
        REQUIRE(outcome.Projects.size() == 1u);
        CHECK(outcome.Projects[0].key == "OFF");
    }

    TEST_CASE("a failure with nothing saved is empty and failed, never an empty success") {
        ScriptedConnectivity conn;
        conn.Error = TrackerErrorAuth("HTTP 401", 401);
        const ProjectListOutcome outcome =
            LoadProjectList(conn, std::make_shared<FakeLookupCache>(), "Jira", /*offline=*/false);
        CHECK(outcome.Failed);
        CHECK_FALSE(outcome.FromSaved);
        CHECK(outcome.Projects.empty());
        CHECK(outcome.Error.Kind == TrackerErrorKind::Auth);
    }

    TEST_CASE("the saved lists of two trackers stay separate") {
        auto store = std::make_shared<FakeLookupCache>();
        SaveList(*store, "Jira", {Project("OFF", "Jira project")});
        SaveList(*store, "Plane", {Project("PLN", "Plane project")});
        ScriptedConnectivity conn;
        const ProjectListOutcome outcome = LoadProjectList(conn, store, "Plane", /*offline=*/true);
        REQUIRE(outcome.Projects.size() == 1u);
        CHECK(outcome.Projects[0].key == "PLN");
    }

    TEST_CASE("a listing that throws is a failure, not a crash, and still serves the saved list") {
        auto store = std::make_shared<FakeLookupCache>();
        SaveList(*store, "Jira", {Project("OFF", "Offline First")});
        ScriptedConnectivity conn;
        conn.Throws = true;
        ProjectListOutcome outcome;
        CHECK_NOTHROW(outcome = LoadProjectList(conn, store, "Jira", /*offline=*/false));
        CHECK(outcome.Failed);
        CHECK(outcome.FromSaved);
        CHECK(outcome.Error.Kind == TrackerErrorKind::Unknown);
        CHECK(outcome.Error.Detail.find("listing exploded") != std::string::npos);
    }

    TEST_CASE("store failures never cost the live list or throw") {
        ScriptedConnectivity conn;
        conn.Projects = {Project("OFF", "Offline First")};
        ProjectListOutcome live;
        CHECK_NOTHROW(live = LoadProjectList(conn, std::make_shared<ThrowingWriteLookupCache>(), "Jira", false));
        CHECK_FALSE(live.Failed);
        CHECK(live.Projects.size() == 1u);

        ProjectListOutcome offline;
        CHECK_NOTHROW(offline = LoadProjectList(conn, std::make_shared<ThrowingReadLookupCache>(), "Jira", true));
        CHECK(offline.Failed);
        CHECK_FALSE(offline.FromSaved);
        CHECK(offline.Projects.empty());
    }

    TEST_CASE("no store or no backend key: nothing saved or restored") {
        ScriptedConnectivity conn;
        conn.Projects = {Project("OFF", "Offline First")};
        CHECK(LoadProjectList(conn, nullptr, "Jira", false).Projects.size() == 1u);
        CHECK_FALSE(LoadProjectList(conn, nullptr, "Jira", true).FromSaved);

        auto store = std::make_shared<FakeLookupCache>();
        LoadProjectList(conn, store, "", false);
        CHECK(store->UpsertCalls.load() == 0);
    }
}
