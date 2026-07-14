// github-projects-v2-fields.md — fixture coverage for the Projects v2 GraphQL shell
// (GitHubProjectsV2.cpp) and its GitHubClient integration (FetchFieldCatalog append +
// the UpdateField pv2 write path), driven at the in-process httplib loopback over
// real cpr HTTP. Every projects-v2 call POSTs to the same /graphql path, so the
// scripted handler routes by request-BODY content (which GraphQL document ran) —
// unlike the REST fixtures that route by path.

#include "GitHubClient.h"
#include "GitHubProjectsV2.h"
#include "GitHubProjectsV2Pure.h"
#include "HttpRequestCapture.h"
#include "JiraCatalogHttpFixture.h"
#include "TestEnvGuard.h"
#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet_tests::HttpRequestCapture;

namespace {

const std::string kGraphQlPath = "/graphql";

// The cfg-less mutation paths read ConfigManager::Load() per request — persist the
// loopback config (with the projects-v2 board anchor) into the TestEnvGuard-redirected
// dir so they resolve to the fixture.
void SaveLoopbackGitHubConfig(const JiraCatalogHttpFixture& fx, int projectNumber) {
    TrackerConfig cfg = ConfigManager::Load();
    cfg.TrackerType = "github";
    cfg.GitHubBaseUrl = "http://127.0.0.1:" + std::to_string(fx.Port());
    cfg.GitHubPat = "loopback-pat";
    cfg.GitHubOwner = "o";
    cfg.GitHubRepo = "r";
    cfg.GitHubProjectNumber = projectNumber;
    ConfigManager::Save(cfg);
}

std::string BaseUrl(const JiraCatalogHttpFixture& fx) { return "http://127.0.0.1:" + std::to_string(fx.Port()); }

// data.organization.projectV2 response with one single-select field ("Priority")
// and one text field ("Team Notes") — the org half resolved, the user half null.
nlohmann::json CatalogResponse() {
    return nlohmann::json::parse(R"({
      "data": { "organization": { "projectV2": {
        "id": "PVT_1", "title": "Roadmap",
        "fields": { "nodes": [
          { "id": "PVTF_text",  "name": "Team Notes", "dataType": "TEXT" },
          { "id": "PVTSSF_sel", "name": "Priority",   "dataType": "SINGLE_SELECT",
            "options": [ { "id": "opt1", "name": "High" }, { "id": "opt2", "name": "Low" } ] }
        ] } } },
        "user": null },
      "errors": [ { "message": "Could not resolve to a User with the login of 'o'." } ]
    })");
}

nlohmann::json ItemForIssueResponse(const char* itemId, const char* projectId) {
    nlohmann::json node = nlohmann::json::object();
    node["id"] = itemId;
    node["project"] = nlohmann::json{{"id", projectId}};
    nlohmann::json body = nlohmann::json::object();
    body["data"]["repository"]["issueOrPullRequest"]["projectItems"]["nodes"] = nlohmann::json::array({node});
    return body;
}

// Route the single /graphql path by document content: catalog query →
// items/item-id queries → mutations. Captures every request body for
// variable-shape assertions.
void ScriptGraphQlRouter(JiraCatalogHttpFixture& fx, HttpRequestCapture& capture, const nlohmann::json& catalog,
                         const nlohmann::json& itemLookup, const nlohmann::json& mutationResponse) {
    fx.ScriptHandler(
        kGraphQlPath,
        [&capture, catalog, itemLookup, mutationResponse](const httplib::Request& req) {
            capture.Push(req.body);
            if (req.body.find("organization(login:") != std::string::npos) {
                return catalog;
            }
            if (req.body.find("issueOrPullRequest(number:") != std::string::npos) {
                return itemLookup;
            }
            return mutationResponse;
        },
        "POST");
}

// The pv2 catalog field the grid would hand to UpdateField — matches CatalogResponse().
TrackerField PriorityField() {
    TrackerField f;
    f.Id = "pv2.priority";
    f.Name = "Priority (project)";
    f.Family = TrackerFieldFamily::SelectSingle;
    f.SchemaSystem = "projects_v2";
    f.SchemaCustom = "PVTSSF_sel";
    TrackerFieldOption high;
    high.Id = "opt1";
    high.Value = "High";
    f.AllowedValueOptions.push_back(high);
    f.IsCustom = true;
    return f;
}

} // namespace

