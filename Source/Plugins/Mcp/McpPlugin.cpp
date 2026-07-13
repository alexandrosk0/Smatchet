#include "McpPlugin.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#endif

#include "AppController.h"
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
#include "ILuaBindingHost.h" // GetLuaBindingHost()->GetLuaMcpTools() — sol2-free MCP tool snapshot (#19c)
#endif
#include "ConfigManager.h"
#include "Commands/Command.h"
#include "Json/BoundedJsonParse.h" // smatchet::json_safe::ParseBounded — depth/node-bounded ingress parse (#1271)
#include "McpJsonRpcPure.h"
#include "McpRateLimitPure.h"
#include <cstdio>  // std::remove, std::fopen/fwrite/fclose
#include <cstdlib> // std::getenv — spawn-token handshake (SMATCHET_MCP_SPAWN_TOKEN)
#if !defined(_WIN32)
#include <unistd.h> // getpid()
#endif
#include "Commands/CommandRegistry.h"
#include "Logger.h"
#include <httplib.h>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>

namespace {

using ::smatchet::mcp::pure::Base64Encode;
using ::smatchet::mcp::pure::BuildRunLuaSummary;
using ::smatchet::mcp::pure::BuildRunLuaToolEntry;
using ::smatchet::mcp::pure::BuildToolCallSummary;
using ::smatchet::mcp::pure::CanAcceptSseConnection;
using ::smatchet::mcp::pure::ConstantTimeStringEquals;
using ::smatchet::mcp::pure::ExtractHostFromUrl;
using ::smatchet::mcp::pure::ExtractJsonRpcErrorMessage;
using ::smatchet::mcp::pure::IsAllowedAttachmentHost;
using ::smatchet::mcp::pure::IsLoopbackAddress;
using ::smatchet::mcp::pure::IsMcpHostOriginAllowed;
using ::smatchet::mcp::pure::LooksLikeHttpUrl;
using ::smatchet::mcp::pure::NormalizeDomain;
using ::smatchet::mcp::pure::ResolveSseCorsOrigin;
using ::smatchet::mcp::pure::TruncateOneLine;
using ::smatchet::mcp::pure::UrlHasUserinfo;

void AppendMcpActivityLine(AppController* app, const std::string& line) {
    if (app != nullptr) {
        app->AppendMcpActivity(line);
    }
}

long long NowMonotonicMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

struct McpPlugin::Impl {
    AppController* app = nullptr;
    // Shutdown signaling for SSE chunked-content providers. Heartbeat lambdas
    // wait on shutdownCv with a 1s timeout instead of sleeping unconditionally
    // so OnStop() can reclaim worker threads within microseconds rather than
    // up to 1 second per connected client.
    // Declared BEFORE `svr` on purpose: members destroy in reverse order, and
    // `~Server` joins the worker pool in its destructor. If a worker is still
    // executing the chunked-content lambda when ~Impl begins (svr.stop() does
    // not synchronously drain in-flight handlers), it must read these primitives
    // through the raw `impl` pointer. Putting them before `svr` guarantees they
    // outlive `~Server`'s pool join.
    std::mutex shutdownMutex;
    std::condition_variable shutdownCv;
    std::atomic<bool> shuttingDown{false};
    // Concurrent-SSE bound (security synthesis #20). Each accepted SSE stream parks a worker
    // in the heartbeat wait-loop; the httplib pool is 8, so this caps how many of those
    // workers SSE may hold (CanAcceptSseConnection) and rejects the rest with 503 instead of
    // letting them silently queue forever and exhaust the pool. Incremented on admit,
    // decremented when the chunked-content provider returns (stream closes).
    std::atomic<int> activeSseConnections{0};
    httplib::Server svr;
    std::thread thread;
    bool routes_installed = false;
    std::string bind_host = SmatchetDefaults::Mcp::kBindLocalhost;
    std::string auth_token;
    // Spawn token adopted from SMATCHET_MCP_SPAWN_TOKEN on a prior OnStart (finding DR12a).
    // Retained because the env var is scrubbed after first adoption, so a restart re-adopts
    // this value instead of coming back tokenless and 401ing every parent request.
    std::string adopted_spawn_token;
    std::string tracker_domain;
    std::vector<std::string> export_fields;
    bool allow_lua_execution = false;
    bool require_token_on_loopback = true;
    // tools/call token bucket (AGENTIC_INFRA_AUDIT.md B3). Guarded by its own mutex —
    // REST and JSON-RPC handlers run on different pool workers. Config-seeded at OnStart.
    std::mutex toolsCallBucketMutex;
    smatchet::mcp::pure::McpToolsCallBucket toolsCallBucket;
    int toolsCallRateBurst = 0;
    int toolsCallRateRefillPerSec = 0;
    /// Path to the instance.json written by OnStart and deleted by OnStop.
    std::string instanceJsonPath;
};

McpPlugin::McpPlugin(int port) : port_(port), impl_(std::make_unique<Impl>()) {} // pimpl — Impl complete here

McpPlugin::~McpPlugin() {
    // cppcheck-suppress virtualCallInConstructor
    OnStop();
}

McpServerStatus McpPlugin::GetStatus() const {
    McpServerStatus s;
    if (!impl_) {
        return s;
    }
    s.PluginRegistered = true;
    s.ListenPort = port_;
    s.BindHost = impl_->bind_host;
    s.AuthRequired = !impl_->auth_token.empty();
    s.RoutesInstalled = impl_->routes_installed;
    s.ThreadJoinable = impl_->thread.joinable();
    s.ServerRunning = impl_->svr.is_running();
    return s;
}

bool McpPlugin::AuthTokenMatches(const std::string& cfgToken) const {
    if (!impl_) {
        return true;
    }
    return smatchet::mcp::McpAuthTokenSatisfiesConfig(impl_->auth_token, cfgToken, impl_->adopted_spawn_token);
}

bool McpPlugin::LuaExecutionEnabledMatches(const bool enabled) const {
    if (!impl_) {
        return true;
    }
    return impl_->allow_lua_execution == enabled;
}

bool McpPlugin::NeedsRestart(const TrackerConfig& cfg) const {
    const McpServerStatus st = GetStatus();
    const int expectedPort =
        (cfg.McpPort >= 1 && cfg.McpPort <= 65535) ? cfg.McpPort : SmatchetDefaults::Mcp::kDefaultPort;
    const std::string expectedBind =
        cfg.McpAllowRemote ? SmatchetDefaults::Mcp::kBindAny : SmatchetDefaults::Mcp::kBindLocalhost;
    return st.ListenPort != expectedPort || st.BindHost != expectedBind || !AuthTokenMatches(cfg.McpAuthToken) ||
           !LuaExecutionEnabledMatches(cfg.McpAllowLuaExecution) ||
           (impl_ && impl_->require_token_on_loopback != cfg.McpRequireTokenOnLoopback) ||
           (impl_ && (impl_->toolsCallRateBurst != cfg.McpToolsCallRateBurst ||
                      impl_->toolsCallRateRefillPerSec != cfg.McpToolsCallRateRefillPerSec));
}

bool McpPlugin::Authorize(const httplib::Request& req, httplib::Response& res) {
    if (impl_->app != nullptr) {
        impl_->app->NotifyMcpClientHttpActivity();
    }
    // DNS-rebinding defence (security synthesis #4). When bound to loopback, the
    // only legitimate callers are local MCP clients, which send a loopback-literal
    // Host (`127.0.0.1:<port>` / `localhost`) and no (or a loopback) Origin. A
    // malicious web page that rebinds DNS to 127.0.0.1 reaches this port with the
    // attacker's hostname in Host and the attacker's Origin -- both rejected here,
    // independent of the bearer token. Skipped when McpAllowRemote bound us to
    // 0.0.0.0, where a non-loopback Host is the operator's explicit intent.
    if (impl_->bind_host == SmatchetDefaults::Mcp::kBindLocalhost) {
        std::string hoReason;
        if (!IsMcpHostOriginAllowed(req.get_header_value("Host"), req.get_header_value("Origin"), hoReason)) {
            res.status = 403;
            res.set_content("MCP access denied: invalid Host/Origin.", "text/plain");
            LOG_WARN("MCP: auth denied remote=%s status=403 reason=%s", req.remote_addr.c_str(), hoReason.c_str());
            AppendMcpActivityLine(impl_->app, std::string("MCP: auth denied remote=") + req.remote_addr +
                                                  " status=403 reason=" + hoReason);
            return false;
        }
    }
    if (impl_->auth_token.empty()) {
        if (!IsLoopbackAddress(req.remote_addr)) {
            res.status = 403;
            res.set_content("MCP access denied: localhost only.", "text/plain");
            AppendMcpActivityLine(impl_->app, std::string("MCP: auth denied remote=") + req.remote_addr +
                                                  " status=403 reason=localhost_only");
            return false;
        }
        // Security hardening: a tokenless loopback server is reachable by ANY local
        // process. With McpRequireTokenOnLoopback ON (default), deny rather than admit
        // an unauthenticated local caller into the command registry. The operator opts
        // back into the legacy behavior by setting McpRequireTokenOnLoopback=false.
        if (impl_->require_token_on_loopback) {
            res.status = 401;
            res.set_header("WWW-Authenticate", "Token realm=\"Smatchet MCP\"");
            res.set_content("MCP requires an auth token. Set mcp_auth_token, or set "
                            "mcp_require_token_on_loopback=false to allow tokenless loopback access.",
                            "text/plain");
            AppendMcpActivityLine(impl_->app, std::string("MCP: auth denied remote=") + req.remote_addr +
                                                  " status=401 reason=token_required_on_loopback");
            return false;
        }
        return true;
    }
    const auto token = req.get_header_value("X-Smatchet-Token");
    if (!ConstantTimeStringEquals(token, impl_->auth_token)) {
        res.status = 401;
        res.set_header("WWW-Authenticate", "Token realm=\"Smatchet MCP\"");
        res.set_content("Missing or invalid MCP token.", "text/plain");
        AppendMcpActivityLine(impl_->app, std::string("MCP: auth denied remote=") + req.remote_addr +
                                              " status=401 reason=invalid_or_missing_token");
        return false;
    }
    return true;
}

void McpPlugin::OnStart(AppController& app) {
    if (!impl_) {
        return;
    }
    impl_->app = &app;
    // Reset shutdown signal in case this plugin instance is being restarted
    // after a prior OnStop()/OnStart() cycle.
    impl_->shuttingDown.store(false);
    const TrackerConfig cfg = ConfigManager::Load();
    impl_->bind_host = cfg.McpAllowRemote ? SmatchetDefaults::Mcp::kBindAny : SmatchetDefaults::Mcp::kBindLocalhost;
    // Spawn-handshake token adoption. An ephemeral instance launched by the CLI
    // --spawn parent has no persisted mcp_auth_token, yet McpRequireTokenOnLoopback
    // is ON by default -- so a tokenless loopback caller (the parent's probe) is
    // denied 401. The parent provisions a per-spawn random token and hands it to
    // this child via the inherited SMATCHET_MCP_SPAWN_TOKEN env var; adopt it as the
    // auth token so the parent's authenticated requests are accepted while EVERY
    // other local caller still needs the token (the secure default is preserved --
    // this strengthens the child, never weakens it). Only when no token is already
    // configured: never override an operator-set mcp_auth_token. The adopted token is retained
    // in impl_->adopted_spawn_token so a later restart (SyncMcpPluginWithConfig) re-adopts it
    // even though the env var below is scrubbed after the first read (finding DR12a) -- otherwise
    // the restart would come back tokenless under require_token_on_loopback and 401 every parent
    // request.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // getenv: cross-platform — _dupenv_s is MSVC-only
#endif
    const char* spawnTokenEnv = std::getenv("SMATCHET_MCP_SPAWN_TOKEN");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
    const std::string envSpawnToken = (spawnTokenEnv != nullptr) ? std::string(spawnTokenEnv) : std::string();
    const smatchet::mcp::McpAuthTokenPlan tokenPlan =
        smatchet::mcp::PlanMcpAuthToken(cfg.McpAuthToken, envSpawnToken, impl_->adopted_spawn_token);
    impl_->auth_token = tokenPlan.effectiveToken;
    impl_->adopted_spawn_token = tokenPlan.retainedSpawnToken;
    if (tokenPlan.scrubSpawnEnv) {
        // Scrub the secret from this process's environment now that it has been adopted and
        // retained: leaving it set would let any subprocess we later spawn inherit it, or any
        // same-user reader pull it from the process env block.
#if defined(_WIN32)
        _putenv_s("SMATCHET_MCP_SPAWN_TOKEN", ""); // empty value deletes the var on Windows
#else
        ::unsetenv("SMATCHET_MCP_SPAWN_TOKEN");
#endif
    }
    impl_->allow_lua_execution = cfg.McpAllowLuaExecution;
    impl_->require_token_on_loopback = cfg.McpRequireTokenOnLoopback;
    impl_->toolsCallRateBurst = cfg.McpToolsCallRateBurst;
    impl_->toolsCallRateRefillPerSec = cfg.McpToolsCallRateRefillPerSec;
    impl_->toolsCallBucket.Tokens = static_cast<double>(impl_->toolsCallRateBurst);
    impl_->toolsCallBucket.LastRefillMs = NowMonotonicMs();
    impl_->tracker_domain = NormalizeDomain(cfg.Domain);
    if (cfg.McpExportFields.empty()) {
        impl_->export_fields = {
            "summary", "status", "priority", "assignee", "updated", "created", "labels", "issuetype",
        };
    } else {
        impl_->export_fields = cfg.McpExportFields;
    }

    // Limit request body and slow-read abuse when exposed on LAN.
    // Write instance.json so CLI attach-mode can discover this MCP endpoint
    // without needing explicit --mcp-host/--mcp-port flags.
    // Format: { "pid": <int>, "port": <int> } — CLI verifies PID is still alive.
    {
        const std::string userDataDir = ConfigManager::GetUserDataDirectory();
        const std::string instancePath = userDataDir + "instance.json";
        try {
            nlohmann::json inst;
            inst["pid"] = static_cast<long long>(
#if defined(_WIN32)
                GetCurrentProcessId()
#else
                getpid()
#endif
            );
            inst["port"] = port_;
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4996) // fopen: cross-platform — fopen_s is MSVC-only
#endif
            std::FILE* f = std::fopen(instancePath.c_str(), "wb");
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            if (f) {
                const std::string s = inst.dump();
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
            }
        } catch (...) {
            // catch-all-ok: non-fatal — CLI can fall back to explicit flags.
            LOG_DEBUG("MCP: instance JSON write failed");
        }
        impl_->instanceJsonPath = instancePath;
    }

