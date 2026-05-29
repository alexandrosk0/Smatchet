#ifndef SMATCHET_TESTS_FAKE_PLANE_FIXTURE_H
#define SMATCHET_TESTS_FAKE_PLANE_FIXTURE_H

// Slice 2 of docs/plans/shipped/autonomous-debugging-no-creds.md — header-only loader
// that turns a checked-in JSON fixture under tests/fixtures/plane/ into a
// scripted FakeTrackerClient("Plane") instance. Same shape as the GitHub /
// Jira fixtures that ship in sibling slices.
//
// Fixture JSON shape (see tests/fixtures/plane/*.json for examples):
//   {
//     "captured_against": "https://api.plane.so/api/v1/...",
//     "project_identifier": "SMT",
//     "users": [ {"account_id": "...", "display_name": "..."}, ... ],
//     "list_response": { "results": [...], "next_page_results": false, "next_cursor": "" }
//   }
//
// The loader pre-maps the `list_response.results` through PlaneIssueMappingPure
// so the FakeTrackerClient's `FetchIssues` returns the mapped CachedTickets
// without any test code having to know the Plane wire shape.

#include "FakeTrackerClient.h"
#include "PlaneIssueMappingPure.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace smatchet_tests {

class FakePlaneFixture {
  public:
    /// Load a fixture JSON file from disk. Throws std::runtime_error if the
    /// file cannot be opened or the JSON is malformed.
    static FakePlaneFixture LoadFromFile(const std::string& path) {
        std::ifstream in(path);
        if (!in.good()) {
            throw std::runtime_error(std::string("FakePlaneFixture: cannot open fixture file: ") + path);
        }
        std::stringstream buf;
        buf << in.rdbuf();
        return LoadFromJsonText(buf.str(), path);
    }

    static FakePlaneFixture LoadFromJsonText(const std::string& text, const std::string& diagPath = std::string()) {
        FakePlaneFixture f;
        f.sourcePath_ = diagPath;
        const nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
        if (j.is_discarded()) {
            throw std::runtime_error(std::string("FakePlaneFixture: invalid JSON in ") + diagPath);
        }
        if (j.is_object() && j.contains("captured_against") && j["captured_against"].is_string()) {
            f.capturedAgainst_ = j["captured_against"].get<std::string>();
        }
        if (j.is_object() && j.contains("project_identifier") && j["project_identifier"].is_string()) {
            f.projectIdentifier_ = j["project_identifier"].get<std::string>();
        }
        if (j.is_object() && j.contains("users") && j["users"].is_array()) {
            for (const auto& u : j["users"]) {
                if (!u.is_object())
                    continue;
                smatchet::plane::UserDisplayLookup lookup;
                if (u.contains("account_id") && u["account_id"].is_string()) {
                    lookup.AccountId = u["account_id"].get<std::string>();
                }
                if (u.contains("display_name") && u["display_name"].is_string()) {
                    lookup.DisplayName = u["display_name"].get<std::string>();
                }
                f.users_.push_back(std::move(lookup));
            }
        }
        if (j.is_object() && j.contains("list_response")) {
            f.listResponse_ = j["list_response"];
        } else {
            f.listResponse_ = nlohmann::json::object();
        }
        return f;
    }

    /// Build a FakeTrackerClient("Plane") populated from this fixture.
    /// FetchIssues will return the mapped CachedTickets; reachability defaults
    /// to AuthenticatedReachable; all writes succeed with the default issueKey.
    std::unique_ptr<FakeTrackerClient> MakeClient() const {
        auto client = std::unique_ptr<FakeTrackerClient>(new FakeTrackerClient("Plane"));
        nlohmann::json results = nlohmann::json::array();
        if (listResponse_.is_object() && listResponse_.contains("results")) {
            results = listResponse_["results"];
        }
        const std::vector<CachedTicket> tickets =
            smatchet::plane::MapPlaneWorkItemsArrayToCachedTickets(results, projectIdentifier_, users_, nullptr);
        client->SetFetchIssuesResult(tickets, /*fullSyncCompleted=*/true);
        return client;
    }

    const std::string& CapturedAgainst() const { return capturedAgainst_; }
    const std::string& ProjectIdentifier() const { return projectIdentifier_; }
    const std::vector<smatchet::plane::UserDisplayLookup>& Users() const { return users_; }
    const nlohmann::json& ListResponse() const { return listResponse_; }
    const std::string& SourcePath() const { return sourcePath_; }

  private:
    FakePlaneFixture() = default;
    std::string capturedAgainst_;
    std::string projectIdentifier_;
    std::vector<smatchet::plane::UserDisplayLookup> users_;
    nlohmann::json listResponse_ = nlohmann::json::object();
    std::string sourcePath_;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_PLANE_FIXTURE_H
