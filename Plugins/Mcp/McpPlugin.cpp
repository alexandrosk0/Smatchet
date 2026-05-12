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
#include "ConfigManager.h"
#include "Commands/Command.h"
#include <cstdio>   // std::remove, std::fopen/fwrite/fclose
#if !defined(_WIN32)
#include <unistd.h>   // getpid()
#endif
#include "Commands/CommandRegistry.h"
#include "Logger.h"
#include <httplib.h>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <iterator>
#include <mutex>

namespace {
std::string Base64Encode(const std::string& in) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    for (size_t i = 0; i < in.size(); i += 3) {
        const unsigned char a = static_cast<unsigned char>(in[i]);
        const unsigned char b = (i + 1 < in.size()) ? static_cast<unsigned char>(in[i + 1]) : 0u;
        const unsigned char c = (i + 2 < in.size()) ? static_cast<unsigned char>(in[i + 2]) : 0u;
        out += table[a >> 2];
        out += table[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < in.size()) ? table[((b & 15) << 2) | (c >> 6)] : '=';
        out += (i + 2 < in.size()) ? table[c & 63] : '=';
    }
    return out;
}

bool LooksLikeHttpUrl(const std::string& url) { return url.find("http://") == 0 || url.find("https://") == 0; }

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string TrimAsciiWhitespace(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
        ++begin;
    }
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
        --end;
    }
    return value.substr(begin, end - begin);
}