    impl_->svr.set_payload_max_length(1u * 1024u * 1024u);
    impl_->svr.set_read_timeout(10, 0);
    impl_->svr.set_write_timeout(30, 0);
    impl_->svr.set_idle_interval(1, 0);

    // Route registration is order-sensitive (cpp-httplib matches in install
    // order). These phases install disjoint route sets in the same order as the
    // original monolithic OnStart — do not reorder.
    if (!impl_->routes_installed) {
        impl_->routes_installed = true;
        RegisterTicketRoutes();
        RegisterToolsListRoute();
        RegisterToolsCallRoute();
        RegisterSseRoute();
        RegisterJsonRpcRoutes();
    }

    StartServerThread();
}

void McpPlugin::RegisterTicketRoutes() {
    impl_->svr.Get("/mcp/list_tickets",
                   [this](const httplib::Request& req, httplib::Response& res) { HandleListTickets(req, res); });

    // Attachment proxy:
    // Tracker attachment URLs generally require Basic Auth headers; Unreal's WebBrowser won't attach them.
    // We proxy through the MCP server (running in the same process) so Unreal can embed/load it.
    impl_->svr.Get("/mcp/attachment_proxy",
                   [this](const httplib::Request& req, httplib::Response& res) { HandleAttachmentProxy(req, res); });

    impl_->svr.Get("/mcp/search",
                   [this](const httplib::Request& req, httplib::Response& res) { HandleSearchTickets(req, res); });
}

