// PR A6 (function-size-compliance) — pure-logic doctest for the Jira editmeta
// field-editability decision extracted from JiraUserAndMeta.cpp::FetchIssueEditMeta.
// No HTTP, no cpr, no JiraClient instance. Goldens capture the pre-extraction
// behaviour byte-for-byte. Mirrors JiraIssueMappingPure.test.cpp in shape.

#include "JiraEditMetaPure.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <string>

using smatchet::jira::JiraEditMetaFieldCanEdit;

namespace {

nlohmann::json MetaWithOps(nlohmann::json operations) {
    nlohmann::json meta;
    meta["operations"] = std::move(operations);
    return meta;
}

nlohmann::json MetaWithSchemaType(const std::string& type) {
    nlohmann::json meta;
    meta["schema"]["type"] = type;
    return meta;
}

} // namespace

TEST_CASE("JiraEditMetaFieldCanEdit: string operations set/add/remove are editable") {
    CHECK(JiraEditMetaFieldCanEdit("summary", MetaWithOps(nlohmann::json::array({"set"}))) == true);
    CHECK(JiraEditMetaFieldCanEdit("labels", MetaWithOps(nlohmann::json::array({"add", "remove"}))) == true);
    CHECK(JiraEditMetaFieldCanEdit("labels", MetaWithOps(nlohmann::json::array({"remove"}))) == true);
}

TEST_CASE("JiraEditMetaFieldCanEdit: operations are case-insensitive") {
    CHECK(JiraEditMetaFieldCanEdit("summary", MetaWithOps(nlohmann::json::array({"SET"}))) == true);
    CHECK(JiraEditMetaFieldCanEdit("summary", MetaWithOps(nlohmann::json::array({"Add"}))) == true);
}

TEST_CASE("JiraEditMetaFieldCanEdit: unknown / empty operations are not editable") {
    CHECK(JiraEditMetaFieldCanEdit("summary", MetaWithOps(nlohmann::json::array({"edit"}))) == false);
    // Explicit empty operations array ≡ not editable (and is NOT treated as date-schema fallback).
    nlohmann::json emptyOps = MetaWithOps(nlohmann::json::array());
    emptyOps["schema"]["type"] = "date";
    CHECK(JiraEditMetaFieldCanEdit("duedate", emptyOps) == false);
}

TEST_CASE("JiraEditMetaFieldCanEdit: object operations via operation/name/type keys") {
    nlohmann::json opObj;
    opObj["operation"] = "set";
    CHECK(JiraEditMetaFieldCanEdit("foo", MetaWithOps(nlohmann::json::array({opObj}))) == true);

    nlohmann::json byName;
    byName["name"] = "add";
    CHECK(JiraEditMetaFieldCanEdit("foo", MetaWithOps(nlohmann::json::array({byName}))) == true);

    nlohmann::json byType;
    byType["type"] = "remove";
    CHECK(JiraEditMetaFieldCanEdit("foo", MetaWithOps(nlohmann::json::array({byType}))) == true);

    nlohmann::json other;
    other["operation"] = "edit";
    CHECK(JiraEditMetaFieldCanEdit("foo", MetaWithOps(nlohmann::json::array({other}))) == false);
}

TEST_CASE("JiraEditMetaFieldCanEdit: missing operations + date schema is editable") {
    CHECK(JiraEditMetaFieldCanEdit("duedate", MetaWithSchemaType("date")) == true);
    CHECK(JiraEditMetaFieldCanEdit("startdate", MetaWithSchemaType("datetime")) == true);
    CHECK(JiraEditMetaFieldCanEdit("startdate", MetaWithSchemaType("DateTime")) == true);
}

TEST_CASE("JiraEditMetaFieldCanEdit: schema-only date denylist stays non-editable") {
    CHECK(JiraEditMetaFieldCanEdit("created", MetaWithSchemaType("datetime")) == false);
    CHECK(JiraEditMetaFieldCanEdit("updated", MetaWithSchemaType("datetime")) == false);
    CHECK(JiraEditMetaFieldCanEdit("resolutiondate", MetaWithSchemaType("date")) == false);
}

TEST_CASE("JiraEditMetaFieldCanEdit: non-date schema without operations is not editable") {
    CHECK(JiraEditMetaFieldCanEdit("summary", MetaWithSchemaType("string")) == false);
}

TEST_CASE("JiraEditMetaFieldCanEdit: non-object meta is not editable") {
    CHECK(JiraEditMetaFieldCanEdit("foo", nlohmann::json("not-an-object")) == false);
    CHECK(JiraEditMetaFieldCanEdit("foo", nlohmann::json::object()) == false);
}
