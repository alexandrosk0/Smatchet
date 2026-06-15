// FakeLuaBindingHost -- header-only RAII fake implementing `ILuaBindingHost` for
// the doctest rig. Backs each `ILuaBindingHost` virtual with a knob (controlled
// return value) + a recording field (test asserts on the call's effect). Lives
// in `tests/support/` so any test TU under `tests/Lua/` that links
// `AppController_LuaBindingsCore.cpp` can drive `smatchet::lua::InitLuaCore`
// without touching ImGui / GLFW / cpr / SQLite -- the Class-C prerequisite that
// PR #144 lifted (see `_plan-locks` § Phase-6-unblocker).
//
// Lifecycle: one fixture per `TEST_CASE`. The fake holds no static state, so
// adjacent cases never poison each other. `LuaCommands()` returns a static
// empty `CommandRegistry` -- tests that need a populated registry instantiate
// their own and override the method in a per-case subclass.
//
// `AppForCommandContext()` returns `nullptr` per the `ILuaBindingHost.h`
// contract for test fakes (command handlers exercised here ignore `ctx.App`).

#pragma once

// sol2 + Lua 5.3-as-C++ ordering: <limits> + <cstdint> MUST precede
// <sol/sol.hpp> (transitively via ILuaBindingHost.h) so GCC 13+ resolves
// `std::numeric_limits<std::size_t>::max` against the correct specialisation
// under `-mcmodel=large`. Mirrors `AppController.h:5-6` and `LuaHostFixture.h:32`.
#include <limits>
#include <cstdint>

#include "CachedTicketTypes.h"
#include "Commands/CommandRegistry.h"
#include "ILuaBindingHost.h"
#include "TrackerFieldSchema.h"

#include <sol/sol.hpp>

#include <nlohmann/json.hpp>

#include <exception>
#include <functional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace smatchet_tests {

// Mirrors AppController_LuaBindings.cpp's depth/node caps + bounded SAX handler
// (security finding A). Kept byte-identical in intent so the fake's decode_json
// rejects the same hostile payloads production does. If the production caps move,
// move these too.
constexpr int kDecodeJsonMaxDepth = 256;
constexpr std::size_t kDecodeJsonMaxNodes = 200000u;

class BoundedDecodeSax : public nlohmann::detail::json_sax_dom_parser<nlohmann::json> {
  public:
    using base = nlohmann::detail::json_sax_dom_parser<nlohmann::json>;
    explicit BoundedDecodeSax(nlohmann::json& root) : base(root, /*allow_exceptions=*/false) {}

    bool null() { return Count() && base::null(); }
    bool boolean(bool v) { return Count() && base::boolean(v); }
    bool number_integer(nlohmann::json::number_integer_t v) { return Count() && base::number_integer(v); }
    bool number_unsigned(nlohmann::json::number_unsigned_t v) { return Count() && base::number_unsigned(v); }
    bool number_float(nlohmann::json::number_float_t v, const nlohmann::json::string_t& s) {
        return Count() && base::number_float(v, s);
    }
    bool string(nlohmann::json::string_t& v) { return Count() && base::string(v); }
    bool binary(nlohmann::json::binary_t& v) { return Count() && base::binary(v); }

    bool start_object(std::size_t elements) {
        if (!Count() || !Descend()) {
            return false;
        }
        return base::start_object(elements);
    }
    bool end_object() {
        --depth_;
        return base::end_object();
    }
    bool start_array(std::size_t elements) {
        if (!Count() || !Descend()) {
            return false;
        }
        return base::start_array(elements);
    }
    bool end_array() {
        --depth_;
        return base::end_array();
    }
    bool key(nlohmann::json::string_t& v) { return base::key(v); }

    bool Overflowed() const { return overflowed_; }

  private:
    bool Count() {
        if (++nodes_ > kDecodeJsonMaxNodes) {
            overflowed_ = true;
            return false;
        }
        return true;
    }
    bool Descend() {
        if (depth_ >= kDecodeJsonMaxDepth) {
            overflowed_ = true;
            return false;
        }
        ++depth_;
        return true;
    }

    int depth_ = 0;
    std::size_t nodes_ = 0;
    bool overflowed_ = false;
};

class FakeLuaBindingHost : public ILuaBindingHost {
  public:
    // ----- Observable fields (tests CHECK against these) -----

    /// Every `LuaLogInfoBind` call appends its message here.
    std::vector<std::string> LoggedInfo;