void McpPlugin::HandleListTickets(const httplib::Request& req, httplib::Response& res) {
    if (!Authorize(req, res)) {
        return;
    }
    const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsPtr;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& t : tickets) {
        nlohmann::json fields = nlohmann::json::object();
        for (const auto& fieldId : impl_->export_fields) {
            const auto it = t.fieldValues.find(fieldId);
            if (it != t.fieldValues.end()) {
                fields[fieldId] = it->second;
            }
        }
        j.push_back({{"id", t.id}, {"fields", std::move(fields)}});
    }
    res.set_content(j.dump(), "application/json");
}

void McpPlugin::HandleAttachmentProxy(const httplib::Request& req, httplib::Response& res) {
    if (!Authorize(req, res)) {
        return;
    }
    const std::string targetUrl = req.get_param_value("url");
    if (targetUrl.empty()) {
        res.status = 400;
        res.set_content("Missing `url` query parameter.", "text/plain");
        return;
    }
    // Cap the URL length before any further work: the 1 MiB payload limit does
    // not bound query-string params, so an over-long url would otherwise buffer
    // unbounded. A real tracker attachment URL is well under 8 KiB.
    constexpr size_t kMaxAttachmentUrlBytes = 8u * 1024u;
    if (targetUrl.size() > kMaxAttachmentUrlBytes) {
        res.status = 414;
        res.set_content("`url` query parameter is too long.", "text/plain");
        return;
    }
    if (!LooksLikeHttpUrl(targetUrl)) {
        res.status = 400;
        res.set_content("Invalid `url` parameter (expected http/https).", "text/plain");
        return;
    }
    if (targetUrl.rfind("https://", 0) != 0) {
        res.status = 400;
        res.set_content("Attachment proxy requires https URLs.", "text/plain");
        return;
    }
    // Reject userinfo (`user:pass@host`) outright. cpr connects to the host after
    // the last '@', so a `https://allowed.tracker:x@evil.com/` URL would otherwise
    // pass the allow-list (keyed on the userinfo-looking prefix) while the fetch --
    // carrying the Basic-auth tracker credential -- hits the attacker's host.
    if (UrlHasUserinfo(targetUrl)) {
        res.status = 400;
        res.set_content("Attachment URL must not contain userinfo.", "text/plain");
        return;
    }
    const std::string targetHost = ExtractHostFromUrl(targetUrl);
    if (!IsAllowedAttachmentHost(targetHost, impl_->tracker_domain)) {
        res.status = 403;
        res.set_content("Attachment host is not allowlisted.", "text/plain");
        return;
    }

    const TrackerConfig currentCfg = ConfigManager::Load();
    if (currentCfg.Email.empty() || currentCfg.ApiToken.empty() || currentCfg.Domain.empty()) {
        res.status = 500;
        res.set_content("Missing Tracker configuration (email/token/domain).", "text/plain");
        return;
    }

    const std::string basicCred = currentCfg.Email + ":" + currentCfg.ApiToken;
    const std::string authHeader = "Basic " + Base64Encode(basicCred);

    cpr::Header headers{
        {"Accept", "*/*"}, {"Authorization", authHeader}, {"User-Agent", "Smatchet/1.0 Tracker-Attachment-Proxy"}};
    cpr::Redirect redirect(false, false);

    constexpr size_t kMaxAttachmentProxyBytes = 10u * 1024u * 1024u;
    bool sizeExceeded = false;
    std::string bodyAccum;
    bodyAccum.reserve(64 * 1024);
    cpr::WriteCallback writeCb{[&](std::string data, intptr_t) -> bool {
        if (bodyAccum.size() + data.size() > kMaxAttachmentProxyBytes) {
            sizeExceeded = true;
            return false;
        }
        bodyAccum.append(data);
        return true;
    }};
    const auto resp =
        cpr::Get(cpr::Url{targetUrl}, headers, redirect, writeCb, cpr::ConnectTimeout{5000}, cpr::Timeout{60000});
    if (sizeExceeded) {
        res.status = 413;
        res.set_content("Attachment too large for proxy.", "text/plain");
        return;
    }
    if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
        res.status = 502;
        res.set_content("Attachment upstream fetch failed.", "text/plain");
        return;
    }

    std::string contentType = "application/octet-stream";
    try {
        const auto it = resp.header.find("Content-Type");
        if (it != resp.header.end()) {
            contentType = it->second;
        }
    } catch (...) { // catch-all-ok: keep default contentType on parse failure
    }

    res.status = resp.status_code;
    res.set_content(bodyAccum, contentType);
}

