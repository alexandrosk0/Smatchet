#include <doctest/doctest.h>

#include "TrackerFieldPayloadPure.h"
#include "TrackerFieldSchema.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using nlohmann::json;
using TrackerFieldPayloadPure::ExtractIssueKey;
using TrackerFieldPayloadPure::FieldUsesAdfDocument;
using TrackerFieldPayloadPure::IsSprintField;
using TrackerFieldPayloadPure::ResolveDisplayValueForSubmittedSelection;
using TrackerFieldPayloadPure::ResolveSprintIdForAgile;
using TrackerFieldPayloadPure::SplitCommaSeparatedValues;

namespace {

TrackerField MakeField(const std::string& id, const std::string& type = "string") {
    TrackerField f;
    f.Id = id;
    f.Type = type;
    return f;
}

TrackerFieldOption MakeOption(const std::string& id, const std::string& value,
                              const std::string& payloadJson = std::string()) {
    TrackerFieldOption o;
    o.Id = id;
    o.Value = value;
    o.PayloadJson = payloadJson;
    return o;
}

// #21: TrackerFieldPayloadPure::BuildValue now returns Result<json>. This shim unwraps
// it back into the prior (outValue, outError) -> bool shape so every behaviour-parity
// assertion below stays byte-for-byte identical to the pre-migration contract.
bool BuildValue(const TrackerField& field, const std::vector<std::string>& rawValues, json& outValue,
                std::string& outError) {
    Result<json> result = TrackerFieldPayloadPure::BuildValue(field, rawValues);
    if (!result.has_value()) {
        outError = result.error();
        return false;
    }
    outError.clear();
    outValue = std::move(result.value());
    return true;
}

} // namespace

TEST_CASE("BuildValue: string family — basic, empty, long") {
    TrackerField f = MakeField("summary", "string");
    json out;
    std::string err;

    SUBCASE("plain scalar passes through") {
        CHECK(BuildValue(f, {"hello world"}, out, err));
        CHECK(err.empty());
        CHECK(out.is_string());
        CHECK(out.get<std::string>() == "hello world");
    }

    SUBCASE("empty single value becomes JSON null (clear field)") {
        CHECK(BuildValue(f, {""}, out, err));
        CHECK(out.is_null());
    }

    SUBCASE("empty input vector becomes JSON null") {
        CHECK(BuildValue(f, {}, out, err));
        CHECK(out.is_null());
    }

    SUBCASE("very long string roundtrips byte-for-byte") {
        const std::string big(8192, 'x');
        CHECK(BuildValue(f, {big}, out, err));
        CHECK(out.is_string());
        CHECK(out.get<std::string>().size() == big.size());
    }
}

TEST_CASE("BuildValue: number family — integer, decimal, negative, malformed") {
    TrackerField f = MakeField("storyPoints", "number");
    json out;
    std::string err;

    SUBCASE("integer parses to double") {
        CHECK(BuildValue(f, {"42"}, out, err));
        CHECK(err.empty());
        CHECK(out.is_number());
        CHECK(out.get<double>() == doctest::Approx(42.0));
    }

    SUBCASE("decimal parses") {
        CHECK(BuildValue(f, {"3.14"}, out, err));
        CHECK(out.get<double>() == doctest::Approx(3.14));
    }

    SUBCASE("negative parses") {
        CHECK(BuildValue(f, {"-7.5"}, out, err));
        CHECK(out.get<double>() == doctest::Approx(-7.5));
    }

    SUBCASE("non-numeric returns false + error") {
        const bool ok = BuildValue(f, {"abc"}, out, err);
        CHECK_FALSE(ok);
        CHECK(err.find("Invalid numeric") != std::string::npos);
    }

    SUBCASE("trailing garbage returns false (parsedChars mismatch)") {
        const bool ok = BuildValue(f, {"12abc"}, out, err);
        CHECK_FALSE(ok);
    }

    SUBCASE("empty number value becomes JSON null") {
        CHECK(BuildValue(f, {""}, out, err));
        CHECK(out.is_null());
    }
}