    /// Every `LuaMcpRegisterToolBind` call appends a record here. `Callback`
    /// is the raw sol::function -- tests can `.call()` it if they want to
    /// drive the tool-handler round-trip, otherwise asserting `.size()` is
    /// enough to prove the registration path fired.
    struct McpRegistration {
        sol::table ToolDef;
        sol::function Callback;
    };
    std::vector<McpRegistration> McpRegistrations;

    /// Every `SubmitFieldEdit` call records {IssueId, FieldId, Values} so
    /// tests can assert "Ticket:set_field forwarded `priority=High` exactly
    /// once".
    struct FieldEditCall {
        std::string IssueId;
        std::string FieldId;
        std::vector<std::string> Values;
    };
    std::vector<FieldEditCall> SubmitFieldEditCalls;

    /// Every `LuaCreateIssueBind` call records the inbound spec keys (string
    /// keys only -- enough to prove the marshal path delivered the table).
    std::vector<std::vector<std::string>> CreateIssueSpecKeys;

    // ----- Knobs (tests set these BEFORE driving the script) -----

    /// `LuaGetTicketBind` resolves the key via this map. Missing key -> returns
    /// {sol::nil, "not found"}.
    std::unordered_map<std::string, CachedTicket> TicketsById;

    /// `LuaGetActiveTicketsBind` returns this verbatim.
    std::vector<CachedTicket> ActiveTickets;

    /// `FindFieldById` resolves via this map. Missing -> nullptr.
    std::unordered_map<std::string, TrackerField> FieldsById;

    /// `SubmitFieldEdit` returns this verbatim. Tests can override per-case.
    bool SubmitFieldEditReturn = true;
    /// `SubmitFieldEdit` sets `outError` to this when it returns. Default empty.
    std::string SubmitFieldEditError;

    /// `LuaCreateIssueBind` populates the returned sol::object with this table
    /// (a sol::nil-equivalent when the test wants the failure path) and the
    /// trailing error string with `CreateIssueErr`. Defaults make the call
    /// return {<empty table>, ""} which `LuaTrackerCreateIssueGlue` reads as
    /// "create_issue returned no result".
    /// Tests script the success path via:
    ///   fake.CreateIssueScripter = [](sol::state_view sv) {
    ///       sol::table t = sv.create_table();
    ///       t["ok"] = true; t["issue_key"] = "ABC-99"; return t;
    ///   };
    std::function<sol::table(sol::state_view)> CreateIssueScripter;
    std::string CreateIssueErr;

    /// `LuaDecodeJsonBind` -- when set, the fake invokes this. Default
    /// implementation parses with nlohmann::json (test default mirrors
    /// production behaviour cheaply, so most tests don't need to override).
    /// Empty function pointer -> default behaviour.
    std::function<std::tuple<sol::object, std::string>(sol::state_view, const std::string&)> DecodeJsonImpl;

    // ----- ILuaBindingHost overrides -----

    void LuaLogInfoBind(const std::string& msg) override { LoggedInfo.push_back(msg); }

    std::tuple<sol::object, std::string> LuaGetTicketBind(sol::state_view sv, const std::string& issueId) override {
        auto it = TicketsById.find(issueId);
        if (it == TicketsById.end()) {
            return std::make_tuple(sol::make_object(sv, sol::nil), std::string("not found"));
        }
        return std::make_tuple(sol::make_object(sv, it->second), std::string());
    }

    std::vector<CachedTicket> LuaGetActiveTicketsBind() override { return ActiveTickets; }

    std::tuple<sol::object, std::string> LuaDecodeJsonBind(sol::state_view sv, const std::string& s) override {
        if (DecodeJsonImpl) {
            return DecodeJsonImpl(sv, s);
        }
        // Mirror the production AppController::Impl::LuaDecodeJsonBind depth/node
        // bound (security finding A): the ImGui-coupled production sink is not
        // linked into SmatchetLuaTests, so the fake reproduces the bounded parse
        // here to keep `decode_json` faithful and let the doctest assert the cap.
        constexpr size_t kMaxDecodeBytes = 4u * 1024u * 1024u;
        if (s.size() > kMaxDecodeBytes) {
            return std::make_tuple(sol::make_object(sv, sol::nil), std::string("input too large"));
        }
        try {
            nlohmann::json j;
            BoundedDecodeSax sax(j);
            const bool ok = nlohmann::json::sax_parse(s, &sax, nlohmann::json::input_format_t::json,
                                                      /*strict=*/true, /*ignore_comments=*/false);
            if (!ok) {
                if (sax.Overflowed()) {
                    return std::make_tuple(sol::make_object(sv, sol::nil),
                                           std::string("input too deeply nested or too many elements"));
                }
                return std::make_tuple(sol::make_object(sv, sol::nil), std::string("invalid JSON"));
            }
            sol::object obj = JsonToSolObject(sv, j);
            return std::make_tuple(obj, std::string());
        } catch (const std::exception& e) {
            return std::make_tuple(sol::make_object(sv, sol::nil), std::string(e.what()));
        }
    }