void McpPlugin::HandleSearchTickets(const httplib::Request& req, httplib::Response& res) {
    if (!Authorize(req, res)) {
        return;
    }
    std::string query = req.get_param_value("q");
    if (query.size() > 512) {
        res.status = 400;
        res.set_content("Query too long.", "text/plain");
        return;
    }
    const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
    const auto& tickets = *ticketsPtr;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& t : tickets) {
        bool matches = (t.id.find(query) != std::string::npos);
        if (!matches) {
            for (const auto& fieldId : impl_->export_fields) {
                const auto it = t.fieldValues.find(fieldId);
                if (it == t.fieldValues.end())
                    continue;
                if (it->second.find(query) != std::string::npos) {
                    matches = true;
                    break;
                }
            }
        }
        if (matches) {
            nlohmann::json fields = nlohmann::json::object();
            for (const auto& fieldId : impl_->export_fields) {
                const auto it = t.fieldValues.find(fieldId);
                if (it != t.fieldValues.end()) {
                    fields[fieldId] = it->second;
                }
            }
            j.push_back({{"id", t.id}, {"fields", std::move(fields)}});
        }
    }
    res.set_content(j.dump(), "application/json");
}

void McpPlugin::RegisterToolsListRoute() {
    impl_->svr.Get("/mcp/tools/list", [this](const httplib::Request& req, httplib::Response& res) {
        if (!Authorize(req, res))
            return;
        LOG_TRACE("MCP: GET /mcp/tools/list remote=%s", req.remote_addr.c_str());
        nlohmann::json j = nlohmann::json::array();
        // Unified Command System — registered commands take precedence and define
        // canonical names. Legacy `list_active_tickets` / `search_active_tickets`
        // surface via the registry's alias table; we don't double-list them.
        const auto registryCommands = impl_->app->Commands().All();
        std::transform(registryCommands.begin(), registryCommands.end(), std::back_inserter(j), [](const auto& c) {
            return nlohmann::json{{"name", c.Name}, {"description", c.Summary}, {"inputSchema", c.BuildJsonSchema()}};
        });
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        const auto luaTools = impl_->app->GetLuaBindingHost()->GetLuaMcpTools();
        if (impl_->allow_lua_execution) {
            j.push_back(BuildRunLuaToolEntry());
        }
        std::transform(luaTools.begin(), luaTools.end(), std::back_inserter(j), [](const auto& t) {
            return nlohmann::json{
                {"name", t.name}, {"description", t.description}, {"inputSchema", t.parametersSchema}};
        });
#endif
        res.set_content(nlohmann::json{{"tools", j}}.dump(), "application/json");
    });
}

void McpPlugin::RegisterToolsCallRoute() {
    impl_->svr.Post("/mcp/tools/call",
                    [this](const httplib::Request& req, httplib::Response& res) { HandleToolsCall(req, res); });
}

bool McpPlugin::AllowToolsCall(long long& retryAfterMs) {
    std::lock_guard<std::mutex> lock(impl_->toolsCallBucketMutex);
    const smatchet::mcp::pure::McpRateLimitDecision decision = smatchet::mcp::pure::ConsumeToolsCallToken(
        impl_->toolsCallBucket, impl_->toolsCallRateBurst, static_cast<double>(impl_->toolsCallRateRefillPerSec),
        NowMonotonicMs());
    retryAfterMs = decision.RetryAfterMs;
    return decision.Allow;
}

bool McpPlugin::RestToolsCallWithinRateLimit(const std::string& name, const std::string& remote,
                                             httplib::Response& res) {
    long long retryAfterMs = 0;
    if (AllowToolsCall(retryAfterMs)) {
        return true;
    }
    // Same HTTP-200 canonical-envelope convention as DispatchRegistryToolsCall: the
    // envelope carries the structured error so callers parse it, not the transport.
    const nlohmann::json envelope = {{"ok", false},
                                     {"command", name},
                                     {"error",
                                      {{"code", "rate-limited"},
                                       {"message", "tools/call rate limit exceeded"},
                                       {"details", {{"retry_after_ms", retryAfterMs}}}}}};
    res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}}.dump(),
                    "application/json");
    LOG_WARN("MCP: REST tools/call rate-limited tool=%s remote=%s", name.c_str(), remote.c_str());
    AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + name + " rate-limited remote=" + remote);
    return false;
}

void McpPlugin::DispatchRegistryToolsCall(const std::string& name, const nlohmann::json& arguments,
                                          const std::string& remote, httplib::Response& res) {
    smatchet::cmd::CommandContext cctx;
    cctx.ScenarioHost = impl_->app;
    cctx.Threading = impl_->app;
    cctx.Source = smatchet::cmd::CommandSource::Mcp;
    cctx.ConfirmedDestructive = arguments.value("__confirm", false);
    cctx.DryRun = arguments.value("__dry_run", false);
    cctx.TimeoutMs = arguments.value("__timeout_ms", 0);
    smatchet::cmd::CommandResult cr = impl_->app->Commands().Dispatch(name, arguments, cctx);
    const nlohmann::json envelope = cr.ToWireJson(name, cctx.DryRun);
    const std::string envelopeStr = envelope.dump();
    LOG_TRACE("MCP: REST tools/call registry tool=%s ok=%d", name.c_str(), cr.Ok ? 1 : 0);
    AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + name +
                                          (cr.Ok ? " ok" : " FAIL(" + cr.Error.Message.substr(0, 80) + ")") +
                                          " remote=" + remote);
    // Always HTTP 200: the envelope carries ok/error; isError=true only for
    // genuine tool-call failures, not for structured command errors.
    res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", envelopeStr}}}}}.dump(),
                    "application/json");
}

void McpPlugin::EmitUnknownToolsCallEnvelope(const std::string& name, const nlohmann::json& arguments,
                                             const std::string& remote, httplib::Response& res) {
    smatchet::cmd::CommandContext cctx;
    cctx.ScenarioHost = impl_->app;
    cctx.Threading = impl_->app;
    cctx.Source = smatchet::cmd::CommandSource::Mcp;
    smatchet::cmd::CommandResult cr = impl_->app->Commands().Dispatch(name, arguments, cctx);
    const nlohmann::json envelope = cr.ToWireJson(name, false);
    res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}}.dump(),
                    "application/json");
    AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + name + " unknown-command remote=" + remote);
}

