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
#include <httplib.h>
#include <cpr/cpr.h>
#include <nlohmann/json.hpp>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <thread>
#include <algorithm>
#include <cctype>

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

bool LooksLikeHttpUrl(const std::string& url) {
    return url.find("http://") == 0 || url.find("https://") == 0;
}

std::string ToLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
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
        value = value.substr(0, slashPos);
    }
    const size_t atPos = value.rfind('@');
    if (atPos != std::string::npos) {
        value = value.substr(atPos + 1);
    }
    const size_t colonPos = value.find(':');
    if (colonPos != std::string::npos) {
        value = value.substr(0, colonPos);
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
    return lowered == "127.0.0.1" || lowered == "::1" || lowered == "localhost" ||
           lowered == "::ffff:127.0.0.1";
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

bool IsAllowedAttachmentHost(const std::string& host, const std::string& jiraDomain) {
    if (host.empty() || jiraDomain.empty()) {
        return false;
    }
    if (host == jiraDomain) {
        return true;
    }
    const std::string jiraSuffix = "." + jiraDomain;
    if (host.size() > jiraSuffix.size() &&
        host.compare(host.size() - jiraSuffix.size(), jiraSuffix.size(), jiraSuffix) == 0) {
        return true;
    }
    return host == "api.media.atlassian.com";
}
} // namespace

struct McpPlugin::Impl {
    AppController* app = nullptr;
    httplib::Server svr;
    std::thread thread;
    bool routes_installed = false;
    std::string bind_host = "127.0.0.1";
    std::string auth_token;
    std::string jira_domain;
    std::vector<std::string> export_fields;
};

McpPlugin::McpPlugin(int port) : port_(port), impl_(new Impl()) {}

McpPlugin::~McpPlugin() { OnStop(); }

void McpPlugin::OnStart(AppController& app) {
    if (!impl_) {
        return;
    }
    impl_->app = &app;
    const JiraConfig cfg = ConfigManager::Load();
    impl_->bind_host = cfg.McpAllowRemote ? "0.0.0.0" : "127.0.0.1";
    impl_->auth_token = cfg.McpAuthToken;
    impl_->jira_domain = NormalizeDomain(cfg.Domain);
    if (cfg.McpExportFields.empty()) {
        impl_->export_fields = {
            "summary", "status", "priority", "assignee",
            "updated", "created", "labels", "issuetype",
        };
    } else {
        impl_->export_fields = cfg.McpExportFields;
    }

    // Limit request body and slow-read abuse when exposed on LAN.
    impl_->svr.set_payload_max_length(1u * 1024u * 1024u);
    impl_->svr.set_read_timeout(10, 0);
    impl_->svr.set_write_timeout(30, 0);
    impl_->svr.set_idle_interval(1, 0);

    if (!impl_->routes_installed) {
        impl_->routes_installed = true;
        auto authorize = [this](const httplib::Request& req, httplib::Response& res) -> bool {
            if (impl_->auth_token.empty()) {
                if (!IsLoopbackAddress(req.remote_addr)) {
                    res.status = 403;
                    res.set_content("MCP access denied: localhost only.", "text/plain");
                    return false;
                }
                return true;
            }
            const auto token = req.get_header_value("X-Smatchet-Token");
            if (!ConstantTimeStringEquals(token, impl_->auth_token)) {
                res.status = 401;
                res.set_header("WWW-Authenticate", "Token realm=\"Smatchet MCP\"");
                res.set_content("Missing or invalid MCP token.", "text/plain");
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
        // Jira attachment URLs generally require Basic Auth headers; Unreal's WebBrowser won't attach them.
        // We proxy through the MCP server (running in the same process) so Unreal can embed/load it.
        impl_->svr.Get("/mcp/attachment_proxy", [this, authorize, cfg](const httplib::Request& req, httplib::Response& res) {
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
            if (!IsAllowedAttachmentHost(targetHost, impl_->jira_domain)) {
                res.status = 403;
                res.set_content("Attachment host is not allowlisted.", "text/plain");
                return;
            }

            if (cfg.ApiToken.empty() || cfg.Domain.empty()) {
                res.status = 500;
                res.set_content("Missing Jira configuration (email/token/domain).", "text/plain");
                return;
            }

            const std::string basicCred = cfg.Email + ":" + cfg.ApiToken;
            const std::string authHeader = "Basic " + Base64Encode(basicCred);

            cpr::Header headers{
                {"Accept", "*/*"},
                {"Authorization", authHeader},
                {"User-Agent", "Smatchet/1.0 Jira-Attachment-Proxy"}
            };
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
            const auto resp = cpr::Get(cpr::Url{targetUrl},
                                       headers,
                                       redirect,
                                       writeCb,
                                       cpr::ConnectTimeout{5000},
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
                        if (it == t.fieldValues.end()) continue;
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
    }

    if (impl_->thread.joinable()) {
        return;
    }

    impl_->thread = std::thread([this]() {
        try {
            impl_->svr.listen(impl_->bind_host.c_str(), port_);
        } catch (const std::exception& e) {
            std::cerr << "MCP server thread error: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "MCP server thread unknown error" << std::endl;
        }
    });
}

void McpPlugin::OnStop() {
    if (!impl_ || !impl_->thread.joinable()) {
        return;
    }
    impl_->svr.stop();
    impl_->thread.join();
}
