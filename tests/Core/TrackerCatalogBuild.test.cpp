// Slice 0 WS2 of docs/plans/multi-grid-tabs.md — characterize the REAL
// Jira catalog-BUILD path (`JiraClient::FetchFieldCatalog` +
// `MergeProjectComponentsFromEndpoint`) against scripted loopback HTTP, before
// the multi-pane refactor touches anything near it. Closes the test.md P2
// (2026-06-01): the pre-built-catalog fake exercised catalog *consumption* but
// never catalog *build*, which hid the "components rendered as text"
// bug class. These are characterization tests — they pin CURRENT behaviour
// (including the surprising bits, flagged inline) and must not be "fixed"
// without a deliberate production change.

#include "JiraClient.h"
#include "JiraCatalogHttpFixture.h"
#include "TrackerFieldSchema.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace {

const TrackerField* FindField(const std::vector<TrackerField>& fields, const std::string& id) {
    for (const auto& f : fields) {
        if (f.Id == id) {
            return &f;
        }
    }
    return nullptr;
}

nlohmann::json SystemField(const char* id, const char* name, const char* type, const char* system) {
    nlohmann::json f;
    f["id"] = id;
    f["name"] = name;
    f["schema"] = {{"type", type}, {"system", system}};
    return f;
}

nlohmann::json ArraySystemField(const char* id, const char* name, const char* items, const char* system) {
    nlohmann::json f;
    f["id"] = id;
    f["name"] = name;
    f["schema"] = {{"type", "array"}, {"items", items}, {"system", system}};
    return f;
}

// The /rest/api/3/field baseline served by most cases below: the system fields
// from the components-as-text incident plus three customfield_* shapes (multi-select
// option array, number, user array) for the per-schema classification asserts.
nlohmann::json BaselineFieldList() {
    nlohmann::json customMulti;
    customMulti["id"] = "customfield_10010";
    customMulti["name"] = "Team Multi";
    customMulti["schema"] = {{"type", "array"},
                             {"items", "option"},
                             {"custom", "com.atlassian.jira.plugin.system.customfieldtypes:multiselect"},
                             {"customId", 10010}};

    nlohmann::json customNumber;
    customNumber["id"] = "customfield_10020";
    customNumber["name"] = "Story Points";
    customNumber["schema"] = {
        {"type", "number"}, {"custom", "com.atlassian.jira.plugin.system.customfieldtypes:float"}, {"customId", 10020}};

    nlohmann::json customUsers;
    customUsers["id"] = "customfield_10030";
    customUsers["name"] = "Reviewers";
    customUsers["schema"] = {{"type", "array"},
                             {"items", "user"},
                             {"custom", "com.atlassian.jira.plugin.system.customfieldtypes:multiuserpicker"},
                             {"customId", 10030}};

    return nlohmann::json::array({
        SystemField("summary", "Summary", "string", "summary"),
        SystemField("priority", "Priority", "priority", "priority"),
        SystemField("status", "Status", "status", "status"),
        SystemField("issuetype", "Issue Type", "issuetype", "issuetype"),
        SystemField("assignee", "Assignee", "user", "assignee"),
        ArraySystemField("components", "Components", "component", "components"),
        ArraySystemField("labels", "Labels", "string", "labels"),
        customMulti,
        customNumber,
        customUsers,
    });
}

void ScriptGlobalEnrichmentEndpoints(JiraCatalogHttpFixture& fx) {
    fx.ScriptJson("/rest/api/3/priority",
                  nlohmann::json::array({{{"id", "1"}, {"name", "High"}}, {{"id", "2"}, {"name", "Low"}}}));
    fx.ScriptJson("/rest/api/3/status",
                  nlohmann::json::array({{{"id", "10000"}, {"name", "To Do"}}, {{"id", "10001"}, {"name", "Done"}}}));
    fx.ScriptJson("/rest/api/3/issuetype",
                  nlohmann::json::array({{{"id", "10002"}, {"name", "Task"}}, {{"id", "10003"}, {"name", "Bug"}}}));
}