void McpPlugin::HandleToolsCall(const httplib::Request& req, httplib::Response& res) {
    if (!Authorize(req, res))
        return;
    const std::string remote = req.remote_addr;
    try {
        // Depth/node-bounded parse of the raw HTTP body (ATTACKER-CONTROLLED). A bare
        // json::parse recurses once per nesting level and stack-overflows on a hostile
        // deep payload BEFORE name/arguments are extracted — the 1 MiB payload cap bounds
        // BYTES only, not depth (Pillar 3 — Never crash). On a cap hit return the same
        // structured HTTP-400 envelope the parse-exception path below produces; never log
        // the raw body.
        std::string parseErr;
        const nlohmann::json body =
            smatchet::json_safe::ParseBounded(req.body, parseErr, /*maxBytes=*/1u * 1024u * 1024u);
        if (!parseErr.empty()) {
            LOG_WARN("MCP: REST tools/call rejected malformed/oversized body remote=%s", remote.c_str());
            res.status = 400;
            res.set_content(nlohmann::json{{"isError", true}, {"error", parseErr}}.dump(), "application/json");
            AppendMcpActivityLine(impl_->app, "MCP: REST tools/call FAIL remote=" + remote +
                                                  " err=parse error: " + TruncateOneLine(parseErr, 200));
            return;
        }
        std::string name = body.value("name", "");
        const nlohmann::json arguments = body.value("arguments", nlohmann::json::object());
        std::string paramsStr = arguments.dump();
        LOG_TRACE("MCP: REST POST /mcp/tools/call remote=%s tool=%s args_len=%zu body_len=%zu", req.remote_addr.c_str(),
                  name.c_str(), paramsStr.size(), req.body.size());
        // Token-bucket gate ahead of every dispatch arm (registry / run_lua / Lua MCP
        // tools / unknown-command) — B3: nothing bounded tool-call frequency before.
        if (!RestToolsCallWithinRateLimit(name, remote, res)) {
            return;
        }
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        // error / result are populated and consumed only by the Lua tool paths
        // (run_lua / isLuaMcpTool → EmitToolsCallResult). The registry and no-Lua
        // paths build their own envelopes, so these are unused when Lua is off.
        std::string error;
        std::string result;
#endif
        // Unified Command System: try registry first. Always returns HTTP 200
        // with a canonical envelope {ok, command, data|error} in content[0].text —
        // even for structured errors (confirm-required, not-found, etc.). This lets
        // callers parse the envelope rather than treating every error as a transport fail.
        if (impl_->app->Commands().Contains(name)) {
            DispatchRegistryToolsCall(name, arguments, remote, res);
            return;
        } else
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            if (name == "run_lua") {
            if (!impl_->allow_lua_execution) {
                error = "run_lua is disabled by configuration";
                LOG_TRACE("MCP: run_lua rejected (disabled in config)");
                AppendMcpActivityLine(impl_->app, "MCP: REST tools/call run_lua FAIL remote=" + remote +
                                                      " err=disabled by configuration");
            } else {
                const nlohmann::json args = arguments;
                const std::string mode = args.value("mode", "");
                LOG_TRACE("MCP: run_lua mode=%s", mode.c_str());
                if (mode == "snippet") {
                    result = impl_->app->ExecuteLuaSnippetForMcp(args.value("code", std::string()),
                                                                 args.value("args", nlohmann::json::object()), error);
                } else if (mode == "script") {
                    result = impl_->app->ExecuteLuaScriptForMcp(args.value("script", std::string()),
                                                                args.value("args", nlohmann::json::object()), error);
                } else {
                    error = "run_lua requires mode='snippet' or mode='script'";
                }
            }
        } else {
            // Check whether this name belongs to an mcp.register_tool Lua tool before
            // trying ExecuteLuaMcpTool. If it's not a Lua MCP tool either, fall back to
            // the registry for a structured unknown-command response with fuzzy suggestions.
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            const auto luaTools = impl_->app->GetLuaBindingHost()->GetLuaMcpTools();
            const bool isLuaMcpTool =
                std::any_of(luaTools.begin(), luaTools.end(), [&name](const auto& lt) { return lt.name == name; });
            if (isLuaMcpTool) {
                result = impl_->app->ExecuteLuaMcpTool(name, paramsStr, error);
            } else {
#endif
                // Not in registry, not run_lua, not a Lua MCP tool.
                // Dispatch through registry to get a canonical unknown-command envelope
                // with fuzzy suggestions (e.g. "did you mean 'tickets.search_active'?").
                EmitUnknownToolsCallEnvelope(name, arguments, remote, res);
                return;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            }
#endif
        }
#else
        // No Lua: unknown name → structured unknown-command from registry.
        {
            smatchet::cmd::CommandContext cctx;
            cctx.ScenarioHost = impl_->app;
            cctx.Threading = impl_->app;
            cctx.Source = smatchet::cmd::CommandSource::Mcp;
            smatchet::cmd::CommandResult cr = impl_->app->Commands().Dispatch(name, arguments, cctx);
            const nlohmann::json envelope = cr.ToWireJson(name, false);
            res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}}.dump(),
                            "application/json");
            return;
        }
#endif
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        // Only the Lua paths (run_lua / isLuaMcpTool) fall through to here with a
        // populated `result`; every non-Lua path above returns its own envelope
        // first, so this call is unreachable when Lua is compiled out (C4702 /WX).
        EmitToolsCallResult(res, name, remote, arguments, error, result);
#endif
    } catch (const std::exception& e) {
        LOG_TRACE("MCP: REST tools/call parse_exception %s", e.what());
        res.status = 400;
        res.set_content(nlohmann::json{{"isError", true}, {"error", e.what()}}.dump(), "application/json");
        AppendMcpActivityLine(impl_->app, "MCP: REST tools/call FAIL remote=" + remote +
                                              " err=parse error: " + TruncateOneLine(e.what(), 200));
    }
}

