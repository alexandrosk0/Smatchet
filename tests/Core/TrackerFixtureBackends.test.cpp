// Fixture backends (TrackerFixtureBackendBase + the Linear / Plane / GitHub fixtures): the shared
// fixture loader's caps and diagnostics, and the shared factory that serves one fixture whatever
// tracker type is asked for. The fixture path is env-var-selectable, so the loader must refuse an
// unreadable, oversized or malformed file with a diagnostic rather than read it whole.

#include "ConfigManager.h"
#include "ITrackerBackend.h"
#include "ITrackerBackendFactory.h"
#include "ITrackerConnectivity.h"
#include "Json/BoundedJsonParse.h"
#include "Tracker/GitHubFixtureBackend.h"
#include "Tracker/LinearFixtureBackend.h"
#include "Tracker/PlaneFixtureBackend.h"

#include <doctest/doctest.h>
#include <ghc/filesystem.hpp>

#include <chrono>
#include <fstream>
#include <memory>
#include <string>

namespace fs = ghc::filesystem;

namespace {

// Per-test scratch directory, removed at scope exit.
class TempDir {
  public:
    TempDir() {
        const auto unique =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
                .count();
        path_ = fs::temp_directory_path() / ("smatchet_fixture_backends_" + std::to_string(unique));
        std::error_code ec;
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string File(const std::string& name, const std::string& contents) const {
        const std::string p = (path_ / name).generic_string();
        std::ofstream out(p.c_str(), std::ios::binary | std::ios::trunc);
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return p;
    }
    std::string Missing() const { return (path_ / "missing.json").generic_string(); }

  private:
    fs::path path_;
};

bool Contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

} // namespace

TEST_CASE("Fixture loader: a valid fixture loads with no error") {
    TempDir tmp;
    CHECK(smatchet::linear::LinearFixtureBackend(tmp.File("ok.json", R"({"nodes":[]})")).LoadError().empty());
    CHECK(smatchet::github::GitHubFixtureBackend(tmp.File("gh.json", R"({"nodes":[]})"), "o", "r", false)
              .LoadError()
              .empty());
}

TEST_CASE("Fixture loader: missing, malformed and oversized files fail with a diagnostic naming the file") {
    TempDir tmp;
    const std::string missing = tmp.Missing();
    const std::string missingErr = smatchet::linear::LinearFixtureBackend(missing).LoadError();
    CHECK(Contains(missingErr, "cannot open fixture file"));
    CHECK(Contains(missingErr, missing));

    const std::string bad = tmp.File("bad.json", "{not json");
    CHECK(Contains(smatchet::plane::PlaneFixtureBackend(bad).LoadError(), "invalid JSON in fixture file"));

    // One byte over the parse cap: refused by size before it is read, never parsed.
    const std::string big =
        tmp.File("big.json", std::string(smatchet::json_safe::kDefaultMaxBytes + 1u, ' ') + R"({"nodes":[]})");
    const std::string bigErr = smatchet::linear::LinearFixtureBackend(big).LoadError();
    CHECK(Contains(bigErr, "too large"));
    CHECK(Contains(bigErr, big));

    // The GitHub fixture uses the same loader, then insists on an object root.
    CHECK(Contains(smatchet::github::GitHubFixtureBackend(missing, "o", "r", false).LoadError(),
                   "cannot open fixture file"));
    CHECK(Contains(smatchet::github::GitHubFixtureBackend(tmp.File("arr.json", "[1,2]"), "o", "r", false).LoadError(),
                   "root is not an object"));
}

TEST_CASE("Fixture factory: serves its own backend type whatever type is requested") {
    TempDir tmp;
    const std::string path = tmp.File("ok.json", R"({"nodes":[]})");
    const TrackerConfig cfg;

    std::unique_ptr<ITrackerBackendFactory> linear = smatchet::linear::MakeLinearFixtureBackendFactory(path);
    REQUIRE(linear);
    std::unique_ptr<ITrackerBackend> fromJira = linear->Create("Jira", cfg);
    REQUIRE(fromJira);
    CHECK(fromJira->Connectivity().GetTrackerType() == "Linear");
    std::unique_ptr<ITrackerBackend> fromLinear = linear->Create("Linear", cfg);
    REQUIRE(fromLinear);
    CHECK(fromLinear->Connectivity().GetTrackerType() == "Linear");

    std::unique_ptr<ITrackerBackendFactory> plane = smatchet::plane::MakePlaneFixtureBackendFactory(path);
    REQUIRE(plane);
    std::unique_ptr<ITrackerBackend> fromEmpty = plane->Create("", cfg);
    REQUIRE(fromEmpty);
    CHECK(fromEmpty->Connectivity().GetTrackerType() == "Plane");

    // A fixture that fails to load still yields a backend: it reports the load error instead of rows.
    std::unique_ptr<ITrackerBackend> broken =
        smatchet::linear::MakeLinearFixtureBackendFactory(tmp.Missing())->Create("Linear", cfg);
    REQUIRE(broken);
    CHECK(broken->Connectivity().GetTrackerType() == "Linear");
}
