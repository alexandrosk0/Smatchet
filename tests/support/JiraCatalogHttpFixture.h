#ifndef SMATCHET_TESTS_JIRA_CATALOG_HTTP_FIXTURE_H
#define SMATCHET_TESTS_JIRA_CATALOG_HTTP_FIXTURE_H

// Slice 0 WS2 of docs/plans/multi-grid-tabs.md — real catalog-BUILD fixture.
// Closes the test.md P2 (2026-06-01): `JiraFakeTrackerFixture` injects a pre-built
// `TrackerFieldCatalogResult` and bypasses the live `FetchFieldCatalog` assembly, so
// the whole enrichment + classification pipeline (the PR #672 "components rendered
// as text" bug class) was fixture-invisible. This fixture spins an in-process
// `httplib::Server` on a loopback ephemeral port with scripted Jira REST JSON
// bodies, so a real `JiraClient` driven at `http://127.0.0.1:<port>` exercises the
// production catalog-build path end-to-end over real cpr HTTP:
//   FetchAndParseFieldList → EnrichGlobalCatalogFields → EnrichFromCreateMeta
//   (+ MergeProjectComponentsFromEndpoint paging) → EnrichIssueTypeFromProject →
//   SortComponentCatalog / family re-classification.
//
// Usage:
//   JiraCatalogHttpFixture fx;
//   fx.ScriptJson("/rest/api/3/field", fieldsArray);
//   JiraClient client;
//   client.FetchFieldCatalog(fx.Config(), "BLU", fields, components, meta, error);
// Unscripted routes return 404 (the production code logs WARN and degrades, which
// is itself part of the characterized behaviour).

#include "ConfigManager.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

class JiraCatalogHttpFixture {
  public:
    /// Handler signature for dynamic routes (paged endpoints that branch on
    /// query params). Receives the request, returns the JSON body to serve.
    typedef std::function<nlohmann::json(const httplib::Request&)> JsonHandler;

    JiraCatalogHttpFixture() {
        server_.Get(".*", [this](const httplib::Request& req, httplib::Response& res) { Dispatch(req, res); });
        port_ = server_.bind_to_any_port("127.0.0.1");
        serverThread_ = std::thread([this]() { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~JiraCatalogHttpFixture() {
        server_.stop();
        if (serverThread_.joinable()) {
            serverThread_.join();
        }
    }

    JiraCatalogHttpFixture(const JiraCatalogHttpFixture&) = delete;
    JiraCatalogHttpFixture& operator=(const JiraCatalogHttpFixture&) = delete;

    /// Serve a fixed JSON body (HTTP 200) for an exact request path (query
    /// string excluded — Jira paging params are ignored by fixed routes).
    void ScriptJson(const std::string& path, const nlohmann::json& body) {
        std::lock_guard<std::mutex> lock(mutex_);
        fixedRoutes_[path] = body.dump();
    }

    /// Serve a scripted handler for an exact request path — used by paged
    /// endpoints that need to branch on `startAt` etc.
    void ScriptHandler(const std::string& path, JsonHandler handler) {
        std::lock_guard<std::mutex> lock(mutex_);
        dynamicRoutes_[path] = std::move(handler);
    }

    /// Force an HTTP status (with an empty JSON-object body) for an exact path.
    void ScriptStatus(const std::string& path, int status) {
        std::lock_guard<std::mutex> lock(mutex_);
        statusRoutes_[path] = status;
    }

    /// Number of requests whose path matched exactly. Counts every hit,
    /// scripted or not.
    int RequestCount(const std::string& path) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::map<std::string, int>::const_iterator it = requestCounts_.find(path);
        return it == requestCounts_.end() ? 0 : it->second;
    }

    /// A TrackerConfig pointed at the loopback server with dummy credentials
    /// (EnsureTrackerAuthConfig only requires Domain + ApiToken to be non-empty).
    TrackerConfig Config() const {
        TrackerConfig cfg;
        cfg.Domain = "http://127.0.0.1:" + std::to_string(port_);
        cfg.Email = "fixture@test.invalid";
        cfg.ApiToken = "fixture-token";
        cfg.TrackerType = "Jira";
        return cfg;
    }

    int Port() const { return port_; }

  private:
    void Dispatch(const httplib::Request& req, httplib::Response& res) {
        // Copy handler state under the lock, run the (possibly slow) handler outside it.
        JsonHandler handler;
        std::string fixedBody;
        bool haveFixed = false;
        int forcedStatus = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++requestCounts_[req.path];
            const std::map<std::string, int>::const_iterator sIt = statusRoutes_.find(req.path);
            if (sIt != statusRoutes_.end()) {
                forcedStatus = sIt->second;
            }
            const std::map<std::string, JsonHandler>::const_iterator dIt = dynamicRoutes_.find(req.path);
            if (dIt != dynamicRoutes_.end()) {
                handler = dIt->second;
            }
            const std::map<std::string, std::string>::const_iterator fIt = fixedRoutes_.find(req.path);
            if (fIt != fixedRoutes_.end()) {
                fixedBody = fIt->second;
                haveFixed = true;
            }
        }
        if (forcedStatus != 0) {
            res.status = forcedStatus;
            res.set_content("{}", "application/json");
            return;
        }
        if (handler) {
            res.status = 200;
            res.set_content(handler(req).dump(), "application/json");
            return;
        }
        if (haveFixed) {
            res.status = 200;
            res.set_content(fixedBody, "application/json");
            return;
        }
        res.status = 404;
        res.set_content("{}", "application/json");
    }

    httplib::Server server_;
    std::thread serverThread_;
    int port_ = 0;

    mutable std::mutex mutex_;
    std::map<std::string, std::string> fixedRoutes_;
    std::map<std::string, JsonHandler> dynamicRoutes_;
    std::map<std::string, int> statusRoutes_;
    std::map<std::string, int> requestCounts_;
};

#endif // SMATCHET_TESTS_JIRA_CATALOG_HTTP_FIXTURE_H