void McpPlugin::EmitToolsCallResult(httplib::Response& res, const std::string& name, const std::string& remote,
                                    const nlohmann::json& arguments, const std::string& error,
                                    const std::string& result) {
    // cppcheck-suppress knownConditionTrueFalse  // !error.empty() is condition-
    // dependent on SMATCHET_WITH_LUA_AUTOMATION; only "always true" under #else.
    if (!error.empty()) {
        LOG_TRACE("MCP: REST tools/call error tool=%s %s", name.c_str(), error.c_str());
        res.status = (name == "run_lua" && !impl_->allow_lua_execution) ? 404 : 500;
        res.set_content(nlohmann::json{{"isError", true}, {"content", {{{"type", "text"}, {"text", error}}}}}.dump(),
                        "application/json");
        if (!(name == "run_lua" && !impl_->allow_lua_execution)) {
            AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + (name.empty() ? "?" : name) +
                                                  " FAIL remote=" + remote + " err=" + TruncateOneLine(error, 200));
        }
    } else {
        LOG_TRACE("MCP: REST tools/call ok tool=%s result_len=%zu", name.c_str(), result.size());
        res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", result}}}}}.dump(), "application/json");
        const std::string summary =
            (name == "run_lua") ? BuildRunLuaSummary(arguments) : BuildToolCallSummary(name, arguments);
        AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + (name.empty() ? "?" : name) +
                                              " ok remote=" + remote + " " + summary);
    }
}

void McpPlugin::RegisterSseRoute() {
    // --- Actual MCP JSON-RPC Specification (SSE) ---

    impl_->svr.Get(SmatchetDefaults::Mcp::kSsePath, [this](const httplib::Request& req, httplib::Response& res) {
        if (!Authorize(req, res))
            return;

        // Concurrent-SSE bound (security synthesis number 20). Reserve a slot BEFORE
        // streaming so a local client cannot open unbounded SSE streams and exhaust the
        // (size-8) httplib worker pool. Increment-then-check is race-free under concurrent
        // connects: claim a slot, and `sseGuard`'s deleter releases it on every exit path
        // (the over-cap 503 below, or when httplib destroys the content provider on close).
        Impl* impl = impl_.get();
        impl->activeSseConnections.fetch_add(1, std::memory_order_acq_rel);
        std::shared_ptr<void> sseGuard(static_cast<void*>(nullptr), [impl](void*) {
            impl->activeSseConnections.fetch_sub(1, std::memory_order_acq_rel);
        });
        // The pre-increment count = (current - 1); admit only if THAT was below the cap.
        if (!CanAcceptSseConnection(impl->activeSseConnections.load(std::memory_order_acquire) - 1)) {
            res.status = 503;
            res.set_header("Retry-After", "5");
            res.set_content("{\"error\":\"too many concurrent SSE connections\"}", "application/json");
            LOG_WARN("MCP: rejected SSE connect — concurrent-connection cap reached (remote=%s)",
                     req.remote_addr.c_str());
            return; // sseGuard releases the reserved slot.
        }

        // In a real implementation, we'd manage session IDs. For now, we use a single global
        // endpoint.
        res.set_header("Content-Type", "text/event-stream");
        res.set_header("Cache-Control", "no-cache");
        res.set_header("Connection", "keep-alive");
        // CORS: never wildcard — see ResolveSseCorsOrigin. Echo the Authorize-vetted Origin on
        // the loopback bind; emit no CORS header on the 0.0.0.0 bind (where the Host/Origin
        // gate is intentionally skipped and a grant would expose the token-authenticated
        // stream to arbitrary web origins).
        const std::string corsOrigin = ResolveSseCorsOrigin(req.get_header_value("Origin"),
                                                            impl->bind_host == SmatchetDefaults::Mcp::kBindLocalhost);
        if (!corsOrigin.empty()) {
            res.set_header("Access-Control-Allow-Origin", corsOrigin);
        }

        std::string endpoint = "/mcp/messages";
        std::string event = "event: endpoint\ndata: " + endpoint + "\n\n";
        res.set_chunked_content_provider(
            "text/event-stream",
            [event, impl, sseGuard](size_t offset, httplib::DataSink& sink) {
                if (offset == 0) {
                    // Initial endpoint event. `sink.write` returns false on client disconnect.
                    return sink.write(event.data(), event.size());
                }
                // Wait up to 1s for shutdown; return early (closing the stream)
                // once the OnStop path flips shuttingDown. Without this early-out
                // a server stop would block until every connected client's worker
                // finished its current sleep — up to N seconds for N clients.
                {
                    std::unique_lock<std::mutex> lk(impl->shutdownMutex);
                    if (impl->shutdownCv.wait_for(lk, std::chrono::seconds(1),
                                                  [impl] { return impl->shuttingDown.load(); })) {
                        return false;
                    }
                }
                constexpr char kSseHeartbeat[] = ": heartbeat\n\n";
                return sink.write(kSseHeartbeat, sizeof(kSseHeartbeat) - 1);
            },
            nullptr);
    });
}

void McpPlugin::HandleJsonRpcRegistryCall(const std::string& name, const nlohmann::json& params,
                                          const std::string& rpcRemote, nlohmann::json& jres) {
    const nlohmann::json args = params.value("arguments", nlohmann::json::object());
    smatchet::cmd::CommandContext cctx;
    cctx.ScenarioHost = impl_->app;
    cctx.Threading = impl_->app;
    cctx.Source = smatchet::cmd::CommandSource::Mcp;
    cctx.ConfirmedDestructive = args.value("__confirm", false);
    cctx.DryRun = args.value("__dry_run", false);
    cctx.TimeoutMs = args.value("__timeout_ms", 0);
    smatchet::cmd::CommandResult cr = impl_->app->Commands().Dispatch(name, args, cctx);
    const nlohmann::json envelope = cr.ToWireJson(name, cctx.DryRun);
    // MCP `result.content[0].text` carries the envelope as a string —
    // that's the standard MCP shape and what hosts know to parse. The
    // envelope itself stays the canonical JSON contract for agents.
    jres["result"] = {{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}};
    AppendMcpActivityLine(impl_->app,
                          "MCP: JSON-RPC tools/call " + name + (cr.Ok ? " ok" : " FAIL") + " remote=" + rpcRemote);
}

void McpPlugin::HandleJsonRpcRunLua(const nlohmann::json& params, const std::string& rpcRemote, nlohmann::json& jres) {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
    const nlohmann::json luaArgs = params.value("arguments", nlohmann::json::object());
    if (!impl_->allow_lua_execution) {
        jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua FAIL remote=" + rpcRemote +
                                              " err=disabled by configuration");
    } else {
        std::string error;
        std::string result;
        const std::string mode = luaArgs.value("mode", "");
        if (mode == "snippet") {
            result = impl_->app->ExecuteLuaSnippetForMcp(luaArgs.value("code", std::string()),
                                                         luaArgs.value("args", nlohmann::json::object()), error);
        } else if (mode == "script") {
            result = impl_->app->ExecuteLuaScriptForMcp(luaArgs.value("script", std::string()),
                                                        luaArgs.value("args", nlohmann::json::object()), error);
        } else {
            error = "run_lua requires mode='snippet' or mode='script'";
        }
        if (!error.empty()) {
            jres["error"] = {{"code", -32603}, {"message", error}};
        } else {
            jres["result"] = {{"content", {{{"type", "text"}, {"text", result}}}}};
        }
        if (jres.contains("error")) {
            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua FAIL remote=" + rpcRemote +
                                                  " err=" + ExtractJsonRpcErrorMessage(jres, 256));
        } else {
            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua ok remote=" + rpcRemote + " " +
                                                  BuildRunLuaSummary(luaArgs));
        }
    }