// One createmeta project/issuetype node with a required summary, a components
// allowedValues list, and allowedValues for the custom multi-select.
nlohmann::json ScopedCreateMeta() {
    nlohmann::json fieldsObj;
    fieldsObj["summary"] = {{"required", true}, {"name", "Summary"}};
    fieldsObj["components"] = {{"required", false},
                               {"allowedValues", nlohmann::json::array({{{"id", "30001"}, {"name", "Audio"}}})}};
    fieldsObj["customfield_10010"] = {{"required", false},
                                      {"allowedValues", nlohmann::json::array({{{"id", "20001"}, {"value", "Red"}},
                                                                               {{"id", "20002"}, {"value", "Blue"}}})}};

    nlohmann::json issueType;
    issueType["id"] = "10002";
    issueType["name"] = "Task";
    issueType["subtask"] = false;
    issueType["fields"] = fieldsObj;

    nlohmann::json project;
    project["key"] = "BLU";
    project["issuetypes"] = nlohmann::json::array({issueType});

    nlohmann::json meta;
    meta["projects"] = nlohmann::json::array({project});
    return meta;
}

void ScriptScopedEndpoints(JiraCatalogHttpFixture& fx) {
    fx.ScriptJson("/rest/api/3/issue/createmeta", ScopedCreateMeta());
    fx.ScriptJson("/rest/api/3/project/BLU/component",
                  nlohmann::json{{"isLast", true},
                                 {"values", nlohmann::json::array({{{"id", "30002"}, {"name", "Renderer"}},
                                                                   {{"id", "30003"}, {"name", "audio tools"}}})}});
    fx.ScriptJson("/rest/api/3/project/BLU",
                  nlohmann::json{{"key", "BLU"},
                                 {"issueTypes", nlohmann::json::array({{{"id", "10002"}, {"name", "Task"}},
                                                                       {{"id", "10003"}, {"name", "Bug"}}})}});
}

} // namespace

TEST_CASE("FetchFieldCatalog unscoped — components stays dropdown-eligible with zero options (PR #672 pin)") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    const bool ok = client.FetchFieldCatalog(fx.Config(), std::string(), fields, components, issueTypeMeta, error);

    REQUIRE(ok);
    CHECK(error.empty());

    const TrackerField* comp = FindField(fields, "components");
    REQUIRE(comp != nullptr);
    // The #672 fix: `components` classifies SelectMulti from items=="component"
    // alone — no options needed — so the grid offers a dropdown, not a text editor.
    CHECK(comp->Family == TrackerFieldFamily::SelectMulti);
    CHECK(comp->IsArray);
    CHECK(comp->AllowedValueOptions.empty()); // unscoped fetch skips Phase-3 component enrichment

    // Unscoped fetch must not touch any project-scoped endpoint.
    CHECK(fx.RequestCount("/rest/api/3/issue/createmeta") == 0);
    CHECK(fx.RequestCount("/rest/api/3/project/BLU/component") == 0);
    CHECK(issueTypeMeta.empty());
}

TEST_CASE("FetchFieldCatalog unscoped — global enrichment fills priority/status/issuetype options") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), std::string(), fields, components, issueTypeMeta, error));

    const TrackerField* priority = FindField(fields, "priority");
    REQUIRE(priority != nullptr);
    REQUIRE(priority->AllowedValueOptions.size() == 2);
    CHECK(priority->AllowedValueOptions[0].Value == "High");
    // BuildSimpleCatalogOptions re-classifies after filling options, so even
    // the unscoped path (which returns before Phase 4) upgrades priority from
    // its parse-time Text to SelectSingle.
    CHECK(priority->Family == TrackerFieldFamily::SelectSingle);

    const TrackerField* status = FindField(fields, "status");
    REQUIRE(status != nullptr);
    CHECK(status->Family == TrackerFieldFamily::Status); // id-keyed — immune to the ordering quirk
    CHECK(status->AllowedValueOptions.size() == 2);

    const TrackerField* issueType = FindField(fields, "issuetype");
    REQUIRE(issueType != nullptr);
    CHECK(issueType->Family == TrackerFieldFamily::IssueType); // explicitly re-classified after enrichment
    CHECK(issueType->AllowedValueOptions.size() == 2);
}

