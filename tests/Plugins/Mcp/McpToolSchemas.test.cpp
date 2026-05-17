// MCP tool-schema pure-logic tests — exercises the canonical run_lua tool-list entry
// shape. The schema is the wire contract between MCP clients and the server; structural
// drift here is a protocol break.

#include "McpJsonRpcPure.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <string>

using smatchet::mcp::pure::BuildRunLuaToolEntry;

TEST_CASE("BuildRunLuaToolEntry exposes name + description + inputSchema at top level"
          * doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp BuildRunLuaToolEntry ::structure path. Removing any of
    // the three top-level keys breaks the .at() lookups below, since the schema is the
    // wire-format contract published to MCP clients.
    const nlohmann::json entry = BuildRunLuaToolEntry();

    REQUIRE(entry.is_object());
    REQUIRE(entry.contains("name"));
    REQUIRE(entry.contains("description"));
    REQUIRE(entry.contains("inputSchema"));

    CHECK(entry.at("name").get<std::string>() == "run_lua");
    CHECK(entry.at("description").is_string());
    CHECK(entry.at("description").get<std::string>().size() > 0);
}

TEST_CASE("BuildRunLuaToolEntry inputSchema is an object-typed JSON Schema with required[mode]"
          * doctest::test_suite("[high-risk]")) {
    // Forces McpJsonRpcPure.cpp BuildRunLuaToolEntry ::"required":{"mode"} invariant.
    // The `mode` field is the single mandatory discriminator for snippet vs script;
    // dropping it from `required` would let clients omit `mode` and slip through.
    const nlohmann::json entry = BuildRunLuaToolEntry();
    const nlohmann::json& schema = entry.at("inputSchema");

    REQUIRE(schema.is_object());
    CHECK(schema.at("type").get<std::string>() == "object");

    REQUIRE(schema.contains("required"));
    const nlohmann::json& required = schema.at("required");
    REQUIRE(required.is_array());
    REQUIRE(required.size() == 1);
    CHECK(required.at(0).get<std::string>() == "mode");
}

TEST_CASE("BuildRunLuaToolEntry mode property is a string with enum [snippet, script]") {
    const nlohmann::json entry = BuildRunLuaToolEntry();
    const nlohmann::json& props = entry.at("inputSchema").at("properties");

    REQUIRE(props.contains("mode"));
    const nlohmann::json& mode = props.at("mode");
    CHECK(mode.at("type").get<std::string>() == "string");

    REQUIRE(mode.contains("enum"));
    const nlohmann::json& en = mode.at("enum");
    REQUIRE(en.is_array());
    REQUIRE(en.size() == 2);
    CHECK(en.at(0).get<std::string>() == "snippet");
    CHECK(en.at(1).get<std::string>() == "script");
}

TEST_CASE("BuildRunLuaToolEntry properties cover code, script, args sub-schemas") {
    const nlohmann::json entry = BuildRunLuaToolEntry();
    const nlohmann::json& props = entry.at("inputSchema").at("properties");

    REQUIRE(props.contains("code"));
    CHECK(props.at("code").at("type").get<std::string>() == "string");

    REQUIRE(props.contains("script"));
    CHECK(props.at("script").at("type").get<std::string>() == "string");

    REQUIRE(props.contains("args"));
    CHECK(props.at("args").at("type").get<std::string>() == "object");
}

TEST_CASE("BuildRunLuaToolEntry returns equal JSON across calls (pure / deterministic)") {
    const nlohmann::json a = BuildRunLuaToolEntry();
    const nlohmann::json b = BuildRunLuaToolEntry();
    CHECK(a == b);
}
