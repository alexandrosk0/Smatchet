// Slice 2 of deterministic-jira-test-backend — Jira fixture
// parser/configurator implementation. See JiraFakeTrackerFixture.h for the contract.

#include "JiraFakeTrackerFixture.h"

#include "FakeNetworkSwitch.h"
#include "ITrackerCollaboration.h"
#include "JiraIssueMappingPure.h"
#include "Tracker/TrackerFieldSchema.h"

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
    if (kind == "TransportDown")
        return TrackerReachabilityProbeKind::TransportDown;
    throw std::runtime_error("JiraFakeTrackerFixture: unknown reachability kind: " + kind);
}

FakeNetworkMode ParseNetworkMode(const std::string& mode) {
    if (mode == "Up")
        return FakeNetworkMode::Up;
    if (mode == "TransportDown")
        return FakeNetworkMode::TransportDown;
    if (mode == "ServiceUnavailable")
        return FakeNetworkMode::ServiceUnavailable;
    throw std::runtime_error("JiraFakeTrackerFixture: unknown network mode: " + mode);
}

TrackerFieldFamily ParseFieldFamily(const std::string& family) {
    if (family == "Text")
        return TrackerFieldFamily::Text;
    if (family == "Number")
        return TrackerFieldFamily::Number;
    if (family == "Date")
        return TrackerFieldFamily::Date;
    if (family == "DateTime")
        return TrackerFieldFamily::DateTime;
    if (family == "Labels")
        return TrackerFieldFamily::Labels;
    if (family == "UserSingle")
        return TrackerFieldFamily::UserSingle;
    if (family == "UserMulti")
        return TrackerFieldFamily::UserMulti;
    if (family == "SelectSingle")
        return TrackerFieldFamily::SelectSingle;
    if (family == "SelectMulti")
        return TrackerFieldFamily::SelectMulti;
    if (family == "CascadingSelect")
        return TrackerFieldFamily::CascadingSelect;
    if (family == "StructuredSingle")
        return TrackerFieldFamily::StructuredSingle;
    if (family == "StructuredMulti")
        return TrackerFieldFamily::StructuredMulti;
    if (family == "Sprint")
        return TrackerFieldFamily::Sprint;
    if (family == "Status")
        return TrackerFieldFamily::Status;
    if (family == "IssueType")
        return TrackerFieldFamily::IssueType;
    return TrackerFieldFamily::Unknown;
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
                ScriptedReply r;
                r.Ok = entry.value("ok", true);
                r.Error = entry.value("error", std::string());
                fixture.updateIssueFieldsReplies_.push_back(std::move(r));
            }
        }
        if (m.contains("createIssue") && m["createIssue"].is_array()) {
            for (const auto& entry : m["createIssue"]) {
                ScriptedReply r;
                r.Ok = entry.value("ok", true);
                r.IssueKey = r.Ok ? entry.value("issueKey", std::string("FIXTURE-1")) : std::string();
                r.Error = r.Ok ? std::string() : entry.value("error", std::string("fixture error"));
                fixture.createIssueReplies_.push_back(std::move(r));
            }
        }
    }

    // Network mode (optional). Unknown modes throw, like unknown reachability kinds.
    if (root.contains("network") && root["network"].is_object()) {
        fixture.hasNetworkMode_ = true;
        fixture.networkMode_ = ParseNetworkMode(root["network"].value("mode", std::string("Up")));
    }

    // Field catalog from the existing "catalog.fields" key (scripted on the client only when non-empty).
    if (root.contains("catalog") && root["catalog"].is_object()) {
        const auto& catalog = root["catalog"];
        if (catalog.contains("fields") && catalog["fields"].is_array()) {
            for (const auto& fieldJson : catalog["fields"]) {
                TrackerField field;
                field.Id = fieldJson.value("id", std::string());
                field.Name = fieldJson.value("name", std::string());
                field.Type = "option";
                field.Family = ParseFieldFamily(fieldJson.value("family", std::string("Unknown")));

                // Parse options (status field options, etc.)
                if (fieldJson.contains("options") && fieldJson["options"].is_array()) {
                    for (const auto& optJson : fieldJson["options"]) {
                        TrackerFieldOption opt;
                        opt.Id = optJson.value("id", std::string());
                        opt.Value = optJson.value("value", std::string());
                        field.AllowedValues.push_back(opt.Value);
                        field.AllowedValueOptions.push_back(std::move(opt));
                    }
                }
                fixture.fields_.push_back(std::move(field));
            }
        }
    }

    // Issue transitions (optional)
    if (root.contains("transitions") && root["transitions"].is_object()) {
        for (auto it = root["transitions"].begin(); it != root["transitions"].end(); ++it) {
            const std::string& issueKey = it.key();
            if (it.value().is_array()) {
                std::vector<TrackerFieldOption> transitions;
                for (const auto& transJson : it.value()) {
                    TrackerFieldOption trans;
                    trans.Id = transJson.value("id", std::string());
                    trans.Value = transJson.value("name", std::string());
                    transitions.push_back(std::move(trans));
                }
                fixture.issueTransitionsByIssueId_[issueKey] = std::move(transitions);
            }
        }
    }

    // Issue comments (optional)
    if (root.contains("comments") && root["comments"].is_object()) {
        for (auto it = root["comments"].begin(); it != root["comments"].end(); ++it) {
            const std::string& issueKey = it.key();
            if (it.value().is_array()) {
                std::vector<TrackerIssueComment> comments;
                for (const auto& commentJson : it.value()) {
                    TrackerIssueComment comment;
                    comment.Id = commentJson.value("id", std::string());
                    comment.Author = commentJson.value("author", std::string());
                    comment.Body = commentJson.value("body", std::string());
                    comment.CreatedAtSec = commentJson.value("createdAtSec", std::int64_t(0));
                    comment.UpdatedAtSec = commentJson.value("updatedAtSec", comment.CreatedAtSec);
                    comments.push_back(std::move(comment));
                }
                fixture.issueCommentsByIssueKey_[issueKey] = std::move(comments);
            }
        }
    }

    return fixture;
}