TEST_CASE("FetchProjectV2Catalog — dual-owner GraphQL query tolerates the unmatched half's errors[]") {
    JiraCatalogHttpFixture fx;
    SUBCASE("org-owned project parses despite the user-half errors[] entry") {
        fx.ScriptJson(kGraphQlPath, CatalogResponse(), "POST");
        const auto r = smatchet::github::FetchProjectV2Catalog(BaseUrl(fx), "pat", "o", 5);
        REQUIRE(r);
        CHECK(r.value().ProjectNodeId == "PVT_1");
        REQUIRE(r.value().Fields.size() == 2);
        CHECK(r.value().Fields[0].Id == "pv2.team-notes");
        CHECK(r.value().Fields[1].Id == "pv2.priority");
    }
    SUBCASE("neither owner kind resolving → InvalidRequest naming the config anchor") {
        fx.ScriptJson(kGraphQlPath, nlohmann::json::parse(R"({"data": {"organization": null, "user": null},
                                                "errors": [{"message": "nope"}]})"),
                      "POST");
        const auto r = smatchet::github::FetchProjectV2Catalog(BaseUrl(fx), "pat", "o", 5);
        REQUIRE_FALSE(r);
        CHECK(r.error().Kind == TrackerErrorKind::InvalidRequest);
    }
    SUBCASE("HTTP 401 classifies as an auth error") {
        fx.ScriptStatus(kGraphQlPath, 401, "POST");
        const auto r = smatchet::github::FetchProjectV2Catalog(BaseUrl(fx), "pat", "o", 5);
        REQUIRE_FALSE(r);
        CHECK(r.error().Kind == TrackerErrorKind::Auth);
    }
}

TEST_CASE("FetchProjectV2ItemValues — follows endCursor pagination and keys by owner/repo#N") {
    JiraCatalogHttpFixture fx;
    const auto catalogParsed = smatchet::github::ParseProjectV2Catalog(CatalogResponse());
    REQUIRE(catalogParsed);
    const std::vector<TrackerField> fields = catalogParsed.value().Fields;

    auto pageBody = [](int issueNumber, const char* text, bool hasNext, const char* cursor) {
        nlohmann::json item = nlohmann::json::object();
        item["content"] =
            nlohmann::json{{"number", issueNumber},
                           {"repository", nlohmann::json{{"name", "r"}, {"owner", nlohmann::json{{"login", "o"}}}}}};
        nlohmann::json value = nlohmann::json::object();
        value["text"] = text;
        value["field"] = nlohmann::json{{"id", "PVTF_text"}};
        item["fieldValues"]["nodes"] = nlohmann::json::array({value});
        nlohmann::json body = nlohmann::json::object();
        body["data"]["node"]["items"]["pageInfo"] = nlohmann::json{{"hasNextPage", hasNext}, {"endCursor", cursor}};
        body["data"]["node"]["items"]["nodes"] = nlohmann::json::array({item});
        return body;
    };
    fx.ScriptHandler(
        kGraphQlPath,
        [&pageBody](const httplib::Request& req) {
            // Page 2 is requested with the cursor from page 1; page 1 with after:null.
            if (req.body.find("\"after\":\"CUR1\"") != std::string::npos) {
                return pageBody(9, "second", false, "");
            }
            return pageBody(7, "first", true, "CUR1");
        },
        "POST");

    std::string warning;
    const auto r = smatchet::github::FetchProjectV2ItemValues(BaseUrl(fx), "pat", "PVT_1", fields, &warning);
    REQUIRE(r);
    CHECK(warning.empty());
    CHECK(fx.RequestCount(kGraphQlPath) == 2);
    REQUIRE(r.value().size() == 2);
    const auto& seven = r.value().at("o/r#7");
    REQUIRE(seven.size() == 1);
    CHECK(seven[0].first == "pv2.team-notes");
    CHECK(seven[0].second == "first");
    CHECK(r.value().at("o/r#9")[0].second == "second");
}

TEST_CASE("GitHubClient FetchFieldCatalog — configured board appends pv2 columns; failure degrades to Warning") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx, 5);
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    TrackerConfig cfg = ConfigManager::Load();

    SUBCASE("board reachable → native fields + pv2 columns, no warning") {
        fx.ScriptJson(kGraphQlPath, CatalogResponse(), "POST");
        const auto r = client.FetchFieldCatalog(cfg, "o/r");
        REQUIRE(r);
        CHECK(r.value().Warning.empty());
        bool sawText = false;
        bool sawSelect = false;
        for (const auto& field : r.value().Fields) {
            sawText = sawText || field.Id == "pv2.team-notes";
            sawSelect = sawSelect || field.Id == "pv2.priority";
        }
        CHECK(sawText);
        CHECK(sawSelect);
        // Editmeta must whitelist the pv2 ids — the cache is default-deny for
        // ids missing from a loaded per-issue map.
        const auto editMeta = client.FetchIssueEditMeta(cfg, "o/r#7");
        REQUIRE(editMeta);
        REQUIRE(editMeta.value().count("pv2.priority") == 1);
        CHECK(editMeta.value().at("pv2.priority"));
    }
    SUBCASE("board fetch failing → native catalog still loads, Warning set") {
        fx.ScriptStatus(kGraphQlPath, 500, "POST");
        const auto r = client.FetchFieldCatalog(cfg, "o/r");
        REQUIRE(r);
        CHECK_FALSE(r.value().Fields.empty());
        CHECK(r.value().Warning.find("project #5") != std::string::npos);
    }
    SUBCASE("no board configured → no /graphql traffic at all") {
        SaveLoopbackGitHubConfig(fx, 0);
        const auto r = client.FetchFieldCatalog(ConfigManager::Load(), "o/r");
        REQUIRE(r);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }
}