#else
    (void)params;
    jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
    AppendMcpActivityLine(impl_->app,
                          "MCP: JSON-RPC tools/call run_lua FAIL remote=" + rpcRemote + " err=Lua automation disabled");
#endif
}

void McpPlugin::HandleJsonRpcToolsCall(const std::string& remote, const nlohmann::json& params, nlohmann::json& jres) {
    const std::string name = params.value("name", "");
    const std::string rpcRemote = remote;
    LOG_TRACE("MCP: JSON-RPC tools/call tool=%s", name.c_str());

    // Same token-bucket gate as the REST route — both entry points share one bucket.
    long long retryAfterMs = 0;
    if (!AllowToolsCall(retryAfterMs)) {
        jres["error"] = {{"code", -32000},
                         {"message", "rate-limited: tools/call rate limit exceeded; retry after " +
                                         std::to_string(retryAfterMs) + " ms"}};
        LOG_WARN("MCP: JSON-RPC tools/call rate-limited tool=%s remote=%s", name.c_str(), rpcRemote.c_str());
        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " + name + " rate-limited remote=" + rpcRemote);
        return;
    }

    if (impl_->app->Commands().Contains(name)) {
        HandleJsonRpcRegistryCall(name, params, rpcRemote, jres);
    } else if (name == "run_lua") {
        HandleJsonRpcRunLua(params, rpcRemote, jres);
    } else if (name == "list_active_tickets") {
        const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
        nlohmann::json ticketIds = nlohmann::json::array();
        std::transform(ticketsPtr->begin(), ticketsPtr->end(), std::back_inserter(ticketIds),
                       [](const auto& t) { return t.id; });
        jres["result"] = {{"content", {{{"type", "text"}, {"text", "Active Tickets: " + ticketIds.dump()}}}}};
        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call list_active_tickets ok remote=" + rpcRemote +
                                              " count=" + std::to_string(ticketIds.size()));
    } else if (name == "search_active_tickets") {
        auto args = params.value("arguments", nlohmann::json::object());
        std::string query = args.value("query", "");
        const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
        nlohmann::json matches = nlohmann::json::array();
        for (const auto& t : *ticketsPtr) {
            const bool hit = (t.id.find(query) != std::string::npos) ||
                             std::any_of(t.fieldValues.begin(), t.fieldValues.end(),
                                         [&](const auto& kv) { return kv.second.find(query) != std::string::npos; });
            if (hit) {
                matches.push_back(t.id);
            }
        }
        jres["result"] = {
            {"content", {{{"type", "text"}, {"text", "Search Results for '" + query + "': " + matches.dump()}}}}};
        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call search_active_tickets ok remote=" + rpcRemote +
                                              " query=" + TruncateOneLine(query, 80) +
                                              " matches=" + std::to_string(matches.size()));
    } else {
        // Check Lua tools
        const nlohmann::json argObj = params.value("arguments", nlohmann::json::object());
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
        std::string argsStr = argObj.dump();
        std::string error;
        std::string result = impl_->app->ExecuteLuaMcpTool(name, argsStr, error);
        if (error.empty() && !result.empty()) {
            jres["result"] = {{"content", {{{"type", "text"}, {"text", result}}}}};
        } else if (!error.empty()) {
            jres["error"] = {{"code", -32603}, {"message", error}};
        } else {
            jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
        }
#else
        jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
#endif
        if (jres.contains("error")) {
            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " + (name.empty() ? "?" : name) +
                                                  " FAIL remote=" + rpcRemote +
                                                  " err=" + ExtractJsonRpcErrorMessage(jres, 256));
        } else {
            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " + (name.empty() ? "?" : name) +
                                                  " ok remote=" + rpcRemote + " " + BuildToolCallSummary(name, argObj));
        }
    }
}

