// Slice 2 of docs/plans/active/deterministic-jira-test-backend.md — Jira fixture
// parser/configurator implementation. See JiraFakeTrackerFixture.h for the contract.

#include "JiraFakeTrackerFixture.h"

#include "JiraIssueMappingPure.h"

#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace smatchet_tests {

namespace {

TrackerReachabilityProbeKind ParseReachabilityKind(const std::string& kind) {
    if (kind == "AuthenticatedReachable")
        return TrackerReachabilityProbeKind::AuthenticatedReachable;
    if (kind == "ReachableAuthOrConfigError")
        return TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
    if (kind == "ServiceUnavailable")
        return TrackerReachabilityProbeKind::ServiceUnavailable;
    return TrackerReachabilityProbeKind::TransportDown;
}

} // namespace

JiraFakeTrackerFixture JiraFakeTrackerFixture::LoadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        throw std::runtime_error("JiraFakeTrackerFixture: cannot open fixture file: " + path);
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return LoadFromString(content);
}

JiraFakeTrackerFixture JiraFakeTrackerFixture::LoadFromString(const std::string& json) {
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json);
    } catch (const std::exception& ex) {
        throw std::runtime_error(std::string("JiraFakeTrackerFixture: JSON parse failed: ") + ex.what());
    }
    return ParseJson(root);
}

JiraFakeTrackerFixture JiraFakeTrackerFixture::ParseJson(const nlohmann::json& root) {
    JiraFakeTrackerFixture fixture;

    // Reachability
    if (root.contains("reachability") && root["reachability"].is_object()) {
        const auto& r = root["reachability"];
        fixture.reachabilityKind_ = ParseReachabilityKind(r.value("kind", std::string("AuthenticatedReachable")));
        fixture.reachabilityDiagnostic_ = r.value("diagnostic", std::string());
    }

    // Fetch scripts
    if (root.contains("fetches") && root["fetches"].is_array()) {
        for (const auto& fetchEntry : root["fetches"]) {
            JiraFixtureFetch fetch;
            fetch.FullSyncCompleted = fetchEntry.value("fullSyncCompleted", true);
            fetch.Warning = fetchEntry.value("warning", std::string());
            fetch.FetchError = fetchEntry.value("fetchError", std::string());

            // Prefer jiraSearchPages (runs through the shared mapper) over cachedTickets.
            if (fetchEntry.contains("jiraSearchPages") && fetchEntry["jiraSearchPages"].is_array()) {
                const std::vector<std::string> selectedFields = fetchEntry.value(
                    "selectedFields", std::vector<std::string>{"summary", "status", "priority", "assignee"});
                auto noComments = [](const std::string&, nlohmann::json&) -> bool { return false; };
                for (const auto& page : fetchEntry["jiraSearchPages"]) {
                    if (!page.contains("issues") || !page["issues"].is_array()) {
                        continue;
                    }
                    for (const auto& issue : page["issues"]) {
                        smatchet::jira::AppendCachedTicketFromJiraSearchIssue(issue, selectedFields, noComments,
                                                                              fetch.Tickets);
                    }
                }
            } else if (fetchEntry.contains("cachedTickets") && fetchEntry["cachedTickets"].is_array()) {
                for (const auto& ct : fetchEntry["cachedTickets"]) {
                    CachedTicket ticket;
                    ticket.id = ct.value("id", std::string());
                    if (ct.contains("fields") && ct["fields"].is_object()) {
                        for (auto it = ct["fields"].begin(); it != ct["fields"].end(); ++it) {
                            if (it.value().is_string()) {
                                ticket.fieldValues[it.key()] = it.value().get<std::string>();
                            } else {
                                ticket.fieldValues[it.key()] = it.value().dump();
                            }
                        }
                    }
                    fetch.Tickets.push_back(std::move(ticket));
                }
            }
            fixture.fetches_.push_back(std::move(fetch));
        }
    }

    // Mutation scripts
    if (root.contains("mutations") && root["mutations"].is_object()) {
        const auto& m = root["mutations"];
        if (m.contains("updateIssueFields") && m["updateIssueFields"].is_array()) {
            for (const auto& entry : m["updateIssueFields"]) {
                bool ok = entry.value("ok", true);
                std::string err = entry.value("error", std::string());
                fixture.updateIssueFieldsReplies_.emplace_back(ok, err);
            }
        }
        if (m.contains("createIssue") && m["createIssue"].is_array()) {
            for (const auto& entry : m["createIssue"]) {
                bool ok = entry.value("ok", true);
                std::string keyOrErr = ok ? entry.value("issueKey", std::string("FIXTURE-1"))
                                          : entry.value("error", std::string("fixture error"));
                fixture.createIssueReplies_.emplace_back(ok, keyOrErr);
            }
        }
    }

    return fixture;
}

void JiraFakeTrackerFixture::Configure(FakeTrackerClient& client) const {
    client.SetReachabilityResult(reachabilityKind_, reachabilityDiagnostic_);

    for (const auto& fetch : fetches_) {
        client.EnqueueFetchResult(fetch.Tickets, fetch.FullSyncCompleted, fetch.FetchError, fetch.Warning);
    }

    for (const auto& reply : updateIssueFieldsReplies_) {
        if (reply.first) {
            client.EnqueueUpdateIssueFieldsSuccess();
        } else {
            client.EnqueueUpdateIssueFieldsFailure(reply.second);
        }
    }

    for (const auto& reply : createIssueReplies_) {
        if (reply.first) {
            client.EnqueueCreateIssueSuccess(reply.second);
        } else {
            client.EnqueueCreateIssueFailure(reply.second);
        }
    }
}

std::unique_ptr<FakeTrackerClient> JiraFakeTrackerFixture::CreateClient() const {
    auto client = std::unique_ptr<FakeTrackerClient>(new FakeTrackerClient("Jira"));
    Configure(*client);
    return client;
}

} // namespace smatchet_tests