void JiraFakeTrackerFixture::Configure(FakeTrackerClient& client) const {
    // Every fixture client honours the process-wide network switch. The switch is only set when the
    // fixture names a mode: the app can create a fresh client mid-test (backend re-init), and that
    // must not silently undo a test's outage. Tests restore it with ScopedFakeNetworkReset.
    client.AttachNetwork(&GlobalFakeNetwork());
    if (hasNetworkMode_) {
        GlobalFakeNetwork().Set(networkMode_);
    }

    client.SetReachabilityResult(reachabilityKind_, reachabilityDiagnostic_);

    for (const auto& fetch : fetches_) {
        client.EnqueueFetchResult(fetch.Tickets, fetch.FullSyncCompleted, fetch.FetchError, fetch.Warning);
    }

    // Make the final scripted fetch the sticky static fallback. The production streaming-sync
    // path can fire more times than a fixture scripts (deferred initial auto-sync on first Draw
    // PLUS each test's explicit SyncWithBackend, all sharing one backend instance). Without a
    // fallback, the FakeTrackerClient returns an EMPTY result with FullSyncCompleted=true once
    // the queue drains — which the real sync interprets as "all issues deleted server-side" and
    // stale-prunes the previously-loaded tickets to nothing. Mirroring a real backend (idempotent
    // re-fetch returns the same steady-state issue set) by replaying the last scripted fetch keeps
    // re-syncs stable.
    if (!fetches_.empty()) {
        const JiraFixtureFetch& last = fetches_.back();
        client.SetFetchIssuesResult(last.Tickets, last.FullSyncCompleted, last.FetchError, last.Warning);
    }

    for (const auto& reply : updateIssueFieldsReplies_) {
        if (reply.Ok) {
            client.EnqueueUpdateIssueFieldsSuccess();
        } else {
            client.EnqueueUpdateIssueFieldsFailure(reply.Error);
        }
    }

    for (const auto& reply : createIssueReplies_) {
        if (reply.Ok) {
            client.EnqueueCreateIssueSuccess(reply.IssueKey);
        } else {
            client.EnqueueCreateIssueFailure(reply.Error);
        }
    }

    // Apply field catalog if present
    if (!fields_.empty()) {
        TrackerFieldCatalogResult catalogResult;
        catalogResult.Fields = fields_;
        client.SetFieldCatalogResult(catalogResult);
    }

    // Apply transitions if present
    for (const auto& entry : issueTransitionsByIssueId_) {
        client.SetIssueTransitions(entry.first, entry.second);
    }

    // Comments live on ITrackerCollaboration, so scripting them turns that role on. Transitions are
    // ITrackerFieldCatalog (always exposed) and need no switch.
    if (!issueCommentsByIssueKey_.empty()) {
        client.EnableCollaboration(true);
        for (const auto& entry : issueCommentsByIssueKey_) {
            client.SetIssueComments(entry.first, entry.second);
        }
    }
}

std::unique_ptr<FakeTrackerClient> JiraFakeTrackerFixture::CreateClient() const {
    auto client = std::make_unique<FakeTrackerClient>("Jira");
    Configure(*client);
    return client;
}

} // namespace smatchet_tests
