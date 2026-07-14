// GitHubProjectsV2Pure doctest — pins the Projects v2 wire contract
// (docs/plans/shipped/github-projects-v2-fields.md): the GraphQL documents, the
// catalog / items-page / item-id parsers, the grid-string→ProjectV2FieldValue
// builder, and the field-name slugifier. Hermetic: no cpr, no config, no filesystem.

#include "GitHubProjectsV2Pure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using smatchet::github::BuildProjectV2FieldValue;
using smatchet::github::ParseProjectV2Catalog;
using smatchet::github::ParseProjectV2ItemIdForProject;
using smatchet::github::ParseProjectV2ItemsPage;
using smatchet::github::ProjectV2Catalog;
using smatchet::github::SlugifyProjectV2FieldName;

namespace {

// Canonical dual-query response: the org half resolved, the user half is null
// (real GitHub also appends an errors[] entry for the unmatched owner kind).
nlohmann::json CatalogBody() {
    return nlohmann::json::parse(R"({
      "data": {
        "organization": { "projectV2": {
          "id": "PVT_kwDO1",
          "title": "Roadmap",
          "fields": { "nodes": [
            { "id": "PVTF_title",  "name": "Title",     "dataType": "TITLE" },
            { "id": "PVTF_text",   "name": "Team Notes", "dataType": "TEXT" },
            { "id": "PVTF_num",    "name": "Estimate",   "dataType": "NUMBER" },
            { "id": "PVTF_date",   "name": "Due Date",   "dataType": "DATE" },
            { "id": "PVTSSF_sel",  "name": "Priority",   "dataType": "SINGLE_SELECT",
              "options": [ { "id": "opt1", "name": "High" }, { "id": "opt2", "name": "Low" } ] },
            { "id": "PVTIF_iter",  "name": "Sprint",     "dataType": "ITERATION" }
          ] } } },
        "user": null
      },
      "errors": [ { "message": "Could not resolve to a User with the login of 'acme-org'." } ]
    })");
}

// A catalog fixture matching CatalogBody() — for the parsers/builders that take
// the parsed field list instead of the raw response.
std::vector<TrackerField> CatalogFields() {
    const auto parsed = ParseProjectV2Catalog(CatalogBody());
    REQUIRE(parsed);
    return parsed.value().Fields;
}

TrackerField FieldById(const std::string& id) {
    for (const auto& field : CatalogFields()) {
        if (field.Id == id) {
            return field;
        }
    }
    REQUIRE_MESSAGE(false, "catalog fixture has no field '", id, "'");
    return TrackerField();
}

} // namespace

TEST_CASE("ProjectV2 GraphQL documents — wire strings pinned") {
    // Structural anchors only (not full-document golden strings) so formatting-only
    // rewrites don't churn the test, but a renamed input or dropped fragment fails.
    const std::string fields = smatchet::github::ProjectV2FieldsQueryDocument();
    CHECK(fields.find("organization(login: $owner)") != std::string::npos);
    CHECK(fields.find("user(login: $owner)") != std::string::npos);
    CHECK(fields.find("projectV2(number: $number)") != std::string::npos);
    CHECK(fields.find("ProjectV2SingleSelectField") != std::string::npos);

    const std::string items = smatchet::github::ProjectV2ItemsQueryDocument();
    CHECK(items.find("items(first: $first, after: $after)") != std::string::npos);
    CHECK(items.find("pageInfo { hasNextPage endCursor }") != std::string::npos);
    CHECK(items.find("ProjectV2ItemFieldIterationValue") != std::string::npos);

    const std::string itemForIssue = smatchet::github::ProjectV2ItemForIssueQueryDocument();
    CHECK(itemForIssue.find("issueOrPullRequest(number: $number)") != std::string::npos);
    CHECK(itemForIssue.find("projectItems(first: 20)") != std::string::npos);

    const std::string update = smatchet::github::ProjectV2UpdateFieldValueMutationDocument();
    CHECK(update.find("updateProjectV2ItemFieldValue") != std::string::npos);
    CHECK(update.find("$value: ProjectV2FieldValue!") != std::string::npos);

    const std::string clear = smatchet::github::ProjectV2ClearFieldValueMutationDocument();
    CHECK(clear.find("clearProjectV2ItemFieldValue") != std::string::npos);
    CHECK(clear.find("$value") == std::string::npos); // clear carries no value input
}