std::string NormalizeDomain(const std::string& rawDomain) {
    std::string value = TrimAsciiWhitespace(rawDomain);
    if (value.find("://") != std::string::npos) {
        const size_t schemeSep = value.find("://");
        value = value.substr(schemeSep + 3);
    }
    const size_t slashPos = value.find('/');
    if (slashPos != std::string::npos) {
        value.resize(slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value.resize(colonPos);
    }
    return ToLowerAscii(value);
}

std::string ExtractHostFromUrl(const std::string& url) {
    const size_t schemeSep = url.find("://");
    if (schemeSep == std::string::npos) {
        return std::string();
    }
    size_t hostStart = schemeSep + 3;
    if (hostStart >= url.size()) {
        return std::string();
    }
    size_t hostEnd = url.find_first_of("/?#", hostStart);
    const std::string hostAndPort = url.substr(hostStart, hostEnd - hostStart);
    if (hostAndPort.empty()) {
        return std::string();
    }
    if (hostAndPort.front() == '[') {
        const size_t closeBracket = hostAndPort.find(']');
        if (closeBracket == std::string::npos) {
            return std::string();
        }
        return ToLowerAscii(hostAndPort.substr(1, closeBracket - 1));
    }
    const size_t colonPos = hostAndPort.find(':');
    if (colonPos != std::string::npos) {
        return ToLowerAscii(hostAndPort.substr(0, colonPos));
    }
    return ToLowerAscii(hostAndPort);
}

bool IsLoopbackAddress(const std::string& remoteAddr) {
    const std::string lowered = ToLowerAscii(TrimAsciiWhitespace(remoteAddr));
    return lowered == SmatchetDefaults::Mcp::kBindLocalhost || lowered == "::1" || lowered == "localhost"
           || lowered == "::ffff:127.0.0.1";
}

// Constant-time string compare: return true iff a == b. Always reads max(|a|,|b|) bytes.
bool ConstantTimeStringEquals(const std::string& a, const std::string& b) {
    const size_t n = std::max(a.size(), b.size());
    unsigned int diff = static_cast<unsigned int>(a.size() ^ b.size());
    for (size_t i = 0; i < n; ++i) {
        const unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0u;
        const unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= static_cast<unsigned int>(ca ^ cb);
    }
    return diff == 0u;
}

bool IsAllowedAttachmentHost(const std::string& host, const std::string& trackerDomain) {
    if (host.empty() || trackerDomain.empty()) {
        return false;
    }
    if (host == trackerDomain) {
        return true;
    }
    const std::string TrackerSuffix = "." + trackerDomain;
    if (host.size() > TrackerSuffix.size() &&
        host.compare(host.size() - TrackerSuffix.size(), TrackerSuffix.size(), TrackerSuffix) == 0) {
        return true;
    }
    return host == "api.media.atlassian.com";
}

void AppendMcpActivityLine(AppController* app, const std::string& line) {
    if (app != nullptr) {
        app->AppendMcpActivity(line);
    }
}

std::string TruncateOneLine(const std::string& s, std::size_t maxChars) {
    if (s.size() <= maxChars) {
        return s;
    }
    return s.substr(0, maxChars) + "...";
}

std::string BasenameForDisplay(const std::string& path) {
    if (path.empty()) {
        return std::string();
    }
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

static void AppendAllowlistedArgKvs(std::ostringstream& oss, const nlohmann::json& obj) {
    if (!obj.is_object()) {
        return;
    }
    static const char* kKeys[] = {"issue_key", "key", "id", "priority"};
    for (const char* k : kKeys) {
        if (!obj.contains(k)) {
            continue;
        }
        const nlohmann::json& v = obj.at(k);
        oss << ' ' << k << '=';
        if (v.is_string()) {
            oss << TruncateOneLine(v.get<std::string>(), 80);
        } else if (v.is_number_integer()) {
            oss << v.get<std::int64_t>();
        } else if (v.is_number_unsigned()) {
            oss << v.get<std::uint64_t>();
        } else if (v.is_number_float()) {
            oss << v.get<double>();
        } else if (v.is_boolean()) {
            oss << (v.get<bool>() ? "true" : "false");
        } else {
            try {
                oss << TruncateOneLine(v.dump(), 80);
            } catch (...) {
                oss << "?";
            }
        }
    }
}

/// Single canonical `run_lua` tool-list entry — eliminates the REST / JSON-RPC duplication (§5.1).
nlohmann::json BuildRunLuaToolEntry() {
    return {{"name", "run_lua"},
            {"description", "Run a Lua snippet or Scripts/*.lua file with optional args."},
            {"inputSchema",
             {{"type", "object"},
              {"properties",
               {{"mode", {{"type", "string"}, {"enum", {"snippet", "script"}}}},
                {"code", {{"type", "string"}}},
                {"script", {{"type", "string"}}},
                {"args", {{"type", "object"}}}}},
              {"required", {"mode"}}}}};
}

/** Summarize `run_lua` arguments (mode, code/script size or basename, allowlisted nested `args`). */
std::string BuildRunLuaSummary(const nlohmann::json& arguments) {
    if (!arguments.is_object()) {
        return std::string();
    }
    std::ostringstream oss;
    const std::string mode = arguments.value("mode", "");
    oss << "mode=" << mode;
    if (mode == "snippet") {
        const std::string code = arguments.value("code", std::string());
        oss << " code_len=" << code.size();
    } else if (mode == "script") {
        oss << " script=" << TruncateOneLine(BasenameForDisplay(arguments.value("script", std::string())), 48);
    }
    AppendAllowlistedArgKvs(oss, arguments.value("args", nlohmann::json::object()));
    return oss.str();
}

/** Tool name plus sorted argument keys and allowlisted identifier values. */
std::string BuildToolCallSummary(const std::string& toolName, const nlohmann::json& arguments) {
    std::ostringstream oss;
    oss << "tool=" << toolName;
    if (!arguments.is_object()) {
        return oss.str();
    }
    oss << " arg_keys=[";
    bool first = true;
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << it.key();
    }
    oss << ']';
    AppendAllowlistedArgKvs(oss, arguments);
    return oss.str();
}

std::string ExtractJsonRpcErrorMessage(const nlohmann::json& jres, std::size_t maxLen) {
    if (!jres.contains("error")) {
        return std::string();
    }
    const nlohmann::json& e = jres["error"];
    if (!e.is_object()) {
        return TruncateOneLine(e.dump(), maxLen);
    }
    if (e.contains("message") && e["message"].is_string()) {
        return TruncateOneLine(e["message"].get<std::string>(), maxLen);
    }
    try {
        return TruncateOneLine(e.dump(), maxLen);
    } catch (...) {
        return "error";
    }
}

} // namespace