TEST_CASE("BuildValue: option family — structured payload vs label fallback") {
    TrackerField f = MakeField("customfield_10010");
    f.Family = TrackerFieldFamily::SelectSingle;
    f.AllowedValueOptions.push_back(MakeOption("10001", "High", "{\"id\":\"10001\",\"value\":\"High\"}"));
    f.AllowedValueOptions.push_back(MakeOption("10002", "Low")); // no PayloadJson -> fallback path

    json out;
    std::string err;

    SUBCASE("id lookup returns structured {id} payload") {
        CHECK(BuildValue(f, {"10001"}, out, err));
        CHECK(out.is_object());
        REQUIRE(out.contains("id"));
        CHECK(out["id"].get<std::string>() == "10001");
    }

    SUBCASE("label lookup returns structured {id} payload") {
        CHECK(BuildValue(f, {"High"}, out, err));
        CHECK(out.is_object());
        REQUIRE(out.contains("id"));
        CHECK(out["id"].get<std::string>() == "10001");
    }

    SUBCASE("catalog miss falls back to {id: scalarValue}") {
        // Option in catalog (Low) has no PayloadJson, so falls back to selectable shape.
        CHECK(BuildValue(f, {"Low"}, out, err));
        CHECK(out.is_object());
        REQUIRE(out.contains("id"));
        CHECK(out["id"].get<std::string>() == "Low");
    }
}

TEST_CASE("BuildValue: depth-bomb PayloadJson / scalar is rejected by the bounded parse (no deep DOM)") {
    // The PayloadJson re-parse routes through json_safe::ParseBoundedOrDiscarded (depth cap 256):
    // a deeply-nested payload — e.g. planted via a tampered field-catalog cache — must degrade to
    // the not-structured fallback instead of building a deep DOM whose recursive ~json teardown
    // can overflow the stack. Same contract as the DetectFieldFamily fix.
    std::string bomb;
    for (int i = 0; i < 300; ++i) bomb += "[";
    for (int i = 0; i < 300; ++i) bomb += "]";

    SUBCASE("option family: over-deep PayloadJson falls back to {id: scalar}") {
        TrackerField f = MakeField("customfield_10010");
        f.Family = TrackerFieldFamily::SelectSingle;
        f.AllowedValueOptions.push_back(MakeOption("10001", "High", bomb));
        json out;
        std::string err;
        CHECK(BuildValue(f, {"High"}, out, err));
        REQUIRE(out.is_object());
        REQUIRE(out.contains("id"));
        // Bounded parse rejects the bomb -> not-structured fallback keeps the raw label,
        // instead of the pre-fix behaviour of adopting the 300-deep parsed payload.
        CHECK(out["id"].get<std::string>() == "High");
    }

    SUBCASE("user family: over-deep pre-encoded JSON scalar degrades to raw {accountId}") {
        std::string deepObj = "{\"accountId\":\"x\",\"pad\":";
        for (int i = 0; i < 300; ++i) deepObj += "{\"a\":";
        deepObj += "1";
        for (int i = 0; i < 300; ++i) deepObj += "}";
        deepObj += "}";
        TrackerField f = MakeField("assignee");
        f.IsUserType = true;
        json out;
        std::string err;
        CHECK(BuildValue(f, {deepObj}, out, err));
        REQUIRE(out.is_object());
        // The bounded parse rejects the bomb, so the accountId embedded past the depth cap is
        // NOT extracted; the scalar passes through the raw-string tail path unchanged.
        REQUIRE(out.contains("accountId"));
        CHECK(out["accountId"].get<std::string>() == deepObj);
    }
}