TEST_CASE("ParseProjectV2Catalog — representable fields map to pv2.<slug> columns") {
    const auto parsed = ParseProjectV2Catalog(CatalogBody());
    REQUIRE(parsed);
    const ProjectV2Catalog& catalog = parsed.value();
    CHECK(catalog.ProjectNodeId == "PVT_kwDO1");
    CHECK(catalog.ProjectTitle == "Roadmap");
    // TITLE (built-in projection) skipped; TEXT/NUMBER/DATE/SINGLE_SELECT/ITERATION kept.
    REQUIRE(catalog.Fields.size() == 5);

    const TrackerField& text = catalog.Fields[0];
    CHECK(text.Id == "pv2.team-notes");
    CHECK(text.Name == "Team Notes (project)");
    CHECK(text.Family == TrackerFieldFamily::Text);
    CHECK(text.SchemaSystem == "projects_v2");
    CHECK(text.SchemaCustom == "PVTF_text");
    CHECK(text.IsCustom);
    CHECK_FALSE(text.ReadOnly);

    CHECK(catalog.Fields[1].Id == "pv2.estimate");
    CHECK(catalog.Fields[1].Family == TrackerFieldFamily::Number);
    CHECK(catalog.Fields[2].Id == "pv2.due-date");
    CHECK(catalog.Fields[2].Family == TrackerFieldFamily::Date);

    const TrackerField& select = catalog.Fields[3];
    CHECK(select.Id == "pv2.priority");
    CHECK(select.Family == TrackerFieldFamily::SelectSingle);
    REQUIRE(select.AllowedValueOptions.size() == 2);
    CHECK(select.AllowedValueOptions[0].Id == "opt1");
    CHECK(select.AllowedValueOptions[0].Value == "High");
    CHECK(select.AllowedValues == std::vector<std::string>{"High", "Low"});

    // ITERATION surfaces as a read-only text column (no iteration-id picker yet).
    CHECK(catalog.Fields[4].Id == "pv2.sprint");
    CHECK(catalog.Fields[4].ReadOnly);
}

TEST_CASE("ParseProjectV2Catalog — user-owned half and failure shapes") {
    SUBCASE("user half resolving works the same as organization") {
        nlohmann::json body = CatalogBody();
        body["data"]["user"] = body["data"]["organization"];
        body["data"]["organization"] = nullptr;
        const auto parsed = ParseProjectV2Catalog(body);
        REQUIRE(parsed);
        CHECK(parsed.value().ProjectNodeId == "PVT_kwDO1");
    }
    SUBCASE("neither half resolved → Err pointing at the config anchor") {
        const auto parsed = ParseProjectV2Catalog(nlohmann::json::parse(
            R"({"data": {"organization": null, "user": null}, "errors": [{"message": "nope"}]})"));
        REQUIRE_FALSE(parsed);
        CHECK(parsed.error().find("project number") != std::string::npos);
    }
    SUBCASE("missing project node id → Err") {
        CHECK_FALSE(
            ParseProjectV2Catalog(nlohmann::json::parse(R"({"data": {"user": {"projectV2": {"title": "x"}}}})")));
    }
    SUBCASE("slug collisions get numeric suffixes") {
        const auto parsed = ParseProjectV2Catalog(nlohmann::json::parse(R"({
          "data": { "user": { "projectV2": { "id": "P1", "title": "t", "fields": { "nodes": [
            { "id": "F1", "name": "Size", "dataType": "TEXT" },
            { "id": "F2", "name": "size", "dataType": "NUMBER" },
            { "id": "F3", "name": "SIZE", "dataType": "DATE" }
          ] } } } } })"));
        REQUIRE(parsed);
        REQUIRE(parsed.value().Fields.size() == 3);
        CHECK(parsed.value().Fields[0].Id == "pv2.size");
        CHECK(parsed.value().Fields[1].Id == "pv2.size-2");
        CHECK(parsed.value().Fields[2].Id == "pv2.size-3");
    }
}

