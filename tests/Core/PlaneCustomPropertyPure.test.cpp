#include <doctest/doctest.h>

#include "PlaneCustomPropertyPure.h"

#include <string>
#include <unordered_map>
#include <vector>

// BACKLOG_CODE_REVIEW.md §C4 — custom (UUID) Plane properties used to be silently
// dropped by BuildCreatePayload. These cases pin the per-family typing of the new
// `properties.<uuid>` serializer: what is emitted, what is skipped, and which inputs
// abort the payload build with a validation error instead of losing data quietly.

using smatchet::plane::BuildPlaneCustomProperties;

namespace {

constexpr const char* kTextUuid = "aaaaaaaa-1111-2222-3333-000000000001";
constexpr const char* kNumberUuid = "aaaaaaaa-1111-2222-3333-000000000002";
constexpr const char* kSelectUuid = "aaaaaaaa-1111-2222-3333-000000000003";
constexpr const char* kMultiUuid = "aaaaaaaa-1111-2222-3333-000000000004";
constexpr const char* kUserUuid = "aaaaaaaa-1111-2222-3333-000000000005";
constexpr const char* kBoolUuid = "aaaaaaaa-1111-2222-3333-000000000006";
constexpr const char* kReadOnlyUuid = "aaaaaaaa-1111-2222-3333-000000000007";

TrackerField MakeCustom(const char* id, TrackerFieldFamily family, const char* type) {
    TrackerField f;
    f.Id = id;
    f.Name = std::string("Custom ") + id;
    f.Family = family;
    f.Type = type;
    f.IsCustom = true;
    return f;
}

std::vector<TrackerField> DefaultCatalog() {
    std::vector<TrackerField> catalog;

    TrackerField summary;
    summary.Id = "summary";
    summary.Family = TrackerFieldFamily::Text;
    summary.Type = "string";
    catalog.push_back(summary); // built-in: never emitted by the customs serializer

    catalog.push_back(MakeCustom(kTextUuid, TrackerFieldFamily::Text, "string"));
    catalog.push_back(MakeCustom(kNumberUuid, TrackerFieldFamily::Number, "number"));

    TrackerField select = MakeCustom(kSelectUuid, TrackerFieldFamily::SelectSingle, "option");
    TrackerFieldOption red;
    red.Id = "opt-red";
    red.Value = "Red";
    select.AllowedValueOptions.push_back(red);
    catalog.push_back(select);

    TrackerField multi = MakeCustom(kMultiUuid, TrackerFieldFamily::SelectMulti, "array");
    multi.IsArray = true;
    TrackerFieldOption a;
    a.Id = "opt-a";
    a.Value = "Alpha";
    multi.AllowedValueOptions.push_back(a);
    TrackerFieldOption b;
    b.Id = "opt-b";
    b.Value = "Beta";
    multi.AllowedValueOptions.push_back(b);
    catalog.push_back(multi);

    TrackerField user = MakeCustom(kUserUuid, TrackerFieldFamily::UserSingle, "user");
    user.IsUserType = true;
    TrackerFieldOption alice;
    alice.Id = "member-uuid-1";
    alice.Value = "Alice Doe";
    user.AllowedValueOptions.push_back(alice);
    catalog.push_back(user);

    catalog.push_back(MakeCustom(kBoolUuid, TrackerFieldFamily::Text, "boolean"));

    TrackerField ro = MakeCustom(kReadOnlyUuid, TrackerFieldFamily::Text, "string");
    ro.ReadOnly = true;
    catalog.push_back(ro);

    return catalog;
}

} // namespace

TEST_CASE("customs are emitted under their UUID; built-ins and unknown ids are not") {
    std::unordered_map<std::string, std::string> values;
    values["summary"] = "a core field";           // built-in — the core payload owns it
    values["not-in-catalog"] = "stray";           // unknown id — nothing to type it with
    values[kTextUuid] = "hello";

    const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
    REQUIRE(result.has_value());
    const auto& props = result.value();
    CHECK(props.size() == 1);
    CHECK(props.at(kTextUuid) == "hello");
}

TEST_CASE("empty values, read-only customs, and an empty map emit nothing") {
    {
        const auto result = BuildPlaneCustomProperties({}, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().empty());
    }
    {
        std::unordered_map<std::string, std::string> values;
        values[kTextUuid] = "";           // cleared cell
        values[kReadOnlyUuid] = "locked"; // server-owned — writing it would fail the mutation
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().empty());
    }
}

TEST_CASE("NUMBER parses to a JSON number; a non-numeric value aborts with the field name") {
    {
        std::unordered_map<std::string, std::string> values;
        values[kNumberUuid] = "42.5";
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().at(kNumberUuid).is_number());
        CHECK(result.value().at(kNumberUuid).get<double>() == doctest::Approx(42.5));
    }
    {
        std::unordered_map<std::string, std::string> values;
        values[kNumberUuid] = "forty-two";
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().find("is not a number") != std::string::npos);
        // The error names the field the user has to fix, not the raw UUID alone.
        CHECK(result.error().find(kNumberUuid) != std::string::npos);
    }
}

TEST_CASE("boolean accepts true/false spellings and rejects anything else") {
    std::unordered_map<std::string, std::string> values;
    for (const char* truthy : {"true", "True", "1", "yes"}) {
        values[kBoolUuid] = truthy;
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().at(kBoolUuid) == true);
    }
    for (const char* falsy : {"false", "FALSE", "0", "no"}) {
        values[kBoolUuid] = falsy;
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().at(kBoolUuid) == false);
    }
    values[kBoolUuid] = "maybe";
    const auto rejected = BuildPlaneCustomProperties(values, DefaultCatalog());
    REQUIRE_FALSE(rejected.has_value());
    CHECK(rejected.error().find("is not a boolean") != std::string::npos);
}

TEST_CASE("selects resolve display labels to option ids; unknown values pass through raw") {
    std::unordered_map<std::string, std::string> values;
    // Grid cells store display labels — "Red" must come out as the option id.
    values[kSelectUuid] = "Red";
    values[kUserUuid] = "Alice Doe";
    {
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().at(kSelectUuid) == "opt-red");
        CHECK(result.value().at(kUserUuid) == "member-uuid-1");
    }
    // A value the cached catalog doesn't know yet passes through for the server to judge.
    values.clear();
    values[kSelectUuid] = "Chartreuse";
    {
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().at(kSelectUuid) == "Chartreuse");
    }
}

TEST_CASE("multi-select splits CSV, resolves each token, and skips an all-empty list") {
    std::unordered_map<std::string, std::string> values;
    values[kMultiUuid] = "Alpha, opt-b, Gamma";
    {
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        const auto& arr = result.value().at(kMultiUuid);
        REQUIRE(arr.is_array());
        REQUIRE(arr.size() == 3);
        CHECK(arr[0] == "opt-a");    // label → id
        CHECK(arr[1] == "opt-b");    // id stays id
        CHECK(arr[2] == "Gamma");    // unknown passes through
    }
    values[kMultiUuid] = " , ,"; // separators only — nothing to send
    {
        const auto result = BuildPlaneCustomProperties(values, DefaultCatalog());
        REQUIRE(result.has_value());
        CHECK(result.value().empty());
    }
}
