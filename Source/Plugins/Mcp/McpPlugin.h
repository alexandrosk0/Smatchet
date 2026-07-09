#pragma once

#include "IPlugin.h"
#include "McpAuthTokenPure.h"
#include "SmatchetDefaults.h"
#include "McpServerStatus.h"
#include <memory>
#include <nlohmann/json_fwd.hpp>
#include <string>

namespace httplib {
class Request;
class Response;
} // namespace httplib

class McpPlugin : public IPlugin {
  public:
    explicit McpPlugin(int port = SmatchetDefaults::Mcp::kDefaultPort);
    ~McpPlugin() override;

    const char* Id() const override { return "mcp"; }
    void OnStart(AppController& app) override;
    // cppcheck-suppress virtualCallInConstructor // invoked from ~McpPlugin for non-dynamic teardown
    void OnStop() override;

    bool NeedsRestart(const TrackerConfig& cfg) const override;
    bool TryGetMcpStatusSnapshot(McpServerStatus& out) const override {
        out = GetStatus();
        return true;
    }

    McpServerStatus GetStatus() const;
    /** Compare bound auth token to current config (for restart-on-change). */
    bool AuthTokenMatches(const std::string& cfgToken) const;
    /** Compare run_lua exposure toggle to current config (for restart-on-change). */
    bool LuaExecutionEnabledMatches(bool enabled) const;

  private:
    /// Shared authorization gate for every MCP route. Returns true when the
    /// request may proceed; on rejection it has already populated `res` with the
    /// appropriate 401/403 status and body. Must be called first in each handler.
    bool Authorize(const httplib::Request& req, httplib::Response& res);

    /// OnStart registration phases. Each installs a disjoint set of routes on
    /// `impl_->svr`; OnStart calls them in declaration order, preserving the
    /// original route-registration order (route precedence is order-sensitive).
    void RegisterTicketRoutes();   ///< /mcp/list_tickets, /mcp/attachment_proxy, /mcp/search
    void RegisterToolsListRoute(); ///< GET /mcp/tools/list
    void RegisterToolsCallRoute(); ///< POST /mcp/tools/call (REST)
    void RegisterSseRoute();       ///< GET <kSsePath> (SSE event stream)
    void RegisterJsonRpcRoutes();  ///< POST /mcp/messages + POST <kSsePath>
    void StartServerThread();      ///< bind + listen on the worker thread

    /// Per-route REST handlers, extracted from the registration phases above so
    /// each registrar stays a thin sequence of route installs under the size
    /// cap. Each is the verbatim lambda body the registrar used to inline;
    /// behaviour and wire shape are byte-for-byte identical.
    void HandleListTickets(const httplib::Request& req, httplib::Response& res);
    void HandleAttachmentProxy(const httplib::Request& req, httplib::Response& res);
    void HandleSearchTickets(const httplib::Request& req, httplib::Response& res);
    void HandleToolsCall(const httplib::Request& req, httplib::Response& res);

    /// Emit the REST `tools/call` success/error envelope once the dispatch arm
    /// has produced either `result` or `error`. Extracted from HandleToolsCall's
    /// tail so that handler stays under the line cap; wire shape and status-code
    /// selection are byte-for-byte identical to the inlined form.
    void EmitToolsCallResult(httplib::Response& res, const std::string& name, const std::string& remote,
                             const nlohmann::json& arguments, const std::string& error, const std::string& result);

    /// tools/call token-bucket gate (AGENTIC_INFRA_AUDIT.md B3), shared by the REST and
    /// JSON-RPC entry points. Consumes one token; on deny returns false and sets
    /// `retryAfterMs` to the minimum wait until a token refills.
    bool AllowToolsCall(long long& retryAfterMs);

    /// REST-side wrapper around AllowToolsCall: on deny populates `res` with the
    /// canonical HTTP-200 rate-limited envelope and returns false.
    bool RestToolsCallWithinRateLimit(const std::string& name, const std::string& remote, httplib::Response& res);

    /// REST `tools/call` registry-hit arm: dispatch a registered command and emit the canonical
    /// envelope. Extracted from HandleToolsCall; wire shape byte-for-byte identical.
    void DispatchRegistryToolsCall(const std::string& name, const nlohmann::json& arguments, const std::string& remote,
                                   httplib::Response& res);

    /// REST `tools/call` unknown-name arm: dispatch through the registry to produce a canonical
    /// unknown-command envelope with fuzzy suggestions. Wire shape byte-for-byte identical.
    void EmitUnknownToolsCallEnvelope(const std::string& name, const nlohmann::json& arguments,
                                      const std::string& remote, httplib::Response& res);

    /// Handle a JSON-RPC `tools/call` request. Populates `jres["result"]` or
    /// `jres["error"]` (the caller already seeded jres with jsonrpc/id). Split
    /// out of RegisterJsonRpcRoutes to keep that registration phase under the
    /// branch cap; behaviour is byte-for-byte identical to the inlined form.
    void HandleJsonRpcToolsCall(const std::string& remote, const nlohmann::json& params, nlohmann::json& jres);

    /// JSON-RPC `tools/call` registry-hit arm: dispatch a registered command into `jres`.
    void HandleJsonRpcRegistryCall(const std::string& name, const nlohmann::json& params, const std::string& rpcRemote,
                                   nlohmann::json& jres);

    /// JSON-RPC `tools/call` `run_lua` arm: execute (or reject) a Lua snippet/script into `jres`.
    void HandleJsonRpcRunLua(const nlohmann::json& params, const std::string& rpcRemote, nlohmann::json& jres);

    int port_;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