struct McpPlugin::Impl {
    AppController* app = nullptr;
    // Shutdown signaling for SSE chunked-content providers. Heartbeat lambdas
    // wait on shutdownCv with a 1s timeout instead of sleeping unconditionally
    // so OnStop() can reclaim worker threads within microseconds rather than
    // up to 1 second per connected client.
    //
    // Declared BEFORE `svr` on purpose: members destroy in reverse order, and
    // `~Server` joins the worker pool in its destructor. If a worker is still
    // executing the chunked-content lambda when ~Impl begins (svr.stop() does
    // not synchronously drain in-flight handlers), it must read these primitives
    // through the raw `impl` pointer. Putting them before `svr` guarantees they
    // outlive `~Server`'s pool join.
    std::mutex shutdownMutex;
    std::condition_variable shutdownCv;
    std::atomic<bool> shuttingDown{false};
    httplib::Server svr;
    std::thread thread;
    bool routes_installed = false;
    std::string bind_host = SmatchetDefaults::Mcp::kBindLocalhost;
    std::string auth_token;
    std::string tracker_domain;
    std::vector<std::string> export_fields;
    bool allow_lua_execution = false;
    /// Path to the instance.json written by OnStart and deleted by OnStop.
    std::string instanceJsonPath;
};

McpPlugin::McpPlugin(int port) : port_(port), impl_(new Impl()) {}

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
    return impl_->auth_token == cfgToken;
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
    return st.ListenPort != expectedPort || st.BindHost != expectedBind ||
           !AuthTokenMatches(cfg.McpAuthToken) || !LuaExecutionEnabledMatches(cfg.McpAllowLuaExecution);
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
    impl_->auth_token = cfg.McpAuthToken;
    impl_->allow_lua_execution = cfg.McpAllowLuaExecution;
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
            inst["pid"]  = static_cast<long long>(
#if defined(_WIN32)
                GetCurrentProcessId()
#else
                getpid()
#endif
            );
            inst["port"] = port_;
            std::FILE* f = std::fopen(instancePath.c_str(), "wb");
            if (f) {
                const std::string s = inst.dump();
                std::fwrite(s.data(), 1, s.size(), f);
                std::fclose(f);
            }
        } catch (...) {
            // Non-fatal — CLI can fall back to explicit flags.
        }
        impl_->instanceJsonPath = instancePath;
    }

    impl_->svr.set_payload_max_length(1u * 1024u * 1024u);
    impl_->svr.set_read_timeout(10, 0);
    impl_->svr.set_write_timeout(30, 0);
    impl_->svr.set_idle_interval(1, 0);

    if (!impl_->routes_installed) {
        impl_->routes_installed = true;
        auto authorize = [this](const httplib::Request& req, httplib::Response& res) -> bool {
            if (impl_->app != nullptr) {
                impl_->app->NotifyMcpClientHttpActivity();
            }
            if (impl_->auth_token.empty()) {
                if (!IsLoopbackAddress(req.remote_addr)) {
                    res.status = 403;
                    res.set_content("MCP access denied: localhost only.", "text/plain");
                    AppendMcpActivityLine(impl_->app, std::string("MCP: auth denied remote=") + req.remote_addr +
                                                         " status=403 reason=localhost_only");
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
        };

        impl_->svr.Get("/mcp/list_tickets", [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res)) {
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
        });

        // Attachment proxy:
        // Tracker attachment URLs generally require Basic Auth headers; Unreal's WebBrowser won't attach them.
        // We proxy through the MCP server (running in the same process) so Unreal can embed/load it.
        impl_->svr.Get("/mcp/attachment_proxy", [this, authorize](const httplib::Request& req,
                                                                       httplib::Response& res) {
            if (!authorize(req, res)) {
                return;
            }
            const std::string targetUrl = req.get_param_value("url");
            if (targetUrl.empty()) {
                res.status = 400;
                res.set_content("Missing `url` query parameter.", "text/plain");
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
            const auto resp = cpr::Get(cpr::Url{targetUrl}, headers, redirect, writeCb, cpr::ConnectTimeout{5000},
                                       cpr::Timeout{60000});
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
            } catch (...) {
                // Ignore; keep default contentType.
            }

            res.status = resp.status_code;
            res.set_content(bodyAccum, contentType);
        });

        impl_->svr.Get("/mcp/search", [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res)) {
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
        });

        impl_->svr.Get("/mcp/tools/list", [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res))
                return;
            LOG_TRACE("MCP: GET /mcp/tools/list remote=%s", req.remote_addr.c_str());
            nlohmann::json j = nlohmann::json::array();
            // Unified Command System — registered commands take precedence and define
            // canonical names. Legacy `list_active_tickets` / `search_active_tickets`
            // surface via the registry's alias table; we don't double-list them.
            const auto registryCommands = impl_->app->Commands().All();
            for (const auto& c : registryCommands) {
                j.push_back({{"name", c.Name},
                             {"description", c.Summary},
                             {"inputSchema", c.BuildJsonSchema()}});
            }
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
            const auto luaTools = impl_->app->GetLuaMcpTools();
            if (impl_->allow_lua_execution) {
                j.push_back(BuildRunLuaToolEntry());
            }
            std::transform(luaTools.begin(), luaTools.end(), std::back_inserter(j), [](const auto& t) {
                return nlohmann::json{{"name", t.name}, {"description", t.description}, {"inputSchema", t.parametersSchema}};
            });
#endif
            res.set_content(nlohmann::json{{"tools", j}}.dump(), "application/json");
        });

        impl_->svr.Post("/mcp/tools/call", [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res))
                return;
            const std::string remote = req.remote_addr;
            try {
                auto body = nlohmann::json::parse(req.body);
                std::string name = body.value("name", "");
                const nlohmann::json arguments = body.value("arguments", nlohmann::json::object());
                std::string paramsStr = arguments.dump();
                LOG_TRACE("MCP: REST POST /mcp/tools/call remote=%s tool=%s args_len=%zu body_len=%zu",
                          req.remote_addr.c_str(), name.c_str(), paramsStr.size(), req.body.size());
                std::string error;
                std::string result;
                // Unified Command System: try registry first. Always returns HTTP 200
                // with a canonical envelope {ok, command, data|error} in content[0].text —
                // even for structured errors (confirm-required, not-found, etc.). This lets
                // callers parse the envelope rather than treating every error as a transport fail.
                if (impl_->app->Commands().FindLocked(name) != nullptr) {
                    smatchet::cmd::CommandContext cctx;
                    cctx.App = impl_->app;
                    cctx.Source = smatchet::cmd::CommandSource::Mcp;
                    cctx.ConfirmedDestructive = arguments.value("__confirm", false);
                    cctx.DryRun = arguments.value("__dry_run", false);
                    cctx.TimeoutMs = arguments.value("__timeout_ms", 0);
                    smatchet::cmd::CommandResult cr =
                        impl_->app->Commands().Dispatch(name, arguments, cctx);
                    const nlohmann::json envelope = cr.ToWireJson(name, cctx.DryRun);
                    const std::string envelopeStr = envelope.dump();
                    LOG_TRACE("MCP: REST tools/call registry tool=%s ok=%d", name.c_str(), cr.Ok ? 1 : 0);
                    AppendMcpActivityLine(impl_->app,
                        "MCP: REST tools/call " + name +
                        (cr.Ok ? " ok" : " FAIL(" + cr.Error.Message.substr(0, 80) + ")") +
                        " remote=" + remote);
                    // Always HTTP 200: the envelope carries ok/error; isError=true only for
                    // genuine tool-call failures, not for structured command errors.
                    res.set_content(
                        nlohmann::json{{"content", {{{"type", "text"}, {"text", envelopeStr}}}}}.dump(),
                        "application/json");
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
                            result = impl_->app->ExecuteLuaSnippetForMcp(
                                args.value("code", std::string()), args.value("args", nlohmann::json::object()), error);
                        } else if (mode == "script") {
                            result = impl_->app->ExecuteLuaScriptForMcp(
                                args.value("script", std::string()), args.value("args", nlohmann::json::object()), error);
                        } else {
                            error = "run_lua requires mode='snippet' or mode='script'";
                        }
                    }
                } else {
                    // Check whether this name belongs to an mcp.register_tool Lua tool before
                    // trying ExecuteLuaMcpTool. If it's not a Lua MCP tool either, fall back to
                    // the registry for a structured unknown-command response with fuzzy suggestions.
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                    const auto luaTools = impl_->app->GetLuaMcpTools();
                    bool isLuaMcpTool = false;
                    for (const auto& lt : luaTools) {
                        if (lt.name == name) { isLuaMcpTool = true; break; }
                    }
                    if (isLuaMcpTool) {
                        result = impl_->app->ExecuteLuaMcpTool(name, paramsStr, error);
                    } else {
#endif
                        // Not in registry, not run_lua, not a Lua MCP tool.
                        // Dispatch through registry to get a canonical unknown-command envelope
                        // with fuzzy suggestions (e.g. "did you mean 'tickets.search_active'?").
                        smatchet::cmd::CommandContext cctx;
                        cctx.App = impl_->app;
                        cctx.Source = smatchet::cmd::CommandSource::Mcp;
                        smatchet::cmd::CommandResult cr =
                            impl_->app->Commands().Dispatch(name, arguments, cctx);
                        const nlohmann::json envelope = cr.ToWireJson(name, false);
                        res.set_content(
                            nlohmann::json{{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}}.dump(),
                            "application/json");
                        AppendMcpActivityLine(impl_->app,
                            "MCP: REST tools/call " + name + " unknown-command remote=" + remote);
                        return;
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                    }
#endif
                }