TEST_CASE("BuildValue: multi-option / array family — collect + skip empties") {
    TrackerField f = MakeField("customfield_components");
    f.IsArray = true;
    f.ItemsType = "option";
    f.AllowedValueOptions.push_back(MakeOption("1", "Alpha", "{\"id\":\"1\"}"));
    f.AllowedValueOptions.push_back(MakeOption("2", "Beta", "{\"id\":\"2\"}"));

    json out;
    std::string err;

    SUBCASE("two-value array builds two object payloads") {
        CHECK(BuildValue(f, {"1", "2"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 2);
        CHECK(out[0]["id"].get<std::string>() == "1");
        CHECK(out[1]["id"].get<std::string>() == "2");
    }

    SUBCASE("empty strings filtered before build") {
        CHECK(BuildValue(f, {"", "1", ""}, out, err));
        REQUIRE(out.is_array());
        CHECK(out.size() == 1);
    }

    SUBCASE("empty array input stays an empty array (no null collapse for arrays)") {
        CHECK(BuildValue(f, {}, out, err));
        REQUIRE(out.is_array());
        CHECK(out.empty());
    }
}

TEST_CASE("BuildValue: id/name collision serializes the id-keyed option, not a value collision") {
    // BLUUP ground truth: a component literally NAMED "10033" carries Id "10067"; another component
    // (TestComponent1) carries Id "10033". A single-pass `Id == v || Value == v` would match the
    // first option's Value=="10033" and serialize the WRONG id ("10067"). The id-first two-pass in
    // FindOptionByIdOrValue must pick the option whose Id is "10033" → TestComponent1.
    TrackerField f = MakeField("components");
    f.IsArray = true;
    f.ItemsType = "component";
    f.AllowedValueOptions.push_back(MakeOption("10067", "10033", "{\"id\":\"10067\"}"));
    f.AllowedValueOptions.push_back(MakeOption("10033", "TestComponent1", "{\"id\":\"10033\"}"));

    json out;
    std::string err;
    CHECK(BuildValue(f, {"10033"}, out, err));
    REQUIRE(out.is_array());
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].is_object());
    REQUIRE(out[0].contains("id"));
    CHECK(out[0]["id"].get<std::string>() == "10033"); // TestComponent1, NOT the Value-collision "10067"
}