TEST_CASE("FetchFieldCatalog unscoped — customfield_* arrays classify per schema") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), std::string(), fields, components, issueTypeMeta, error));

    const TrackerField* users = FindField(fields, "customfield_10030");
    REQUIRE(users != nullptr);
    CHECK(users->IsUserType);
    CHECK(users->Family == TrackerFieldFamily::UserMulti); // array-of-user classifies without options

    const TrackerField* number = FindField(fields, "customfield_10020");
    REQUIRE(number != nullptr);
    CHECK(number->Family == TrackerFieldFamily::Number);

    const TrackerField* multi = FindField(fields, "customfield_10010");
    REQUIRE(multi != nullptr);
    CHECK(multi->IsCustom);
    CHECK(multi->IsArray);
    // CHARACTERIZATION: an option-array custom field has no allowedValues until
    // a scoped createmeta fetch supplies them, and ClassifyTrackerFieldFamily
    // has no opts-independent case for items=="option" — so unscoped it lands
    // on Text (the same fixture-invisible degradation the P2 describes).
    CHECK(multi->AllowedValueOptions.empty());
    CHECK(multi->Family == TrackerFieldFamily::Text);
}

TEST_CASE("FetchFieldCatalog scoped — per-project component options populated, merged and sorted") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    ScriptScopedEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, issueTypeMeta, error));

    const TrackerField* comp = FindField(fields, "components");
    REQUIRE(comp != nullptr);
    CHECK(comp->Family == TrackerFieldFamily::SelectMulti);

    // createmeta contributed "Audio" (30001); the project component endpoint
    // contributed "Renderer" (30002) + "audio tools" (30003). SortComponentCatalog
    // orders options case-insensitively by value.
    REQUIRE(comp->AllowedValueOptions.size() == 3);
    CHECK(comp->AllowedValueOptions[0].Value == "Audio");
    CHECK(comp->AllowedValueOptions[1].Value == "audio tools");
    CHECK(comp->AllowedValueOptions[2].Value == "Renderer");
    CHECK(comp->AllowedValues.size() == 3); // refreshed from options after the sort

    REQUIRE(components.size() == 3);
    CHECK(components[0].Name == "Audio");
    CHECK(components[1].Name == "audio tools");
    CHECK(components[2].Name == "Renderer");
}

TEST_CASE("FetchFieldCatalog scoped — createmeta allowedValues flip the custom multi-select to SelectMulti") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    ScriptScopedEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, issueTypeMeta, error));

    const TrackerField* multi = FindField(fields, "customfield_10010");
    REQUIRE(multi != nullptr);
    REQUIRE(multi->AllowedValueOptions.size() == 2);
    CHECK(multi->AllowedValueOptions[0].Value == "Red");
    CHECK(multi->AllowedValueOptions[1].Value == "Blue");
    CHECK(multi->Family == TrackerFieldFamily::SelectMulti);

    // The scoped path's Phase-4 re-classification keeps priority SelectSingle.
    const TrackerField* priority = FindField(fields, "priority");
    REQUIRE(priority != nullptr);
    CHECK(priority->Family == TrackerFieldFamily::SelectSingle);
}

TEST_CASE("FetchFieldCatalog scoped — issue-type meta harvests required fields; project issue types preferred") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    ScriptScopedEndpoints(fx);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, issueTypeMeta, error));

    REQUIRE(issueTypeMeta.size() == 1);
    CHECK(issueTypeMeta[0].ProjectKey == "BLU");
    CHECK(issueTypeMeta[0].IssueTypeName == "Task");
    CHECK(issueTypeMeta[0].RequiredFieldIds.count("summary") == 1);
    CHECK(issueTypeMeta[0].RequiredFieldIds.count("components") == 0);

    // GET /project/BLU returns the FULL issue-type list (real ids for PUT) —
    // preferred over createmeta's per-screen single type.
    const TrackerField* issueType = FindField(fields, "issuetype");
    REQUIRE(issueType != nullptr);
    REQUIRE(issueType->AllowedValueOptions.size() == 2);
    CHECK(issueType->AllowedValueOptions[0].Value == "Bug"); // sorted by value
    CHECK(issueType->AllowedValueOptions[1].Value == "Task");
    CHECK(issueType->AllowedValueOptions[0].Id == "10003");
}