#else
                // No Lua: unknown name → structured unknown-command from registry.
                {
                    smatchet::cmd::CommandContext cctx;
                    cctx.App = impl_->app;
                    cctx.Source = smatchet::cmd::CommandSource::Mcp;
                    smatchet::cmd::CommandResult cr =
                        impl_->app->Commands().Dispatch(name, arguments, cctx);
                    const nlohmann::json envelope = cr.ToWireJson(name, false);
                    res.set_content(
                        nlohmann::json{{"content", {{{"type", "text"}, {"text", envelope.dump()}}}}}.dump(),
                        "application/json");
                    return;
                }
#endif
                // cppcheck-suppress knownConditionTrueFalse  // !error.empty() is condition-
                // dependent on SMATCHET_WITH_LUA_AUTOMATION; only "always true" under #else.
                if (!error.empty()) {
                    LOG_TRACE("MCP: REST tools/call error tool=%s %s", name.c_str(), error.c_str());
                    res.status = (name == "run_lua" && !impl_->allow_lua_execution) ? 404 : 500;
                    res.set_content(
                        nlohmann::json{{"isError", true}, {"content", {{{"type", "text"}, {"text", error}}}}}.dump(),
                        "application/json");
                    if (!(name == "run_lua" && !impl_->allow_lua_execution)) {
                        AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + (name.empty() ? "?" : name) +
                                                             " FAIL remote=" + remote + " err=" +
                                                             TruncateOneLine(error, 200));
                    }
                } else {
                    LOG_TRACE("MCP: REST tools/call ok tool=%s result_len=%zu", name.c_str(), result.size());
                    res.set_content(nlohmann::json{{"content", {{{"type", "text"}, {"text", result}}}}}.dump(),
                                    "application/json");
                    const std::string summary =
                        (name == "run_lua") ? BuildRunLuaSummary(arguments) : BuildToolCallSummary(name, arguments);
                    AppendMcpActivityLine(impl_->app, "MCP: REST tools/call " + (name.empty() ? "?" : name) +
                                                         " ok remote=" + remote + " " + summary);
                }
            } catch (const std::exception& e) {
                LOG_TRACE("MCP: REST tools/call parse_exception %s", e.what());
                res.status = 400;
                res.set_content(nlohmann::json{{"isError", true}, {"error", e.what()}}.dump(), "application/json");
                AppendMcpActivityLine(impl_->app, "MCP: REST tools/call FAIL remote=" + remote +
                                                     " err=parse error: " + TruncateOneLine(e.what(), 200));
            }
        });

        // --- Actual MCP JSON-RPC Specification (SSE) ---

        impl_->svr.Get(SmatchetDefaults::Mcp::kSsePath, [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res))
                return;

            // In a real implementation, we'd manage session IDs. For now, we use a single global endpoint.
            res.set_header("Content-Type", "text/event-stream");
            res.set_header("Cache-Control", "no-cache");
            res.set_header("Connection", "keep-alive");
            res.set_header("Access-Control-Allow-Origin", "*");

            std::string endpoint = "/mcp/messages";
            std::string event = "event: endpoint\ndata: " + endpoint + "\n\n";

            // Capture impl_ by raw pointer: cpp-httplib's svr.stop() in OnStop()
            // waits for in-flight handlers to return before destruction, so the
            // Impl outlives every chunked-content callback invocation.
            Impl* impl = impl_.get();
            res.set_chunked_content_provider(
                "text/event-stream",
                [event, impl](size_t offset, httplib::DataSink& sink) {
                    if (offset == 0) {
                        // Initial endpoint event. `sink.write` returns false on client disconnect.
                        return sink.write(event.data(), event.size());
                    }
                    // Wait up to 1s for shutdown; return early (closing the stream)
                    // when OnStop() flips shuttingDown. Without this, svr.stop()
                    // would block until every connected client's worker finished
                    // its current sleep — up to N seconds for N clients.
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

        // Streamable HTTP clients (e.g. Cursor) POST JSON-RPC to the same URL as the SSE endpoint; legacy clients
        // POST to /mcp/messages after reading the endpoint event from GET /mcp/sse.
        auto handleMcpJsonRpcPost = [this, authorize](const httplib::Request& req, httplib::Response& res) {
            if (!authorize(req, res))
                return;

            try {
                auto jreq = nlohmann::json::parse(req.body);
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
                    AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC initialize ok remote=") +
                                                         req.remote_addr + " protocol=" +
                                                         TruncateOneLine(negotiatedProtocol, 64));
                } else if (method == "tools/list") {
                    nlohmann::json toolList = nlohmann::json::array();

                    // Unified Command System — canonical command catalog. Legacy names
                    // `list_active_tickets` / `search_active_tickets` are aliases on the
                    // matching registry commands, so they continue to route through
                    // tools/call without needing a duplicate tools/list entry.
                    const auto registryCommands = impl_->app->Commands().All();
                    for (const auto& c : registryCommands) {
                        toolList.push_back({{"name", c.Name},
                                            {"description", c.Summary},
                                            {"inputSchema", c.BuildJsonSchema()}});
                    }

#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                    if (impl_->allow_lua_execution) {
                        toolList.push_back(BuildRunLuaToolEntry());
                    }
                    const auto tools = impl_->app->GetLuaMcpTools();
                    std::transform(tools.begin(), tools.end(), std::back_inserter(toolList), [](const auto& t) {
                        return nlohmann::json{
                            {"name", t.name}, {"description", t.description}, {"inputSchema", t.parametersSchema}};
                    });
#endif
                    jres["result"] = {{"tools", toolList}};
                    AppendMcpActivityLine(impl_->app, std::string("MCP: JSON-RPC tools/list ok remote=") +
                                                         req.remote_addr + " tools=" +
                                                         std::to_string(toolList.size()));
                } else if (method == "tools/call") {
                    auto params = jreq.value("params", nlohmann::json::object());
                    std::string name = params.value("name", "");
                    const std::string rpcRemote = req.remote_addr;
                    LOG_TRACE("MCP: JSON-RPC tools/call tool=%s", name.c_str());

                    if (impl_->app->Commands().FindLocked(name) != nullptr) {
                        const nlohmann::json args = params.value("arguments", nlohmann::json::object());
                        smatchet::cmd::CommandContext cctx;
                        cctx.App = impl_->app;
                        cctx.Source = smatchet::cmd::CommandSource::Mcp;
                        cctx.ConfirmedDestructive = args.value("__confirm", false);
                        cctx.DryRun = args.value("__dry_run", false);
                        cctx.TimeoutMs = args.value("__timeout_ms", 0);
                        smatchet::cmd::CommandResult cr =
                            impl_->app->Commands().Dispatch(name, args, cctx);
                        const nlohmann::json envelope = cr.ToWireJson(name, cctx.DryRun);
                        // MCP `result.content[0].text` carries the envelope as a string —
                        // that's the standard MCP shape and what hosts know to parse. The
                        // envelope itself stays the canonical JSON contract for agents.
                        jres["result"] = {
                            {"content", {{{"type", "text"}, {"text", envelope.dump()}}}}};
                        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " + name +
                                                             (cr.Ok ? " ok" : " FAIL") +
                                                             " remote=" + rpcRemote);
                    } else if (name == "run_lua") {
#if defined(SMATCHET_WITH_LUA_AUTOMATION)
                        const nlohmann::json luaArgs = params.value("arguments", nlohmann::json::object());
                        if (!impl_->allow_lua_execution) {
                            jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
                            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua FAIL remote=" +
                                                                 rpcRemote + " err=disabled by configuration");
                        } else {
                            std::string error;
                            std::string result;
                            const std::string mode = luaArgs.value("mode", "");
                            if (mode == "snippet") {
                                result = impl_->app->ExecuteLuaSnippetForMcp(luaArgs.value("code", std::string()),
                                                                             luaArgs.value("args", nlohmann::json::object()),
                                                                             error);
                            } else if (mode == "script") {
                                result = impl_->app->ExecuteLuaScriptForMcp(luaArgs.value("script", std::string()),
                                                                            luaArgs.value("args", nlohmann::json::object()),
                                                                            error);
                            } else {
                                error = "run_lua requires mode='snippet' or mode='script'";
                            }
                            if (!error.empty()) {
                                jres["error"] = {{"code", -32603}, {"message", error}};
                            } else {
                                jres["result"] = {{"content", {{{"type", "text"}, {"text", result}}}}};
                            }
                            if (jres.contains("error")) {
                                AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua FAIL remote=" +
                                                                     rpcRemote + " err=" +
                                                                     ExtractJsonRpcErrorMessage(jres, 256));
                            } else {
                                AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua ok remote=" +
                                                                     rpcRemote + " " + BuildRunLuaSummary(luaArgs));
                            }
                        }