TEST_CASE("BuildValue: labels family — comma-split + token sanitize + dedupe-of-empty") {
    TrackerField f = MakeField("labels");
    f.Family = TrackerFieldFamily::Labels;

    json out;
    std::string err;

    SUBCASE("comma-separated input splits to array of strings") {
        CHECK(BuildValue(f, {"alpha,beta,gamma"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 3);
        CHECK(out[0].get<std::string>() == "alpha");
        CHECK(out[2].get<std::string>() == "gamma");
    }

    SUBCASE("whitespace in token gets dash-normalised") {
        CHECK(BuildValue(f, {"with space"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 1);
        CHECK(out[0].get<std::string>() == "with-space");
    }

    SUBCASE("empty labels collapse to JSON null") {
        CHECK(BuildValue(f, {",,"}, out, err));
        CHECK(out.is_null());
    }
}

TEST_CASE("BuildValue: user family — account-id, displayName lookup, raw string") {
    TrackerField f = MakeField("assignee");
    f.IsUserType = true;
    f.AllowedValueOptions.push_back(MakeOption("abc123", "Alice Adams"));
    json out;
    std::string err;

    SUBCASE("raw account-id passes through as {accountId}") {
        CHECK(BuildValue(f, {"abc123"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out.contains("accountId"));
        CHECK(out["accountId"].get<std::string>() == "abc123");
    }

    SUBCASE("displayName resolves to catalog accountId via lookup") {
        CHECK(BuildValue(f, {"Alice Adams"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["accountId"].get<std::string>() == "abc123");
    }

    SUBCASE("pre-encoded JSON with accountId reduces to minimal {accountId}") {
        CHECK(BuildValue(f, {"{\"accountId\":\"xyz999\",\"displayName\":\"X\"}"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["accountId"].get<std::string>() == "xyz999");
        // displayName must be stripped — Jira rejects extra keys on some endpoints.
        CHECK_FALSE(out.contains("displayName"));
    }

    SUBCASE("empty user value becomes JSON null") {
        CHECK(BuildValue(f, {""}, out, err));
        CHECK(out.is_null());
    }
}

TEST_CASE("BuildValue: cascading-select — parent+child, parent-only, missing parent") {
    TrackerField f = MakeField("customfield_cascade");
    f.Family = TrackerFieldFamily::CascadingSelect;
    TrackerFieldOption parent = MakeOption("p1", "Hardware", "{\"id\":\"p1\"}");
    parent.Children.push_back(MakeOption("c1", "Mouse", "{\"id\":\"c1\"}"));
    parent.Children.push_back(MakeOption("c2", "Keyboard", "{\"id\":\"c2\"}"));
    f.AllowedValueOptions.push_back(parent);

    json out;
    std::string err;

    SUBCASE("parent + child encoded with \\x1f separator nests under \"child\"") {
        const std::string encoded = std::string("p1") + '\x1f' + "c1";
        CHECK(BuildValue(f, {encoded}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "p1");
        REQUIRE(out.contains("child"));
        CHECK(out["child"]["id"].get<std::string>() == "c1");
    }

    SUBCASE("parent-only does NOT emit \"child\" key") {
        CHECK(BuildValue(f, {"p1"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "p1");
        CHECK_FALSE(out.contains("child"));
    }

    SUBCASE("unknown parent falls back to selectable scalar (catalog miss)") {
        CHECK(BuildValue(f, {"unknown_parent"}, out, err));
        // Family is CascadingSelect -> on miss BuildValue uses FallbackPayloadForSelectableField
        // which for non-User / non-Status / non-IssueType / non-component with allowed-options
        // returns {id: scalar}. Verify we got *something* sensible and didn't crash.
        CHECK((out.is_object() || out.is_string()));
    }
}

TEST_CASE("BuildValue: parent / project — issue-key extraction and project key") {
    json out;
    std::string err;

    SUBCASE("parent field with raw issue key wraps as {key}") {
        TrackerField f = MakeField("parent");
        CHECK(BuildValue(f, {"PROJ-123"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["key"].get<std::string>() == "PROJ-123");
    }

    SUBCASE("parent field with key + summary suffix extracts key") {
        TrackerField f = MakeField("parent");
        CHECK(BuildValue(f, {"PROJ-7 - Some summary"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["key"].get<std::string>() == "PROJ-7");
    }

    SUBCASE("parent field with non-key string passes through as JSON string") {
        TrackerField f = MakeField("parent");
        CHECK(BuildValue(f, {"not-a-key"}, out, err));
        CHECK(out.is_string());
    }

    SUBCASE("project field wraps as {key}") {
        TrackerField f = MakeField("project");
        CHECK(BuildValue(f, {"PROJ"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["key"].get<std::string>() == "PROJ");
    }
}

TEST_CASE("BuildValue: priority / version / resolution / group — structured shape") {
    json out;
    std::string err;

    SUBCASE("priority with digits-only uses {id}") {
        TrackerField f = MakeField("priority", "priority");
        CHECK(BuildValue(f, {"3"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "3");
    }

    SUBCASE("priority with non-digits uses {name}") {
        TrackerField f = MakeField("priority", "priority");
        CHECK(BuildValue(f, {"High"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "High");
    }

    SUBCASE("version uses {name}") {
        TrackerField f = MakeField("fixVersion", "version");
        CHECK(BuildValue(f, {"v1.2.3"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "v1.2.3");
    }

    SUBCASE("group uses {name}") {
        TrackerField f = MakeField("group", "group");
        CHECK(BuildValue(f, {"developers"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "developers");
    }
}

TEST_CASE("BuildValue: per-builder dispatch table — one group per scalar field type") {
    json out;
    std::string err;

    SUBCASE("resolution (no catalog) uses {name}") {
        TrackerField f = MakeField("resolution", "resolution");
        REQUIRE(BuildValue(f, {"Fixed"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "Fixed");
    }

    SUBCASE("version (no catalog) uses {name}") {
        TrackerField f = MakeField("fixVersion", "version");
        REQUIRE(BuildValue(f, {"v2"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "v2");
    }

    SUBCASE("securitylevel digits-only uses {id}") {
        TrackerField f = MakeField("security", "securitylevel");
        REQUIRE(BuildValue(f, {"42"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "42");
    }

    SUBCASE("securitylevel non-digits uses {name}") {
        TrackerField f = MakeField("security", "securitylevel");
        REQUIRE(BuildValue(f, {"Internal"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["name"].get<std::string>() == "Internal");
    }

    SUBCASE("priority digits-only (no catalog) uses {id}") {
        TrackerField f = MakeField("priority", "priority");
        REQUIRE(BuildValue(f, {"3"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "3");
    }

    SUBCASE("catalogued field with allowed-options is claimed by option-branch first") {
        // Allowed-options precedence: the option/component branch produces {id: scalarValue}
        // BEFORE the priority/version/resolution type branches are ever reached. This mirrors
        // the original tower's top-to-bottom ordering exactly.
        TrackerField f = MakeField("priority", "priority");
        f.AllowedValueOptions.push_back(MakeOption("3", "Medium", "{\"id\":\"3\"}"));
        REQUIRE(BuildValue(f, {"Medium"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "Medium");
    }

    SUBCASE("ADF field (description) emits doc payload from markdown") {
        TrackerField f = MakeField("description", "string");
        REQUIRE(BuildValue(f, {"Hello"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["type"].get<std::string>() == "doc");
    }

    SUBCASE("date field formats valid date") {
        TrackerField f = MakeField("duedate", "date");
        REQUIRE(BuildValue(f, {"2026-05-31"}, out, err));
        CHECK(err.empty());
        CHECK(out.is_string());
        CHECK(out.get<std::string>().find("2026-05-31") != std::string::npos);
    }

    SUBCASE("date field rejects invalid value with error") {
        TrackerField f = MakeField("duedate", "date");
        CHECK_FALSE(BuildValue(f, {"not-a-date"}, out, err));
        CHECK(err.find("Invalid date/datetime") != std::string::npos);
    }

    SUBCASE("option-type without family uses {id} passthrough") {
        TrackerField f = MakeField("customfield_opt", "option");
        REQUIRE(BuildValue(f, {"7"}, out, err));
        REQUIRE(out.is_object());
        CHECK(out["id"].get<std::string>() == "7");
    }

    SUBCASE("unknown plain type falls through to raw string") {
        TrackerField f = MakeField("customfield_misc", "weirdtype");
        REQUIRE(BuildValue(f, {"raw value"}, out, err));
        CHECK(out.is_string());
        CHECK(out.get<std::string>() == "raw value");
    }
}

TEST_CASE("BuildValue: array path — user, structured-multi, plain string items") {
    json out;
    std::string err;

    SUBCASE("array of user values builds accountId objects, skipping empties") {
        TrackerField f = MakeField("multiUser");
        f.IsArray = true;
        f.IsUserType = true;
        REQUIRE(BuildValue(f, {"acc1", "", "acc2"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 2);
        CHECK(out[0]["accountId"].get<std::string>() == "acc1");
        CHECK(out[1]["accountId"].get<std::string>() == "acc2");
    }

    SUBCASE("array structured-multi family resolves catalogued option payloads") {
        TrackerField f = MakeField("multiSelect");
        f.IsArray = true;
        f.Family = TrackerFieldFamily::StructuredMulti;
        f.AllowedValueOptions.push_back(MakeOption("a", "Alpha", "{\"id\":\"a\"}"));
        REQUIRE(BuildValue(f, {"Alpha"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 1);
        CHECK(out[0]["id"].get<std::string>() == "a");
    }

    SUBCASE("array plain (no options) pushes raw strings") {
        TrackerField f = MakeField("multiText");
        f.IsArray = true;
        REQUIRE(BuildValue(f, {"x", "y"}, out, err));
        REQUIRE(out.is_array());
        REQUIRE(out.size() == 2);
        CHECK(out[0].get<std::string>() == "x");
        CHECK(out[1].get<std::string>() == "y");
    }
}

TEST_CASE("BuildValue: Result API — Ok carries the built json, Err carries the message") {
    SUBCASE("valid scalar yields Ok with the value") {
        TrackerField f = MakeField("summary", "string");
        Result<json> r = TrackerFieldPayloadPure::BuildValue(f, {"hello"});
        REQUIRE(r.has_value());
        CHECK(r.value().get<std::string>() == "hello");
    }

    SUBCASE("invalid number yields Err with the message") {
        TrackerField f = MakeField("storyPoints", "number");
        Result<json> r = TrackerFieldPayloadPure::BuildValue(f, {"abc"});
        REQUIRE_FALSE(r.has_value());
        CHECK(r.error().find("Invalid numeric") != std::string::npos);
    }
}

TEST_CASE("ExtractIssueKey: shape detection") {
    CHECK(ExtractIssueKey("PROJ-1") == "PROJ-1");
    CHECK(ExtractIssueKey("ABC-42 - summary") == "ABC-42");
    CHECK(ExtractIssueKey("  WS_2-9  ") == "WS_2-9"); // underscore + digits allowed in prefix
    CHECK(ExtractIssueKey("not-a-key") == "");
    CHECK(ExtractIssueKey("lowercase-1") == ""); // prefix must be upper/digit/underscore
    CHECK(ExtractIssueKey("") == "");
    CHECK(ExtractIssueKey("PROJ-") == "");   // suffix must be non-empty digits
    CHECK(ExtractIssueKey("PROJ-1a") == ""); // suffix must be all digits
}

TEST_CASE("SplitCommaSeparatedValues: tokeniser") {
    SUBCASE("basic split") {
        const auto r = SplitCommaSeparatedValues("a,b,c");
        REQUIRE(r.size() == 3);
        CHECK(r[0] == "a");
        CHECK(r[2] == "c");
    }
    SUBCASE("trims whitespace + drops empty segments") {
        const auto r = SplitCommaSeparatedValues(" a , ,b ,, c ");
        REQUIRE(r.size() == 3);
        CHECK(r[0] == "a");
        CHECK(r[1] == "b");
        CHECK(r[2] == "c");
    }
    SUBCASE("empty input yields empty vector") {
        CHECK(SplitCommaSeparatedValues("").empty());
        CHECK(SplitCommaSeparatedValues(", , ,").empty());
    }
}

TEST_CASE("ResolveSprintIdForAgile: catalog lookup + digit passthrough") {
    TrackerField f;
    f.AllowedValueOptions.push_back(MakeOption("999", "Sprint 1"));

    CHECK(ResolveSprintIdForAgile(f, "Sprint 1") == "999"); // label resolves
    CHECK(ResolveSprintIdForAgile(f, "999") == "999");      // id resolves
    CHECK(ResolveSprintIdForAgile(f, "12345") == "12345");  // digits-only passthrough
    CHECK(ResolveSprintIdForAgile(f, "unknown") == "");     // miss + non-digits -> empty
    CHECK(ResolveSprintIdForAgile(f, "") == "");
}

TEST_CASE("IsSprintField: family vs schema-custom heuristic") {
    TrackerField f;
    CHECK_FALSE(IsSprintField(f));
    f.Family = TrackerFieldFamily::Sprint;
    CHECK(IsSprintField(f));

    TrackerField g;
    g.SchemaCustom = "com.pyxis.greenhopper.jira:gh-sprint";
    CHECK(IsSprintField(g));
}

TEST_CASE("FieldUsesAdfDocument: id, type, schema-custom signals") {
    TrackerField f;

    SUBCASE("description by id") {
        f.Id = "description";
        CHECK(FieldUsesAdfDocument(f));
    }

    SUBCASE("environment by id") {
        f.Id = "environment";
        CHECK(FieldUsesAdfDocument(f));
    }

    SUBCASE("type doc") {
        f.Id = "customfield_x";
        f.Type = "doc";
        CHECK(FieldUsesAdfDocument(f));
    }

    SUBCASE("schema-custom textarea") {
        f.Id = "customfield_y";
        f.SchemaCustom = "com.atlassian.jira.plugin.system.customfieldtypes:textarea";
        CHECK(FieldUsesAdfDocument(f));
    }

    SUBCASE("plain string field with nothing matching") {
        f.Id = "summary";
        f.Type = "string";
        CHECK_FALSE(FieldUsesAdfDocument(f));
    }
}

TEST_CASE("ResolveDisplayValueForSubmittedSelection: label resolution") {
    TrackerField f;
    f.AllowedValueOptions.push_back(MakeOption("10", "Bug"));

    SUBCASE("value id resolves to display label") { CHECK(ResolveDisplayValueForSubmittedSelection(f, "10") == "Bug"); }

    SUBCASE("unknown value passes through unchanged") {
        CHECK(ResolveDisplayValueForSubmittedSelection(f, "unknown") == "unknown");
    }

    SUBCASE("cascading parent + child formats as 'parent > child'") {
        TrackerField c;
        c.Family = TrackerFieldFamily::CascadingSelect;
        TrackerFieldOption p = MakeOption("p1", "Hardware");
        p.Children.push_back(MakeOption("c1", "Mouse"));
        c.AllowedValueOptions.push_back(p);
        const std::string encoded = std::string("p1") + '\x1f' + "c1";
        CHECK(ResolveDisplayValueForSubmittedSelection(c, encoded) == "Hardware > Mouse");
    }

    SUBCASE("cascading parent-only formats as 'parent' only") {
        TrackerField c;
        c.Family = TrackerFieldFamily::CascadingSelect;
        c.AllowedValueOptions.push_back(MakeOption("p1", "Hardware"));
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "p1") == "Hardware");
    }

    // BLUUP-1 ground truth: a per-project option set with a real id/name collision — components
    // are literally NAMED "10033"/"10034", and other options carry those strings as their Id.
    SUBCASE("id-preferred resolution returns id-keyed option when another option's Value collides") {
        TrackerField c;
        c.AllowedValueOptions.push_back(MakeOption("10067", "10033"));
        c.AllowedValueOptions.push_back(MakeOption("10069", "10034"));
        c.AllowedValueOptions.push_back(MakeOption("10070", "10067"));
        c.AllowedValueOptions.push_back(MakeOption("10033", "TestComponent1"));
        c.AllowedValueOptions.push_back(MakeOption("10034", "TestComponent2"));
        // value "10033" must resolve via Id==10033 (TestComponent1), not the earlier Value=="10033" option.
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "10033") == "TestComponent1");
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "10034") == "TestComponent2");
    }

    SUBCASE("multi-value array splits, resolves each, re-joins (BLUUP components)") {
        TrackerField c;
        c.IsArray = true;
        c.Family = TrackerFieldFamily::SelectMulti;
        c.AllowedValueOptions.push_back(MakeOption("10067", "10033"));
        c.AllowedValueOptions.push_back(MakeOption("10069", "10034"));
        c.AllowedValueOptions.push_back(MakeOption("10070", "10067"));
        c.AllowedValueOptions.push_back(MakeOption("10033", "TestComponent1"));
        c.AllowedValueOptions.push_back(MakeOption("10034", "TestComponent2"));
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "10033, 10034") == "TestComponent1, TestComponent2");
    }

    SUBCASE("non-colliding single value still resolves (regression guard)") {
        TrackerField c;
        c.AllowedValueOptions.push_back(MakeOption("10", "Bug"));
        c.AllowedValueOptions.push_back(MakeOption("20", "Task"));
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "20") == "Task");
        // resolve by Value too, when no Id collision exists
        CHECK(ResolveDisplayValueForSubmittedSelection(c, "Bug") == "Bug");
    }
}

TEST_CASE("AdfCommentBodyFromMarkdown: plain text becomes a single paragraph") {
    const json doc = TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown("hello world");
    CHECK(doc["type"] == "doc");
    CHECK(doc["version"] == 1);
    REQUIRE(doc["content"].is_array());
    REQUIRE(doc["content"].size() == 1);
    CHECK(doc["content"][0]["type"] == "paragraph");
    CHECK(doc["content"][0]["content"][0]["text"] == "hello world");
}

TEST_CASE("AdfCommentBodyFromMarkdown: Markdown formatting is preserved (fidelity vs plain paragraph)") {
    const json doc = TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown("**bold** and *em*");
    const json& marks = doc["content"][0]["content"][0]["marks"];
    REQUIRE(marks.is_array());
    CHECK(marks[0]["type"] == "strong");

    const json list = TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown("- a\n- b");
    CHECK(list["content"][0]["type"] == "bulletList");
}

TEST_CASE("AdfCommentBodyFromMarkdown: empty / whitespace input yields a valid non-empty ADF doc") {
    // Jira rejects an ADF doc with empty content — the helper must always emit at least one node.
    for (const std::string& in : {std::string(""), std::string("   "), std::string("\n\t ")}) {
        const json doc = TrackerFieldPayloadPure::AdfCommentBodyFromMarkdown(in);
        CHECK(doc["type"] == "doc");
        REQUIRE(doc["content"].is_array());
        REQUIRE(doc["content"].size() >= 1);
        CHECK(doc["content"][0]["type"] == "paragraph");
    }
}