void McpPlugin::RegisterJsonRpcRoutes() {
    // Streamable HTTP clients (e.g. Cursor) POST JSON-RPC to the same URL as the SSE endpoint; legacy clients
    // POST to /mcp/messages after reading the endpoint event from GET /mcp/sse.
    auto handleMcpJsonRpcPost = [this](const httplib::Request& req, httplib::Response& res) {
        if (!Authorize(req, res))
            return;

        try {
            // Depth/node-bounded parse of the raw HTTP body (ATTACKER-CONTROLLED). A bare
            // json::parse recurses once per nesting level and stack-overflows on a hostile
            // deep payload — the 1 MiB payload cap bounds BYTES only, not depth (Pillar 3 —
            // Never crash). On a cap hit return the JSON-RPC -32700 parse error the
            // parse-exception path below produces; never log the raw body.
            std::string parseErr;
            const nlohmann::json jreq =
                smatchet::json_safe::ParseBounded(req.body, parseErr, /*maxBytes=*/1u * 1024u * 1024u);
            if (!parseErr.empty()) {
                LOG_WARN("MCP: JSON-RPC rejected malformed/oversized body remote=%s", req.remote_addr.c_str());
                res.status = 400;
                res.set_content(
                    nlohmann::json{{"jsonrpc", "2.0"},
                                   {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + parseErr}}}}
                        .dump(),
                    "application/json");
                AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC parse FAIL remote=") + req.remote_addr +
                                                      " err=" + TruncateOneLine(parseErr, 200));
                return;
            }
            std::string method = jreq.value("method", "");
            const bool isNotification = !jreq.contains("id");
            LOG_TRACE("MCP: JSON-RPC remote=%s method=%s notification=%d body_len=%zu", req.remote_addr.c_str(),
                      method.c_str(), isNotification ? 1 : 0, req.body.size());
            if (isNotification) {
                res.status = 200;
                return;
            }
            auto id = jreq["id"];
            nlohmann::json jres = {{"jsonrpc", "2.0"}, {"id", id}};

            if (method == "initialize") {
                const auto initParams = jreq.value("params", nlohmann::json::object());
                std::string negotiatedProtocol = initParams.value("protocolVersion", std::string("2024-11-05"));
                if (negotiatedProtocol.empty()) {
                    negotiatedProtocol = "2024-11-05";
                }
                jres["result"] = {{"protocolVersion", negotiatedProtocol},
                                  {"capabilities", {{"tools", nlohmann::json::object()}}},
                                  {"serverInfo", {{"name", "Smatchet"}, {"version", "1.2"}}}};
                AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC initialize ok remote=") + req.remote_addr +
                                                      " protocol=" + TruncateOneLine(negotiatedProtocol, 64));
            } else if (method == "tools/list") {
                nlohmann::json toolList = nlohmann::json::array();

                // Unified Command System — canonical command catalog. Legacy names
                // `list_active_tickets` / `search_active_tickets` are aliases on the
                // matching registry commands, so they continue to route through
                // tools/call without needing a duplicate tools/list entry.
                const auto registryCommands = impl_->app->Commands().All();
                std::transform(
                    registryCommands.begin(), registryCommands.end(), std::back_inserter(toolList), [](const auto& c) {
                        return nlohmann::json{
                            {"name", c.Name}, {"description", c.Summary}, {"inputSchema", c.BuildJsonSchema()}};
                    });

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                if (impl_->allow_lua_execution) {
                    toolList.push_back(BuildRunLuaToolEntry());
                }
                const auto tools = impl_->app->GetLuaBindingHost()->GetLuaMcpTools();
                std::transform(tools.begin(), tools.end(), std::back_inserter(toolList), [](const auto& t) {
                    return nlohmann::json{
                        {"name", t.name}, {"description", t.description}, {"inputSchema", t.parametersSchema}};
                });
#endif
                jres["result"] = {{"tools", toolList}};
                AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC tools/list ok remote=") + req.remote_addr +
                                                      " tools=" + std::to_string(toolList.size()));
            } else if (method == "tools/call") {
                const auto params = jreq.value("params", nlohmann::json::object());
                HandleJsonRpcToolsCall(req.remote_addr, params, jres);
            } else {
                jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
                AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC FAIL remote=") + req.remote_addr +
                                                      " method=" + TruncateOneLine(method, 120) +
                                                      " err=method not found");
            }

            res.set_content(jres.dump(), "application/json");
        } catch (const std::exception& e) {
            res.status = 400;
            res.set_content(
                nlohmann::json{{"jsonrpc", "2.0"},
                               {"error", {{"code", -32700}, {"message", std::string("Parse error: ") + e.what()}}}}
                    .dump(),
                "application/json");
            AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC parse FAIL remote=") + req.remote_addr +
                                                  " err=" + TruncateOneLine(e.what(), 200));
        }
    };

    impl_->svr.Post("/mcp/messages", handleMcpJsonRpcPost);
    impl_->svr.Post(SmatchetDefaults::Mcp::kSsePath, handleMcpJsonRpcPost);
}

void McpPlugin::StartServerThread() {
    if (impl_->thread.joinable()) {
        return;
    }

    // #987: cap httplib's worker pool — the default queue spawns
    // max(8, hardware_concurrency()-1) threads (a 47-thread burst on a 48-core
    // box; one failed std::thread ctor under build load = std::system_error).
    // 8 (not the notify server's 2): EACH connected client parks ~2 workers — an
    // SSE stream sits permanently in the heartbeat wait loop AND keep-alive parks
    // one per persistent POST channel — so two clients already consume ~4. A pool
    // that small would silently exhaust (the next connection queues forever in
    // httplib's unbounded job deque with no log line — a hang that is worse to
    // diagnose than the crash). 8 covers a few concurrent MCP hosts + long tool
    // calls while still killing the 47-thread burst and its ~47 MB stack reserve.
    impl_->svr.new_task_queue = [] {
        return new httplib::ThreadPool(8); // httplib owns the queue
    };

    AppController* appPtr = impl_->app;
    // #987 review HIGH-2: guard the std::thread CONSTRUCTOR — it throws
    // std::system_error under the same thread-exhaustion pressure, and unguarded
    // it would escape StartServerThread() to std::terminate. The listen lambda's
    // own body is already guarded below; this covers the spawn itself.
    try {
        impl_->thread = std::thread([this, appPtr]() {
            try {
                // `listen()` blocks until `stop()`, so activity would stay empty during normal use.
                // Split bind vs accept loop so we can log as soon as the port is open.
                if (appPtr != nullptr) {
                    appPtr->AppendMcpActivity(std::string("MCP: binding ") + impl_->bind_host + ":" +
                                              std::to_string(port_) + "...");
                }
                if (!impl_->svr.bind_to_port(impl_->bind_host.c_str(), port_)) {
                    if (appPtr != nullptr) {
                        appPtr->AppendMcpActivity("MCP: bind failed (port in use or permission denied).");
                    }
                    LOG_ERROR("MCP server: bind_to_port failed for %s:%d", impl_->bind_host.c_str(), port_);
                    return;
                }
                if (appPtr != nullptr) {
                    appPtr->AppendMcpActivity(std::string("MCP: listening on http://") + impl_->bind_host + ":" +
                                              std::to_string(port_) + " (MCP routes ready).");
                }
                const bool ok = impl_->svr.listen_after_bind();
                if (appPtr != nullptr) {
                    appPtr->AppendMcpActivity(std::string("MCP: HTTP server stopped (listen returned ") +
                                              (ok ? "true" : "false") + ").");
                }
            } catch (const std::exception& e) {
                if (appPtr != nullptr) {
                    appPtr->AppendMcpActivity(std::string("MCP: listen thread exception: ") + e.what());
                }
                LOG_ERROR("MCP server thread error: %s", e.what());
            } catch (...) {
                if (appPtr != nullptr) {
                    appPtr->AppendMcpActivity("MCP: listen thread unknown exception.");
                }
                LOG_ERROR("MCP server thread unknown error");
            }
        });
    } catch (const std::exception& e) {
        if (appPtr != nullptr) {
            appPtr->AppendMcpActivity(std::string("MCP: listen thread spawn failed: ") + e.what());
        }
        LOG_WARN("MCP server: listen thread spawn failed (%s); MCP server disabled this session", e.what());
        impl_->svr.stop();
    }
}

void McpPlugin::OnStop() {
    if (!impl_ || !impl_->thread.joinable()) {
        return;
    }
    // Wake any SSE heartbeat lambdas blocked in shutdownCv.wait_for before
    // asking httplib to stop — otherwise svr.stop() would block while each
    // worker finishes its current heartbeat cycle.
    {
        std::lock_guard<std::mutex> lk(impl_->shutdownMutex);
        impl_->shuttingDown.store(true);
    }
    impl_->shutdownCv.notify_all();
    impl_->svr.stop();
    impl_->thread.join();

    // Delete instance.json so the CLI knows the server is gone.
    if (!impl_->instanceJsonPath.empty()) {
        std::remove(impl_->instanceJsonPath.c_str()); // best-effort; ignore failure
        impl_->instanceJsonPath.clear();
    }
}
