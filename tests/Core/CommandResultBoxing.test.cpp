// CommandResultBoxing — unit gate for the std::shared_ptr<nlohmann::json> boxing
// of Command.h's three by-value json members (ParamSpec::Default,
// CommandError::Details, CommandResult::Data). The boxing let Command.h drop the
// full <nlohmann/json.hpp> for <nlohmann/json_fwd.hpp> (debt
// json-fwd-swap-by-value-carriers). This pins that the factories box correctly,
// that every reader null-guards the now-nullable members (a default-constructed
// member is a NULL POINTER, not a json null), and that the value types stay
// copyable (the registry's All() returns vector<Command> by value).
//
// See Source/Core/include/Commands/Command.h + Source/Core/src/Commands/Command.cpp.

#include <doctest/doctest.h>

#include "Commands/Command.h"

#include <memory>

#include <nlohmann/json.hpp>

using smatchet::cmd::Command;
using smatchet::cmd::CommandError;
using smatchet::cmd::CommandResult;
using smatchet::cmd::ErrorCode;
using smatchet::cmd::ParamSpec;
using smatchet::cmd::ParamType;

TEST_CASE("CommandResult::Success boxes Data and ToWireJson round-trips it") {
    CommandResult r = CommandResult::Success(nlohmann::json{{"hello", "world"}});
    REQUIRE(r.Ok == true);
    REQUIRE(r.Data != nullptr); // factory boxed it
    CHECK((*r.Data)["hello"] == "world");
    const nlohmann::json wire = r.ToWireJson("demo.cmd");
    CHECK(wire["ok"] == true);
    CHECK(wire["command"] == "demo.cmd");
    CHECK(wire["data"]["hello"] == "world");
}

TEST_CASE("ToWireJson: an Ok result with unset Data emits an empty object (null-ptr guard)") {
    // A handler that returns Ok without Success() leaves Data a null shared_ptr;
    // ToWireJson must treat that as {} rather than dereferencing null.
    CommandResult r;
    r.Ok = true; // Data left default-constructed (null pointer)
    REQUIRE(r.Data == nullptr);
    const nlohmann::json wire = r.ToWireJson("demo.cmd");
    CHECK(wire["ok"] == true);
    CHECK(wire["data"].is_object());
    CHECK(wire["data"].empty());
}

TEST_CASE("CommandError::ToJson includes details only when the boxed member is non-null") {
    CommandError e;
    e.Code = ErrorCode::ValidationError;
    e.Message = "bad arg";
    REQUIRE(e.Details == nullptr);                  // default: no structured detail
    CHECK(e.ToJson().contains("details") == false); // null-ptr guard omits it

    e.Details = std::make_shared<nlohmann::json>(nlohmann::json{{"param", "limit"}});
    const nlohmann::json j = e.ToJson();
    REQUIRE(j.contains("details"));
    CHECK(j["details"]["param"] == "limit");
}

TEST_CASE("Command::BuildJsonSchema emits a default only for params with a boxed Default") {
    Command c;
    c.Name = "demo.cmd";
    ParamSpec withDefault;
    withDefault.Name = "limit";
    withDefault.Type = ParamType::Int;
    withDefault.Default = std::make_shared<nlohmann::json>(50);
    ParamSpec noDefault;
    noDefault.Name = "query";
    noDefault.Type = ParamType::String; // Default left a null pointer
    c.Params = {withDefault, noDefault};

    const nlohmann::json schema = c.BuildJsonSchema();
    REQUIRE(schema["properties"].contains("limit"));
    REQUIRE(schema["properties"].contains("query"));
    CHECK(schema["properties"]["limit"]["default"] == 50);
    CHECK(schema["properties"]["query"].contains("default") == false); // null-ptr guard
}

TEST_CASE("CommandResult stays copyable and shares the boxed Data (shared_ptr semantics)") {
    // Command/CommandResult must remain copyable — the registry's All() returns
    // vector<Command> by value and the palette copies it. shared_ptr boxing keeps
    // the compiler-generated copy; the boxed json is shared, which is safe because
    // Data is write-once / read-only after construction.
    CommandResult a = CommandResult::Success(nlohmann::json{{"n", 1}});
    CommandResult b = a; // copy
    REQUIRE(b.Data != nullptr);
    CHECK((*b.Data)["n"] == 1);
    CHECK(a.Data == b.Data); // shared control block — copy is shallow by design
}
