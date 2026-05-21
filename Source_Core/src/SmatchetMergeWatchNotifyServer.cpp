#include "SmatchetMergeWatchNotifyServer.h"

#include "AppController.h"
#include "Logger.h"
#include "SmatchetToast.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>

namespace {

// Per Phase 4a's NOTIFY_STATES set in scripts/dev/merge-watcher.py.
// Server-side allow-list — reject any other state string so a compromised
// (or buggy) caller can't inject arbitrary toast text under an arbitrary
// state label.
const std::set<std::string>& KnownStates() {
    static const std::set<std::string> s = {
        "CI_FAIL",
        "GH_API_DOWN",
        "PR_CLOSED_OR_MERGED",
        "PAGINATION_OVERFLOW",
        "TIMEOUT",
        "TRIAGE_BUDGET_EXHAUSTED",
    };
    return s;
}

// Cap message length so a runaway payload can't flood the toast surface OR
// the dispatcher queue. 500 chars matches the per-line cap in
// SmatchetToastManager::Render's wrap path.
constexpr std::size_t kMaxMessageBytes = 500;

// Strip control characters (< 0x20 except \t \r \n) — ImGui renders text
// verbatim + control chars can hose layout. Doesn't escape HTML / markdown
// because ToastManager doesn't render either.
std::string SanitizeText(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (uc < 0x20 && uc != '\t' && uc != '\n' && uc != '\r') {
            out.push_back('?');
        } else {
            out.push_back(c);
        }
    }
    if (out.size() > kMaxMessageBytes) {
        out.resize(kMaxMessageBytes);
        out.append("...");
    }
    return out;
}

ToastType TypeForState(const std::string& state) {
    if (state == "CI_FAIL" || state == "PAGINATION_OVERFLOW" ||
        state == "TRIAGE_BUDGET_EXHAUSTED") {
        return ToastType::Error;
    }
    if (state == "PR_CLOSED_OR_MERGED") {
        return ToastType::Success;
    }
    return ToastType::Warning;
}

} // namespace

SmatchetMergeWatchNotifyServer::SmatchetMergeWatchNotifyServer() : running_(false) {}

SmatchetMergeWatchNotifyServer::~SmatchetMergeWatchNotifyServer() { Stop(); }

bool SmatchetMergeWatchNotifyServer::Start(AppController& app, std::uint16_t port) {
    if (running_.load(std::memory_order_acquire)) {
        LOG_WARN("SmatchetMergeWatchNotifyServer::Start: already running");
        return false;
    }
    server_ = std::unique_ptr<httplib::Server>(new httplib::Server());

    // Capture dispatcher by reference — outlives the server per AppController's
    // dtor ordering (Stop() runs before mainThreadDispatcher.BeginShutdown).
    MainThreadDispatcher& dispatcher = app.mainThreadDispatcher;

    server_->Post("/merge-watch/notify",
                  [&dispatcher](const httplib::Request& req, httplib::Response& res) {
                      // Pillar 3: parse + validate before any toast dispatch.
                      nlohmann::json j;
                      try {
                          j = nlohmann::json::parse(req.body);
                      } catch (const nlohmann::json::parse_error& e) {
                          res.status = 400;
                          res.set_content(std::string("{\"error\":\"bad JSON: ") + e.what() + "\"}",
                                          "application/json");
                          return;
                      }
                      if (!j.is_object() || !j.contains("pr") || !j.contains("state") ||
                          !j.contains("message")) {
                          res.status = 400;
                          res.set_content(
                              "{\"error\":\"missing required field (pr / state / message)\"}",
                              "application/json");
                          return;
                      }
                      if (!j["pr"].is_number_integer() || !j["state"].is_string() ||
                          !j["message"].is_string()) {
                          res.status = 400;
                          res.set_content("{\"error\":\"field type mismatch\"}",
                                          "application/json");
                          return;
                      }
                      const int pr = j["pr"].get<int>();
                      const std::string state = j["state"].get<std::string>();
                      const std::string rawMessage = j["message"].get<std::string>();
                      if (KnownStates().find(state) == KnownStates().end()) {
                          res.status = 400;
                          res.set_content(
                              "{\"error\":\"unknown state (allowed: CI_FAIL, GH_API_DOWN, "
                              "PR_CLOSED_OR_MERGED, PAGINATION_OVERFLOW, TIMEOUT, "
                              "TRIAGE_BUDGET_EXHAUSTED)\"}",
                              "application/json");
                          return;
                      }
                      const std::string message = SanitizeText(rawMessage);
                      const ToastType type = TypeForState(state);

                      // Post to UI thread — dispatcher is thread-safe + bounded.
                      const std::string title =
                          std::string("Smatchet watcher · PR #") + std::to_string(pr) + " · " + state;
                      dispatcher.PostToMainThread(
                          [title, message, type]() {
                              SmatchetToastManager::Instance().Push(title, message, type, 6000);
                          });

                      res.status = 200;
                      res.set_content("{\"ok\":true}", "application/json");
                  });

    // Health probe — daemon's smatchet-notify.sh can use to detect endpoint
    // availability without firing a real notification.
    server_->Get("/merge-watch/health",
                 [](const httplib::Request&, httplib::Response& res) {
                     res.status = 200;
                     res.set_content("{\"ok\":true}", "application/json");
                 });

    // Bind 127.0.0.1 ONLY. cpp-httplib's bind_to_port returns true on success.
    if (!server_->bind_to_port("127.0.0.1", port)) {
        LOG_WARN("SmatchetMergeWatchNotifyServer::Start: bind 127.0.0.1:%u failed (in use?)",
                 static_cast<unsigned>(port));
        server_.reset();
        return false;
    }

    running_.store(true, std::memory_order_release);
    // Spawn listen-loop thread. listen_after_bind blocks until Stop().
    listenThread_.reset(new std::thread([this]() {
        if (server_) {
            server_->listen_after_bind();
        }
        running_.store(false, std::memory_order_release);
    }));
    LOG_INFO("SmatchetMergeWatchNotifyServer: listening on 127.0.0.1:%u", static_cast<unsigned>(port));
    return true;
}

void SmatchetMergeWatchNotifyServer::Stop() {
    if (server_) {
        server_->stop();
    }
    if (listenThread_ && listenThread_->joinable()) {
        listenThread_->join();
    }
    listenThread_.reset();
    server_.reset();
    running_.store(false, std::memory_order_release);
}