TEST_CASE("ParseProjectV2ItemsPage — items join by owner/repo#N and display-map values") {
    const nlohmann::json body = nlohmann::json::parse(R"({
      "data": { "node": { "items": {
        "pageInfo": { "hasNextPage": true, "endCursor": "CUR1" },
        "nodes": [
          { "content": { "number": 7, "repository": { "name": "smatchet", "owner": { "login": "acme" } } },
            "fieldValues": { "nodes": [
              { "text": "needs design", "field": { "id": "PVTF_text" } },
              { "number": 3.0, "field": { "id": "PVTF_num" } },
              { "date": "2026-08-01", "field": { "id": "PVTF_date" } },
              { "name": "High", "field": { "id": "PVTSSF_sel" } },
              { "title": "Sprint 12", "field": { "id": "PVTIF_iter" } },
              { "text": "from a projection", "field": { "id": "PVTF_title" } }
            ] } },
          { "content": null, "fieldValues": { "nodes": [] } },
          { "content": { "number": 9, "repository": { "name": "smatchet", "owner": { "login": "acme" } } },
            "fieldValues": { "nodes": [] } }
        ] } } }
    })");
    const auto parsed = ParseProjectV2ItemsPage(body, CatalogFields());
    REQUIRE(parsed);
    CHECK(parsed.value().HasNext);
    CHECK(parsed.value().EndCursor == "CUR1");
    // Draft item (content null) skipped; the empty-values item still joins.
    REQUIRE(parsed.value().Items.size() == 2);
    const auto& first = parsed.value().Items[0];
    CHECK(first.TicketKey == "acme/smatchet#7");
    // 5 catalog values kept; the projection value (unknown field node id) dropped.
    REQUIRE(first.Values.size() == 5);
    CHECK(first.Values[0].first == "pv2.team-notes");
    CHECK(first.Values[0].second == "needs design");
    CHECK(first.Values[1].first == "pv2.estimate");
    CHECK(first.Values[1].second == "3"); // integral number formats without float noise
    CHECK(first.Values[2].second == "2026-08-01");
    CHECK(first.Values[3].second == "High");
    CHECK(first.Values[4].second == "Sprint 12");
    CHECK(parsed.value().Items[1].TicketKey == "acme/smatchet#9");
}

TEST_CASE("ParseProjectV2ItemsPage — malformed body → Err; last page → HasNext false") {
    CHECK_FALSE(ParseProjectV2ItemsPage(nlohmann::json::parse(R"({"data": {"node": null}})"), CatalogFields()));
    const auto parsed = ParseProjectV2ItemsPage(
        nlohmann::json::parse(
            R"({"data": {"node": {"items": {"pageInfo": {"hasNextPage": false, "endCursor": null}, "nodes": []}}}})"),
        CatalogFields());
    REQUIRE(parsed);
    CHECK_FALSE(parsed.value().HasNext);
    CHECK(parsed.value().Items.empty());
}

TEST_CASE("ParseProjectV2ItemIdForProject — matches on the project node id") {
    const nlohmann::json body = nlohmann::json::parse(R"({
      "data": { "repository": { "issueOrPullRequest": { "projectItems": { "nodes": [
        { "id": "ITEM_other", "project": { "id": "PVT_other" } },
        { "id": "ITEM_ours",  "project": { "id": "PVT_kwDO1" } }
      ] } } } }
    })");
    SUBCASE("issue on the board → its item id") {
        const auto parsed = ParseProjectV2ItemIdForProject(body, "PVT_kwDO1");
        REQUIRE(parsed);
        CHECK(parsed.value() == "ITEM_ours");
    }
    SUBCASE("issue not on this board → Ok(empty), not an error") {
        const auto parsed = ParseProjectV2ItemIdForProject(body, "PVT_absent");
        REQUIRE(parsed);
        CHECK(parsed.value().empty());
    }
    SUBCASE("issue/PR not found at all → Err") {
        CHECK_FALSE(ParseProjectV2ItemIdForProject(
            nlohmann::json::parse(R"({"data": {"repository": {"issueOrPullRequest": null}}})"), "PVT_kwDO1"));
    }
}