#else
                        jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
                        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call run_lua FAIL remote=" + rpcRemote +
                                                             " err=Lua automation disabled");
#endif
                    } else if (name == "list_active_tickets") {
                        const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
                        nlohmann::json ticketIds = nlohmann::json::array();
                        std::transform(ticketsPtr->begin(), ticketsPtr->end(), std::back_inserter(ticketIds),
                                       [](const auto& t) { return t.id; });
                        jres["result"] = {
                            {"content", {{{"type", "text"}, {"text", "Active Tickets: " + ticketIds.dump()}}}}};
                        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call list_active_tickets ok remote=" +
                                                             rpcRemote + " count=" + std::to_string(ticketIds.size()));
                    } else if (name == "search_active_tickets") {
                        auto args = params.value("arguments", nlohmann::json::object());
                        std::string query = args.value("query", "");
                        const auto ticketsPtr = impl_->app->GetActiveTicketsSnapshot();
                        nlohmann::json matches = nlohmann::json::array();
                        for (const auto& t : *ticketsPtr) {
                            const bool hit =
                                (t.id.find(query) != std::string::npos) ||
                                std::any_of(t.fieldValues.begin(), t.fieldValues.end(), [&](const auto& kv) {
                                    return kv.second.find(query) != std::string::npos;
                                });
                            if (hit) {
                                matches.push_back(t.id);
                            }
                        }
                        jres["result"] = {
                            {"content",
                             {{{"type", "text"}, {"text", "Search Results for '" + query + "': " + matches.dump()}}}}};
                        AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call search_active_tickets ok remote=" +
                                                             rpcRemote + " query=" + TruncateOneLine(query, 80) +
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
                            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " +
                                                                 (name.empty() ? "?" : name) + " FAIL remote=" +
                                                                 rpcRemote + " err=" +
                                                                 ExtractJsonRpcErrorMessage(jres, 256));
                        } else {
                            AppendMcpActivityLine(impl_->app, "MCP: JSON-RPC tools/call " +
                                                                 (name.empty() ? "?" : name) + " ok remote=" +
                                                                 rpcRemote + " " + BuildToolCallSummary(name, argObj));
                        }
                    }
                } else {
                    jres["error"] = {{"code", -32601}, {"message", "Method not found"}};
                    AppendMcpActivityLine(impl_->app,
                                          std::string("MCP: JSON-RPC FAIL remote=") + req.remote_addr + " method=" +
                                              TruncateOneLine(method, 120) + " err=method not found");
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

    if (impl_->thread.joinable()) {
        return;
    }

    AppController* appPtr = &app;
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
                std::cerr << "MCP server: bind_to_port failed for " << impl_->bind_host << ":" << port_ << std::endl;
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
            std::cerr << "MCP server thread error: " << e.what() << std::endl;
        } catch (...) {
            if (appPtr != nullptr) {
                appPtr->AppendMcpActivity("MCP: listen thread unknown exception.");
            }
            std::cerr << "MCP server thread unknown error" << std::endl;
        }
    });
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
        std::remove(impl_->instanceJsonPath.c_str());  // best-effort; ignore failure
        impl_->instanceJsonPath.clear();
    }
}