TEST_CASE("GitHubClient UpdateField pv2 — item resolve + typed updateProjectV2ItemFieldValue mutation") {
    smatchet_tests::TestEnvGuard guard;
    JiraCatalogHttpFixture fx;
    SaveLoopbackGitHubConfig(fx, 5);
    GitHubClient client("http://bogus.invalid", "ctor-pat");
    nlohmann::json mutationOk = nlohmann::json::object();
    mutationOk["data"]["updateProjectV2ItemFieldValue"]["projectV2Item"] = nlohmann::json{{"id", "ITEM_1"}};

    SUBCASE("option display name resolves to singleSelectOptionId in the mutation variables") {
        HttpRequestCapture capture;
        ScriptGraphQlRouter(fx, capture, CatalogResponse(), ItemForIssueResponse("ITEM_1", "PVT_1"), mutationOk);
        const TrackerError err = client.UpdateField("o/r#7", PriorityField(), {"High"});
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        // catalog ensure + item resolve + mutation = 3 GraphQL round-trips.
        REQUIRE(bodies.size() == 3);
        const nlohmann::json mutation = nlohmann::json::parse(bodies[2]);
        CHECK(mutation["query"].get<std::string>().find("updateProjectV2ItemFieldValue") != std::string::npos);
        CHECK(mutation["variables"]["project"].get<std::string>() == "PVT_1");
        CHECK(mutation["variables"]["item"].get<std::string>() == "ITEM_1");
        CHECK(mutation["variables"]["field"].get<std::string>() == "PVTSSF_sel");
        CHECK(mutation["variables"]["value"] == nlohmann::json{{"singleSelectOptionId", "opt1"}});
    }
    SUBCASE("empty value routes to the clear mutation (no $value input)") {
        nlohmann::json clearOk = nlohmann::json::object();
        clearOk["data"]["clearProjectV2ItemFieldValue"]["projectV2Item"] = nlohmann::json{{"id", "ITEM_1"}};
        HttpRequestCapture capture;
        ScriptGraphQlRouter(fx, capture, CatalogResponse(), ItemForIssueResponse("ITEM_1", "PVT_1"), clearOk);
        const TrackerError err = client.UpdateField("o/r#7", PriorityField(), {""});
        CHECK(err.IsOk());
        const std::vector<std::string> bodies = capture.Snapshot();
        REQUIRE(bodies.size() == 3);
        const nlohmann::json mutation = nlohmann::json::parse(bodies[2]);
        CHECK(mutation["query"].get<std::string>().find("clearProjectV2ItemFieldValue") != std::string::npos);
        CHECK(mutation["variables"].find("value") == mutation["variables"].end());
    }
    SUBCASE("issue not on the board → InvalidRequest telling the user to add it, no mutation sent") {
        HttpRequestCapture capture;
        ScriptGraphQlRouter(fx, capture, CatalogResponse(), ItemForIssueResponse("ITEM_x", "PVT_other"), mutationOk);
        const TrackerError err = client.UpdateField("o/r#7", PriorityField(), {"High"});
        REQUIRE_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(err.Detail.find("add it to the project") != std::string::npos);
        CHECK(capture.Snapshot().size() == 2); // catalog + item resolve only
    }
    SUBCASE("value not matching any option is rejected before any HTTP") {
        const TrackerError err = client.UpdateField("o/r#7", PriorityField(), {"Nonexistent"});
        REQUIRE_FALSE(err.IsOk());
        CHECK(err.Kind == TrackerErrorKind::InvalidRequest);
        CHECK(fx.RequestCount(kGraphQlPath) == 0);
    }
    SUBCASE("mutation-level GraphQL errors[] surface as a failed update") {
        HttpRequestCapture capture;
        ScriptGraphQlRouter(fx, capture, CatalogResponse(), ItemForIssueResponse("ITEM_1", "PVT_1"),
                            nlohmann::json::parse(R"({"data": null,
                                                      "errors": [{"message": "Field value is locked"}]})"));
        const TrackerError err = client.UpdateField("o/r#7", PriorityField(), {"High"});
        REQUIRE_FALSE(err.IsOk());
        CHECK(err.Detail.find("Field value is locked") != std::string::npos);
    }
}
