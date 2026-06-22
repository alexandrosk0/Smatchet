#include <doctest/doctest.h>

#include "Tracker/PlaneProjectScope.h"

#include <nlohmann/json.hpp>

#include <string>

// Coverage ramp (global-floor 65->70) — PlaneProjectScope::SetProjectInQuery is the
// pure Plane structured-query project-scoping helper and had no dedicated test. It
// injects/replaces "project_id" in a JSON query blob (or synthesises one), returning
// compact JSON. Pure: nlohmann only, no cpr/sqlite. PlaneProjectScope.cpp linked here.

using PlaneProjectScope::SetProjectInQuery;

TEST_CASE("SetProjectInQuery: empty projectId is a no-op (returns the input verbatim)") {
    CHECK(SetProjectInQuery("", "") == "");
    CHECK(SetProjectInQuery("{\"state\":\"open\"}", "") == "{\"state\":\"open\"}");
}

TEST_CASE("SetProjectInQuery: empty query synthesises a single-field object") {
    const std::string out = SetProjectInQuery("", "proj-xyz");
    const nlohmann::json j = nlohmann::json::parse(out);
    REQUIRE(j.is_object());
    CHECK(j.at("project_id").get<std::string>() == "proj-xyz");
    CHECK(j.size() == 1);
}

TEST_CASE("SetProjectInQuery: injects project_id into an existing object, preserving other keys") {
    const std::string out = SetProjectInQuery("{\"state\":\"open\",\"priority\":\"high\"}", "P1");
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j.at("project_id").get<std::string>() == "P1");
    CHECK(j.at("state").get<std::string>() == "open");
    CHECK(j.at("priority").get<std::string>() == "high");
}

TEST_CASE("SetProjectInQuery: replaces an existing project_id") {
    const std::string out = SetProjectInQuery("{\"project_id\":\"OLD\",\"state\":\"open\"}", "NEW");
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j.at("project_id").get<std::string>() == "NEW");
    CHECK(j.at("state").get<std::string>() == "open");
}

TEST_CASE("SetProjectInQuery: non-JSON input is discarded and replaced by a fresh object") {
    const std::string out = SetProjectInQuery("not json at all", "P9");
    const nlohmann::json j = nlohmann::json::parse(out);
    CHECK(j.at("project_id").get<std::string>() == "P9");
    CHECK(j.size() == 1);
}

TEST_CASE("SetProjectInQuery: a JSON non-object (array) is discarded and replaced") {
    const std::string out = SetProjectInQuery("[1,2,3]", "P7");
    const nlohmann::json j = nlohmann::json::parse(out);
    REQUIRE(j.is_object());
    CHECK(j.at("project_id").get<std::string>() == "P7");
    CHECK(j.size() == 1);
}