    const TrackerField* FindFieldById(const std::string& fieldId) const override {
        auto it = FieldsById.find(fieldId);
        return (it != FieldsById.end()) ? &it->second : nullptr;
    }

    bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                         const std::vector<std::string>& rawValues, std::string& outError) override {
        FieldEditCall rec;
        rec.IssueId = issueId;
        rec.FieldId = field.Id;
        rec.Values = rawValues;
        SubmitFieldEditCalls.push_back(std::move(rec));
        outError = SubmitFieldEditError;
        return SubmitFieldEditReturn;
    }

    std::tuple<sol::object, std::string> LuaCreateIssueBind(sol::state_view sv, sol::table spec) override {
        // Record the inbound spec keys so tests can prove the marshal happened.
        std::vector<std::string> keys;
        spec.for_each([&](sol::object k, sol::object /*v*/) {
            if (k.is<std::string>())
                keys.push_back(k.as<std::string>());
        });
        CreateIssueSpecKeys.push_back(std::move(keys));

        if (CreateIssueScripter) {
            sol::table t = CreateIssueScripter(sv);
            return std::make_tuple(sol::object(t), CreateIssueErr);
        }
        // Default: sol::nil -> `LuaTrackerCreateIssueGlue` reads as
        // "create_issue returned no result". Tests asserting the success path
        // must set `CreateIssueScripter`.
        return std::make_tuple(sol::make_object(sv, sol::nil), CreateIssueErr);
    }

    void LuaMcpRegisterToolBind(sol::table toolDef, sol::function callback) override {
        McpRegistrations.push_back({std::move(toolDef), std::move(callback)});
    }

    /// `GetLuaMcpTools` snapshot (added in #19c). Defaults to empty; tests that exercise the
    /// MCP tool-listing path populate `McpTools` directly.
    std::vector<smatchet::lua::McpToolDefinition> McpTools;
    std::vector<smatchet::lua::McpToolDefinition> GetLuaMcpTools() const override { return McpTools; }

    smatchet::cmd::CommandRegistry& LuaCommands() override { return Commands_; }

    AppController* AppForCommandContext() override { return nullptr; }

    // ----- Helpers tests may want -----

    /// The sol::state the fake will marshal CachedTicket through. Set by the
    /// test after calling `smatchet::lua::InitLuaCore(state, &fake)` -- the
    /// fake needs a `state_view` to construct sol::object instances inside
    /// the bind callbacks. Stored as raw `lua_State*` to avoid sol2
    /// constructor ambiguity.
    void AttachState(sol::state& state) { lua_state_ = state.lua_state(); }

    /// Direct access to the embedded `CommandRegistry`. Tests register
    /// commands here BEFORE driving the script when they want
    /// `commands.invoke` to succeed.
    smatchet::cmd::CommandRegistry& Commands() { return Commands_; }

  private:
    smatchet::cmd::CommandRegistry Commands_;
    lua_State* lua_state_ = nullptr;

    sol::state_view GetStateView() const { return sol::state_view(lua_state_); }

    static sol::object JsonToSolObject(sol::state_view sv, const nlohmann::json& j) {
        if (j.is_null())
            return sol::make_object(sv, sol::nil);
        if (j.is_boolean())
            return sol::make_object(sv, j.get<bool>());
        if (j.is_number_integer())
            return sol::make_object(sv, static_cast<double>(j.get<std::int64_t>()));
        if (j.is_number_unsigned())
            return sol::make_object(sv, static_cast<double>(j.get<std::uint64_t>()));
        if (j.is_number_float())
            return sol::make_object(sv, j.get<double>());
        if (j.is_string())
            return sol::make_object(sv, j.get<std::string>());
        if (j.is_array()) {
            sol::table arr = sv.create_table();
            std::size_t i = 1;
            for (const auto& el : j)
                arr[i++] = JsonToSolObject(sv, el);
            return arr;
        }
        if (j.is_object()) {
            sol::table tbl = sv.create_table();
            for (auto it = j.begin(); it != j.end(); ++it)
                tbl[it.key()] = JsonToSolObject(sv, it.value());
            return tbl;
        }
        return sol::make_object(sv, sol::nil);
    }
};

} // namespace smatchet_tests