TEST_CASE("FetchFieldCatalog scoped — component endpoint paginates until isLast") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    fx.ScriptJson("/rest/api/3/issue/createmeta", ScopedCreateMeta());
    fx.ScriptJson(
        "/rest/api/3/project/BLU",
        nlohmann::json{{"key", "BLU"}, {"issueTypes", nlohmann::json::array({{{"id", "10002"}, {"name", "Task"}}})}});
    fx.ScriptHandler("/rest/api/3/project/BLU/component", [](const httplib::Request& req) {
        const std::string startAt = req.get_param_value("startAt");
        if (startAt == "0") {
            return nlohmann::json{{"isLast", false},
                                  {"values", nlohmann::json::array({{{"id", "40001"}, {"name", "PageOne"}}})}};
        }
        return nlohmann::json{{"isLast", true},
                              {"values", nlohmann::json::array({{{"id", "40002"}, {"name", "PageTwo"}}})}};
    });

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, issueTypeMeta, error));

    CHECK(fx.RequestCount("/rest/api/3/project/BLU/component") == 2);
    const TrackerField* comp = FindField(fields, "components");
    REQUIRE(comp != nullptr);
    bool sawPageOne = false;
    bool sawPageTwo = false;
    for (const auto& opt : comp->AllowedValueOptions) {
        sawPageOne = sawPageOne || opt.Value == "PageOne";
        sawPageTwo = sawPageTwo || opt.Value == "PageTwo";
    }
    CHECK(sawPageOne);
    CHECK(sawPageTwo);
}

TEST_CASE("FetchFieldCatalog scoped — project fetch failure falls back to createmeta issue-type names") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    fx.ScriptJson("/rest/api/3/issue/createmeta", ScopedCreateMeta());
    fx.ScriptJson("/rest/api/3/project/BLU/component",
                  nlohmann::json{{"isLast", true}, {"values", nlohmann::json::array()}});
    fx.ScriptStatus("/rest/api/3/project/BLU", 500);
    // The global /issuetype enrichment must ALSO miss for the name-only fallback
    // to engage (it only fires when AllowedValueOptions is still empty).
    fx.ScriptStatus("/rest/api/3/issuetype", 500);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    REQUIRE(client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, issueTypeMeta, error));

    const TrackerField* issueType = FindField(fields, "issuetype");
    REQUIRE(issueType != nullptr);
    // Name-only fallback: option Id == name (loses real ids — last resort).
    REQUIRE(issueType->AllowedValueOptions.size() == 1);
    CHECK(issueType->AllowedValueOptions[0].Value == "Task");
    CHECK(issueType->AllowedValueOptions[0].Id == "Task");
}

TEST_CASE("FetchFieldCatalog — /field failure fails the whole build with an HTTP error") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/field", 503);

    JiraClient client;
    std::vector<TrackerField> fields;
    std::vector<TrackerComponent> components;
    std::vector<TrackerIssueTypeCreateMeta> issueTypeMeta;
    std::string error;
    const bool ok = client.FetchFieldCatalog(fx.Config(), std::string(), fields, components, issueTypeMeta, error);

    CHECK_FALSE(ok);
    CHECK(error.find("503") != std::string::npos);
    CHECK(fields.empty());
}