TEST_CASE("BuildProjectV2FieldValue — typed value objects per field family") {
    SUBCASE("text") {
        const auto r = BuildProjectV2FieldValue(FieldById("pv2.team-notes"), {"hello world"});
        REQUIRE(r);
        CHECK(r.value() == nlohmann::json{{"text", "hello world"}});
    }
    SUBCASE("number — validated, full-consume") {
        const auto ok = BuildProjectV2FieldValue(FieldById("pv2.estimate"), {" 2.5 "});
        REQUIRE(ok);
        CHECK(ok.value() == nlohmann::json{{"number", 2.5}});
        const auto bad = BuildProjectV2FieldValue(FieldById("pv2.estimate"), {"3 days"});
        REQUIRE_FALSE(bad);
        CHECK(bad.error().find("number") != std::string::npos);
    }
    SUBCASE("date — strict YYYY-MM-DD") {
        const auto ok = BuildProjectV2FieldValue(FieldById("pv2.due-date"), {"2026-08-01"});
        REQUIRE(ok);
        CHECK(ok.value() == nlohmann::json{{"date", "2026-08-01"}});
        CHECK_FALSE(BuildProjectV2FieldValue(FieldById("pv2.due-date"), {"08/01/2026"}));
        CHECK_FALSE(BuildProjectV2FieldValue(FieldById("pv2.due-date"), {"2026-8-1"}));
    }
    SUBCASE("single-select — option resolved by display name or id") {
        const auto byName = BuildProjectV2FieldValue(FieldById("pv2.priority"), {"High"});
        REQUIRE(byName);
        CHECK(byName.value() == nlohmann::json{{"singleSelectOptionId", "opt1"}});
        const auto byId = BuildProjectV2FieldValue(FieldById("pv2.priority"), {"opt2"});
        REQUIRE(byId);
        CHECK(byId.value() == nlohmann::json{{"singleSelectOptionId", "opt2"}});
        const auto unknown = BuildProjectV2FieldValue(FieldById("pv2.priority"), {"Urgent"});
        REQUIRE_FALSE(unknown);
        CHECK(unknown.error().find("no option") != std::string::npos);
    }
    SUBCASE("empty / blank values → Ok(null) — the clear-mutation route") {
        const auto r = BuildProjectV2FieldValue(FieldById("pv2.team-notes"), {"  ", ""});
        REQUIRE(r);
        CHECK(r.value().is_null());
        const auto empty = BuildProjectV2FieldValue(FieldById("pv2.estimate"), {});
        REQUIRE(empty);
        CHECK(empty.value().is_null());
    }
    SUBCASE("read-only iteration field rejects non-empty edits") {
        const auto r = BuildProjectV2FieldValue(FieldById("pv2.sprint"), {"Sprint 13"});
        REQUIRE_FALSE(r);
        CHECK(r.error().find("read-only") != std::string::npos);
    }
}

TEST_CASE("SlugifyProjectV2FieldName — lowercase, dash-collapsed, never empty") {
    CHECK(SlugifyProjectV2FieldName("Team Notes") == "team-notes");
    CHECK(SlugifyProjectV2FieldName("  Q3 / OKR!! score ") == "q3-okr-score");
    CHECK(SlugifyProjectV2FieldName("Status") == "status");
    CHECK(SlugifyProjectV2FieldName("---") == "field");
    CHECK(SlugifyProjectV2FieldName("") == "field");
}
