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
} // namespace

struct McpPlugin::Impl {
    AppController* app = nullptr;
    httplib::Server svr;
    std::thread thread;
    bool routes_installed = false;
};

McpPlugin::McpPlugin(int port) : port_(port), impl_(new Impl()) {}

McpPlugin::~McpPlugin() { OnStop(); }

void McpPlugin::OnStart(AppController& app) {
    if (!impl_) {
        return;
    }
    impl_->app = &app;

    if (!impl_->routes_installed) {
        impl_->routes_installed = true;
        impl_->svr.Get("/mcp/list_tickets", [this](const httplib::Request&, httplib::Response& res) {
            const auto& tickets = impl_->app->GetActiveTickets();
            nlohmann::json j = nlohmann::json::array();
            for (const auto& t : tickets) {
                j.push_back({{"id", t.id}, {"fields", t.fieldValues}});
            }
            res.set_content(j.dump(), "application/json");
        });

        // Attachment proxy:
        // Jira attachment URLs generally require Basic Auth headers; Unreal's WebBrowser won't attach them.
        // We proxy through the MCP server (running in the same process) so Unreal can embed/load it.
        impl_->svr.Get("/mcp/attachment_proxy", [this](const httplib::Request& req, httplib::Response& res) {
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

            const JiraConfig cfg = ConfigManager::Load();
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
            cpr::Redirect redirect(true, true);

            const auto resp = cpr::Get(cpr::Url{targetUrl}, headers, redirect);
            if (resp.error.code != cpr::ErrorCode::OK || resp.status_code < 200 || resp.status_code >= 300) {
                res.status = 502;
                res.set_content(resp.text, "text/plain");
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
            res.set_content(resp.text, contentType);
        });

        impl_->svr.Get("/mcp/search", [this](const httplib::Request& req, httplib::Response& res) {
            std::string query = req.get_param_value("q");
            const auto& tickets = impl_->app->GetActiveTickets();
            nlohmann::json j = nlohmann::json::array();
            for (const auto& t : tickets) {
                bool matches = (t.id.find(query) != std::string::npos);
                if (!matches) {
                    for (const auto& kv : t.fieldValues) {
                        if (kv.first.find(query) != std::string::npos ||
                            kv.second.find(query) != std::string::npos) {
                            matches = true;
                            break;
                        }
                    }
                }
                if (matches) {
                    j.push_back({{"id", t.id}, {"fields", t.fieldValues}});
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
            impl_->svr.listen("0.0.0.0", port_);
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