TEST_CASE("FetchFieldCatalog result overload — users fetched and injected into user-type fields") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    fx.ScriptJson("/rest/api/3/users/search",
                  nlohmann::json::array({{{"accountId", "u1"}, {"displayName", "Alice"}, {"active", true}},
                                         {{"accountId", "u2"}, {"displayName", "Bob"}, {"active", true}},
                                         {{"accountId", "u3"}, {"displayName", "Gone"}, {"active", false}}}));

    JiraClient client;
    auto catalogResult = client.FetchFieldCatalog(fx.Config(), std::string());
    REQUIRE(catalogResult);
    const TrackerFieldCatalogResult& catalog = catalogResult.value();

    CHECK(catalog.Warning.empty());
    REQUIRE(catalog.Users.size() == 2); // inactive user dropped

    const TrackerField* assignee = FindField(catalog.Fields, "assignee");
    REQUIRE(assignee != nullptr);
    CHECK(assignee->Family == TrackerFieldFamily::UserSingle);
    REQUIRE(assignee->AllowedValueOptions.size() == 2);
    CHECK(assignee->AllowedValueOptions[0].Id == "u1");
    CHECK(assignee->AllowedValueOptions[0].Value == "Alice");

    const TrackerField* reviewers = FindField(catalog.Fields, "customfield_10030");
    REQUIRE(reviewers != nullptr);
    CHECK(reviewers->AllowedValueOptions.size() == 2);
}

TEST_CASE("FetchFieldCatalog result overload — users fetch failure degrades to a warning, catalog still built") {
    JiraCatalogHttpFixture fx;
    fx.ScriptJson("/rest/api/3/field", BaselineFieldList());
    ScriptGlobalEnrichmentEndpoints(fx);
    fx.ScriptStatus("/rest/api/3/users/search", 403);

    JiraClient client;
    auto catalogResult = client.FetchFieldCatalog(fx.Config(), std::string());
    REQUIRE(catalogResult);
    const TrackerFieldCatalogResult& catalog = catalogResult.value();

    CHECK(!catalog.Warning.empty());
    CHECK(catalog.Users.empty());
    CHECK(FindField(catalog.Fields, "components") != nullptr);
}

TEST_CASE("FetchFieldCatalog result overload — /field failure returns an Err with a TrackerError") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/field", 500);

    JiraClient client;
    auto catalogResult = client.FetchFieldCatalog(fx.Config(), std::string());
    REQUIRE_FALSE(catalogResult);
    // The virtual unwraps the internal bool 5-vector helper, so the kind is the
    // deliberately-unclassified Unknown; the detail string is preserved from the
    // helper's HTTP-failure message (non-empty).
    CHECK(catalogResult.error().Kind == TrackerErrorKind::Unknown);
    CHECK_FALSE(catalogResult.error().Detail.empty());
}

TEST_CASE("FetchIssueEditMeta result overload — editmeta fields map into the Ok payload") {
    JiraCatalogHttpFixture fx;
    nlohmann::json editmeta;
    editmeta["fields"]["summary"]["operations"] = nlohmann::json::array({"set"});
    fx.ScriptJson("/rest/api/3/issue/TEST-1/editmeta", editmeta);

    JiraClient client;
    auto metaResult = client.FetchIssueEditMeta(fx.Config(), "TEST-1");
    REQUIRE(metaResult);
    CHECK(metaResult.value().count("summary") == 1);
}

TEST_CASE("FetchIssueEditMeta result overload — HTTP 404 returns an Err classified NotFound") {
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/TEST-1/editmeta", 404);

    JiraClient client;
    auto metaResult = client.FetchIssueEditMeta(fx.Config(), "TEST-1");
    REQUIRE_FALSE(metaResult);
    CHECK(metaResult.error().Kind == TrackerErrorKind::NotFound);
}

TEST_CASE("FetchIssueEditMeta result overload — 2xx-non-200 (202) yields Err with preserved Detail, not Ok") {
    // Regression: the !=200 failure branch fed TrackerErrorFromHttpStatus, which returns Ok()
    // (Kind==None, detail discarded) for any 2xx — producing a contradictory Err with empty Detail.
    JiraCatalogHttpFixture fx;
    fx.ScriptStatus("/rest/api/3/issue/TEST-1/editmeta", 202);

    JiraClient client;
    auto metaResult = client.FetchIssueEditMeta(fx.Config(), "TEST-1");
    REQUIRE_FALSE(metaResult);
    CHECK(metaResult.error().Kind != TrackerErrorKind::None);
    CHECK_FALSE(metaResult.error().Detail.empty());
    CHECK(metaResult.error().Detail.find("HTTP 202") != std::string::npos);
}
