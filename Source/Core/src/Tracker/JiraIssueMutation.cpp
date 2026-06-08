#include "JiraClient.h"

#include "BackendAuditTrail.h"
#include "JiraIssueMappingPure.h"
#include "TrackerFieldValueParser.h"
#include "TrackerHttpUtils.h"
#include "Logger.h"
#include "NetworkUsageTracker.h"
#include "StringUtil.h"
#include "TrackerFieldPayload.h"
#include "IssueDraft.h"

#include "TrackerFieldSchema.h"

#include <cpr/cpr.h>
#include <ghc/filesystem.hpp>

#include <cctype>
#include <cstdint>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

// Returns the transition id leading to the requested status (status id → to.name →
// transition-name fallback, prioritised GLOBALLY). The matcher is the pure,
// unit-tested smatchet::jira::FindJiraTransitionId; the LOG_WARN for a name-fallback
// match lives here because the pure unit is Logger-free (#670).
std::string FindTransitionIdInArray(const nlohmann::json& transitionsArray, const std::string& targetStatusId,
                                    const std::string& targetStatusName) {
    const smatchet::jira::JiraTransitionMatch match =
        smatchet::jira::FindJiraTransitionId(transitionsArray, targetStatusId, targetStatusName);
    if (match.usedNameFallback) {
        LOG_WARN("JiraClient: transition name fallback triggered: transition '%s' matched target "
                 "status name '%s' but status name is '%s' — workflow may have diverged.",
                 match.transitionName.c_str(), targetStatusName.c_str(), match.toStatusName.c_str());
    }
    return match.id;
}

nlohmann::json AdfDocumentFromPlainText(const std::string& plainText) {
    nlohmann::json content = nlohmann::json::array();
    std::string line;
    std::istringstream iss(plainText);
    while (std::getline(iss, line)) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        nlohmann::json inner = nlohmann::json::array();
        nlohmann::json textNode = nlohmann::json::object();
        textNode["type"] = "text";
        textNode["text"] = line;
        inner.push_back(std::move(textNode));
        para["content"] = std::move(inner);
        content.push_back(std::move(para));
    }
    if (content.empty()) {
        nlohmann::json para = nlohmann::json::object();
        para["type"] = "paragraph";
        para["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", ""}}});
        content.push_back(std::move(para));
    }
    return nlohmann::json{{"type", "doc"}, {"version", 1}, {"content", std::move(content)}};
}

// Extract the target status id + name from the requested status value (object
// with id/name, or a bare string treated as a name).
void ParseTargetStatus(const nlohmann::json& statusValue, std::string& outId, std::string& outName) {
    if (statusValue.is_object()) {
        if (statusValue.contains("id")) {
            if (statusValue["id"].is_string()) {
                outId = statusValue["id"].get<std::string>();
            } else if (statusValue["id"].is_number_integer()) {
                outId = std::to_string(statusValue["id"].get<long long>());
            } else if (statusValue["id"].is_number_unsigned()) {
                outId = std::to_string(statusValue["id"].get<unsigned long long>());
            }
        }
        if (statusValue.contains("name") && statusValue["name"].is_string()) {
            outName = statusValue["name"].get<std::string>();
        }
    } else if (statusValue.is_string()) {
        outName = statusValue.get<std::string>();
    }
}

// Emit a failure audit-trail record for a transition attempt, tagged with the
// requested target status id + name.
void AppendTransitionFailure(const std::string& issueId, const std::string& auditOp, const std::string& outError,
                             const std::string& targetStatusId, const std::string& targetStatusName) {
    BackendAuditTrail::AppendResult(
        "issue_transition", "jira_client", issueId, auditOp, false, outError,
        nlohmann::json{{"target_status_id", targetStatusId}, {"target_status_name", targetStatusName}});
}

} // namespace

bool JiraClient::UpdateIssueFieldsViaTransition(const std::string& issueId, const nlohmann::json& statusValue,
                                                const std::string& base, const cpr::Header& headers,
                                                const std::string& auditOp, std::string& outError) {
    std::string targetStatusId;
    std::string targetStatusName;
    ParseTargetStatus(statusValue, targetStatusId, targetStatusName);

    if (targetStatusId.empty() && targetStatusName.empty()) {
        outError = "Missing target status for transition.";
        LOG_WARN("JiraClient: %s issue=%s", outError.c_str(), issueId.c_str());
        AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
        return false;
    }

    const std::string transitionsUrl = base + "/rest/api/3/issue/" + UrlEncode(issueId) + "/transitions";
    auto transitionsResp = TrackerGetLogged("JiraClient", transitionsUrl, headers);
    if (transitionsResp.status_code != 200) {
        outError = "Failed to fetch issue transitions: HTTP " + std::to_string(transitionsResp.status_code);
        if (!transitionsResp.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(transitionsResp.text, 1200);
        }
        LOG_ERROR("JiraClient: %s", outError.c_str());
        AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
        return false;
    }

    std::string transitionId;
    try {
        auto transitionsJson = nlohmann::json::parse(transitionsResp.text);
        if (!transitionsJson.contains("transitions") || !transitionsJson["transitions"].is_array()) {
            outError = "Invalid transitions response format.";
            LOG_ERROR("JiraClient: %s issue=%s body=%s", outError.c_str(), issueId.c_str(),
                      TruncateForLog(transitionsResp.text, 300).c_str());
            AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
            return false;
        }
        transitionId = FindTransitionIdInArray(transitionsJson["transitions"], targetStatusId, targetStatusName);
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse transitions response: ") + ex.what();
        LOG_ERROR("JiraClient: %s", outError.c_str());
        AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
        return false;
    }

    if (transitionId.empty()) {
        outError = "No valid transition found for requested status.";
        LOG_WARN("JiraClient: %s issue=%s targetStatusId=%s targetStatusName=%s", outError.c_str(), issueId.c_str(),
                 targetStatusId.c_str(), targetStatusName.c_str());
        AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
        return false;
    }

    nlohmann::json transitionBody = {{"transition", {{"id", transitionId}}}};
    const std::string transitionBodyStr = transitionBody.dump();
    auto transitionResp = TrackerPostLogged("JiraClient", transitionsUrl, headers, transitionBodyStr);
    if (transitionResp.status_code != 204 && transitionResp.status_code != 200) {
        outError = "Failed to transition issue status: HTTP " + std::to_string(transitionResp.status_code);
        if (!transitionResp.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(transitionResp.text, 1200);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueId.c_str());
        AppendTransitionFailure(issueId, auditOp, outError, targetStatusId, targetStatusName);
        return false;
    }

    LOG_INFO("JiraClient: transitioned Jira issue %s status successfully.", issueId.c_str());
    BackendAuditTrail::AppendResult("issue_transition", "jira_client", issueId, auditOp, true, std::string(),
                                    nlohmann::json{{"transition_id", transitionId},
                                                   {"target_status_id", targetStatusId},
                                                   {"target_status_name", targetStatusName}});
    return true;
}

bool JiraClient::UpdateIssueFieldsViaPut(const std::string& issueId, const nlohmann::json& fieldsAudited,
                                         const std::string& base, const cpr::Header& headers,
                                         const std::string& auditOp, std::string& outError) {
    nlohmann::json body = nlohmann::json::object();
    body["fields"] = fieldsAudited;
    const std::string updateUrl = base + "/rest/api/3/issue/" + UrlEncode(issueId);
    const std::string putBodyStr = body.dump();
    auto response = TrackerPutLogged("JiraClient", updateUrl, headers, putBodyStr);

    if (response.status_code != 204 && response.status_code != 200) {
        outError = "Failed to update issue fields: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(response.text, 1200);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueId.c_str());
        LOG_DEBUG("JiraClient: update payload:\n%s", body.dump(2).c_str());
        BackendAuditTrail::AppendResult(
            "issue_update_fields", "jira_client", issueId, auditOp, false, outError,
            nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fieldsAudited)}});
        return false;
    }

    LOG_INFO("JiraClient: updated Jira issue %s fields successfully.", issueId.c_str());
    BackendAuditTrail::AppendResult(
        "issue_update_fields", "jira_client", issueId, auditOp, true, std::string(),
        nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fieldsAudited)}});
    return true;
}

bool JiraClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("issue-update");
    const bool auditIsTransition = fields.is_object() && fields.size() == 1 && fields.contains("status");
    const std::string auditAction = auditIsTransition ? "issue_transition" : "issue_update_fields";

    // PUT path normalizes time-tracking strings to seconds; audit begin must match what we send.
    static const std::set<std::string> kTimeTrackingFieldIds = {
        "timeoriginalestimate",          "timeestimate",          "timespent",
        "aggregatetimeoriginalestimate", "aggregatetimeestimate", "aggregatetimespent"};
    nlohmann::json fieldsAudited = fields;
    if (fields.is_object() && !auditIsTransition) {
        for (const std::string& fieldId : kTimeTrackingFieldIds) {
            if (!fieldsAudited.contains(fieldId))
                continue;
            const auto& v = fieldsAudited[fieldId];
            if (v.is_string()) {
                std::string s = TrimCopy(v.get<std::string>());
                if (s.empty()) {
                    fieldsAudited[fieldId] = nullptr;
                } else {
                    long long sec = ParseWorkDurationToSeconds(s);
                    fieldsAudited[fieldId] = sec;
                }
            } else if (v.is_null()) {
                fieldsAudited[fieldId] = nullptr;
            }
        }
    }

    BackendAuditTrail::AppendBegin(
        auditAction, "jira_client", issueId, auditOp,
        nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fieldsAudited)}});

    TrackerConfig cfg = ConfigManager::Load();
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult(auditAction, "jira_client", issueId, auditOp, false, outError);
        return false;
    }
    if (issueId.empty()) {
        outError = "Issue id is empty.";
        LOG_WARN("JiraClient: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, "jira_client", issueId, auditOp, false, outError);
        return false;
    }
    if (!fields.is_object() || fields.empty()) {
        outError = "Update fields payload is empty.";
        LOG_WARN("JiraClient: %s", outError.c_str());
        BackendAuditTrail::AppendResult(auditAction, "jira_client", issueId, auditOp, false, outError);
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    // Jira requires status updates via transitions endpoint, not field PUT.
    if (auditIsTransition) {
        return UpdateIssueFieldsViaTransition(issueId, fields["status"], base, headers, auditOp, outError);
    }
    return UpdateIssueFieldsViaPut(issueId, fieldsAudited, base, headers, auditOp, outError);
}

bool JiraClient::AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                      const std::string& plainText, std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("comment");
    BackendAuditTrail::AppendBegin("issue_add_comment", "jira_client", issueKey, auditOp,
                                   nlohmann::json{{"comment_text", plainText}});
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError);
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError);
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    nlohmann::json body = nlohmann::json::object();
    body["body"] = AdfDocumentFromPlainText(plainText);
    const std::string postUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/comment";
    const std::string bodyStr = body.dump();
    auto response = TrackerPostLogged("JiraClient", postUrl, headers, bodyStr);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Add comment failed: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(response.text, 1200);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKey.c_str());
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError,
                                        nlohmann::json{{"comment_text", plainText}});
        return false;
    }
    BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, true, std::string(),
                                    nlohmann::json{{"comment_text", plainText}});
    return true;
}

bool JiraClient::AddWorklog(const TrackerConfig& cfg, const std::string& issueKey, const std::string& timeSpent,
                            const std::string& timeRemaining, const std::string& adjustEstimate,
                            const std::string& workDescription, const std::string& startedDate, std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("worklog");
    nlohmann::json auditData = nlohmann::json{{"time_spent", timeSpent},
                                              {"time_remaining", timeRemaining},
                                              {"adjust_estimate", adjustEstimate},
                                              {"work_description", workDescription},
                                              {"started", startedDate}};
    BackendAuditTrail::AppendBegin("issue_add_worklog", "jira_client", issueKey, auditOp, auditData);

    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_add_worklog", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        BackendAuditTrail::AppendResult("issue_add_worklog", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    nlohmann::json body = nlohmann::json::object();
    body["timeSpent"] = timeSpent;
    body["started"] = startedDate;
    if (!workDescription.empty()) {
        body["comment"] = AdfDocumentFromPlainText(workDescription);
    }

    std::string postUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/worklog";
    if (!adjustEstimate.empty()) {
        postUrl += "?adjustEstimate=" + UrlEncode(adjustEstimate);
        if (adjustEstimate == "new" && !timeRemaining.empty()) {
            postUrl += "&newEstimate=" + UrlEncode(timeRemaining);
        } else if (adjustEstimate == "manual" && !timeRemaining.empty()) {
            postUrl += "&reduceBy=" + UrlEncode(timeRemaining);
        }
    }

    const std::string bodyStr = body.dump();
    auto response = TrackerPostLogged("JiraClient", postUrl, headers, bodyStr);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Add worklog failed: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — " + TruncateForLog(response.text, 1200);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKey.c_str());
        BackendAuditTrail::AppendResult("issue_add_worklog", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }

    BackendAuditTrail::AppendResult("issue_add_worklog", "jira_client", issueKey, auditOp, true, std::string(),
                                    auditData);
    return true;
}

bool JiraClient::AddIssueCommentAnnotateContext(const TrackerConfig& cfg, const std::string& issueKey,
                                                const std::string& p4User, const std::string& functionName,
                                                const std::string& filePath, const int lineNumber,
                                                const std::string& changelist, const std::string& date,
                                                const bool approximated, const std::string& codeSnippet,
                                                std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("comment");
    const nlohmann::json auditData = nlohmann::json{{"comment_kind", "annotate_context"},
                                                    {"p4_user", p4User},
                                                    {"function", functionName},
                                                    {"file", filePath},
                                                    {"line", lineNumber},
                                                    {"changelist", changelist}};
    BackendAuditTrail::AppendBegin("issue_add_comment", "jira_client", issueKey, auditOp, auditData);
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);

    auto para = [](const std::string& text) {
        nlohmann::json p = nlohmann::json::object();
        p["type"] = "paragraph";
        p["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", text}}});
        return p;
    };

    nlohmann::json content = nlohmann::json::array();
    std::string head = "Annotate — " + p4User + " | " + functionName + " | " + filePath + ":" +
                       std::to_string(lineNumber) + " | CL " + changelist + " | " + date;
    content.push_back(para(head));
    if (approximated) {
        content.push_back(para("Note: annotate is approximated (exact line not found in annotate)."));
    }
    std::string blockBody =
        "L" + std::to_string(lineNumber) + "  CL:" + changelist + "  " + p4User + "\n" + codeSnippet;
    nlohmann::json codeBlock = nlohmann::json::object();
    codeBlock["type"] = "codeBlock";
    codeBlock["attrs"] = nlohmann::json::object();
    codeBlock["attrs"]["language"] = "cpp";
    codeBlock["content"] = nlohmann::json::array({nlohmann::json{{"type", "text"}, {"text", blockBody}}});
    content.push_back(std::move(codeBlock));

    nlohmann::json doc = nlohmann::json::object();
    doc["type"] = "doc";
    doc["version"] = 1;
    doc["content"] = std::move(content);

    nlohmann::json body = nlohmann::json::object();
    body["body"] = std::move(doc);
    const std::string postUrl = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/comment";
    const std::string bodyStr = body.dump();
    auto response = TrackerPostLogged("JiraClient", postUrl, headers, bodyStr);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Add Annotate comment failed: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(response.text, 1200);
        }
        LOG_ERROR("JiraClient: %s issue=%s", outError.c_str(), issueKey.c_str());
        BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, false, outError,
                                        auditData);
        return false;
    }
    BackendAuditTrail::AppendResult("issue_add_comment", "jira_client", issueKey, auditOp, true, std::string(),
                                    auditData);
    return true;
}

std::string JiraClient::CreateIssue(const nlohmann::json& fields, std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("issue-create");
    BackendAuditTrail::AppendBegin("issue_create", "jira_client", std::string(), auditOp,
                                   nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});

    TrackerConfig cfg = ConfigManager::Load();
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_create", "jira_client", std::string(), auditOp, false, outError);
        return {};
    }
    if (!fields.is_object() || fields.empty()) {
        outError = "Create issue payload is empty.";
        LOG_WARN("JiraClient: %s", outError.c_str());
        BackendAuditTrail::AppendResult("issue_create", "jira_client", std::string(), auditOp, false, outError);
        return {};
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);
    nlohmann::json body = nlohmann::json::object();
    body["fields"] = fields;
    const std::string url = base + "/rest/api/3/issue";
    const std::string bodyStr = body.dump();
    auto response = TrackerPostLogged("JiraClient", url, headers, bodyStr);

    if (response.status_code != 201 && response.status_code != 200) {
        outError = "Create issue failed: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " — ";
            outError += TruncateForLog(response.text, 1200);
        }
        LOG_ERROR("JiraClient: %s", outError.c_str());
        LOG_DEBUG("JiraClient: create payload:\n%s", body.dump(2).c_str());
        BackendAuditTrail::AppendResult(
            "issue_create", "jira_client", std::string(), auditOp, false, outError,
            nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});
        return {};
    }

    try {
        auto j = nlohmann::json::parse(response.text);
        const std::string key = j.value("key", std::string());
        if (key.empty()) {
            outError = "Create issue response missing 'key'.";
            LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), TruncateForLog(response.text, 300).c_str());
            BackendAuditTrail::AppendResult(
                "issue_create", "jira_client", std::string(), auditOp, false, outError,
                nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});
            return {};
        }
        LOG_INFO("JiraClient: created Jira issue %s.", key.c_str());
        BackendAuditTrail::AppendResult(
            "issue_create", "jira_client", key, auditOp, true, std::string(),
            nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});
        return key;
    } catch (const std::exception& ex) {
        outError = std::string("Failed to parse create response: ") + ex.what();
        LOG_ERROR("JiraClient: %s body=%s", outError.c_str(), TruncateForLog(response.text, 300).c_str());
        BackendAuditTrail::AppendResult(
            "issue_create", "jira_client", std::string(), auditOp, false, outError,
            nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});
        return {};
    }
}

bool JiraClient::AttachFilesToIssue(const std::string& issueKey, const std::vector<std::string>& absolutePaths,
                                    std::vector<std::pair<std::string, std::string>>& outFailures,
                                    std::string& outError) {
    outFailures.clear();
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("attach");
    nlohmann::json fileNames = nlohmann::json::array();
    for (const auto& path : absolutePaths) {
        if (!path.empty()) {
            fileNames.push_back(ghc::filesystem::path(path).filename().string());
        }
    }
    BackendAuditTrail::AppendBegin("issue_attach_files", "jira_client", issueKey, auditOp,
                                   nlohmann::json{{"files", fileNames}, {"file_count", fileNames.size()}});

    TrackerConfig cfg = ConfigManager::Load();
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_attach_files", "jira_client", issueKey, auditOp, false, outError);
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        BackendAuditTrail::AppendResult("issue_attach_files", "jira_client", issueKey, auditOp, false, outError);
        return false;
    }
    if (absolutePaths.empty()) {
        BackendAuditTrail::AppendResult("issue_attach_files", "jira_client", issueKey, auditOp, true, std::string(),
                                        nlohmann::json{{"file_count", 0}});
        return true;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    // Multipart: let cpr set Content-Type; always include X-Atlassian-Token.
    cpr::Header headers = BuildTrackerHeaders(cfg, false);
    headers["X-Atlassian-Token"] = "no-check";
    const std::string url = base + "/rest/api/3/issue/" + UrlEncode(issueKey) + "/attachments";

    bool allOk = true;
    for (const auto& path : absolutePaths) {
        if (path.empty()) {
            continue;
        }
        std::error_code ec;
        if (!ghc::filesystem::exists(path, ec) || ec) {
            outFailures.emplace_back(path, "File not found.");
            allOk = false;
            BackendAuditTrail::AppendResult(
                "issue_attach_file", "jira_client", issueKey, auditOp, false, "File not found.",
                nlohmann::json{{"filename", ghc::filesystem::path(path).filename().string()}});
            continue;
        }

        cpr::Multipart multipart{{"file", cpr::File{path}}};
        cpr::Redirect redirect(true, true);
        cpr::Response response =
            cpr::Post(cpr::Url{url}, headers, multipart, redirect, cpr::ConnectTimeout{kTrackerConnectTimeoutMs},
                      cpr::Timeout{kTrackerOverallTimeoutMs});
        std::uint64_t approxBytes = 0;
        try {
            approxBytes = static_cast<std::uint64_t>(ghc::filesystem::file_size(path, ec));
            if (ec)
                approxBytes = 0;
        } catch (...) { // catch-all-ok: best-effort byte estimate for usage telemetry; 0 on any file_size failure
            approxBytes = 0;
        }
        NetworkUsageTracker::Instance().Record(HttpTrafficKind::Tracker, approxBytes, response);
        LogTrackerHttpResult("JiraClient", "POST", url, response);

        if (response.status_code != 200 && response.status_code != 201) {
            std::string msg = "HTTP " + std::to_string(response.status_code);
            if (!response.text.empty()) {
                msg += " — ";
                msg += TruncateForLog(response.text, 600);
            }
            outFailures.emplace_back(path, std::move(msg));
            allOk = false;
            LOG_WARN("JiraClient: attachment upload failed issue=%s path=%s HTTP=%d", issueKey.c_str(), path.c_str(),
                     static_cast<int>(response.status_code));
            BackendAuditTrail::AppendResult(
                "issue_attach_file", "jira_client", issueKey, auditOp, false, outFailures.back().second,
                nlohmann::json{{"filename", ghc::filesystem::path(path).filename().string()}});
            continue;
        }
        LOG_INFO("JiraClient: uploaded attachment issue=%s path=%s", issueKey.c_str(), path.c_str());
        BackendAuditTrail::AppendResult(
            "issue_attach_file", "jira_client", issueKey, auditOp, true, std::string(),
            nlohmann::json{{"filename", ghc::filesystem::path(path).filename().string()}, {"bytes", approxBytes}});
    }

    if (!allOk && outError.empty()) {
        outError = "One or more attachments failed to upload.";
    }
    BackendAuditTrail::AppendResult(
        "issue_attach_files", "jira_client", issueKey, auditOp, allOk, allOk ? std::string() : outError,
        nlohmann::json{{"file_count", fileNames.size()}, {"failure_count", outFailures.size()}});
    return allOk;
}

bool JiraClient::AddIssueToSprint(const std::string& issueKey, const std::string& sprintId, std::string& outError) {
    const TrackerConfig cfg = ConfigManager::Load();
    return AddIssueToSprint(cfg, issueKey, sprintId, outError);
}

bool JiraClient::AddIssueToSprint(const TrackerConfig& cfg, const std::string& issueKey, const std::string& sprintId,
                                  std::string& outError) {
    outError.clear();
    const std::string auditOp = BackendAuditTrail::MakeOperationId("sprint");
    BackendAuditTrail::AppendBegin("issue_add_to_sprint", "jira_client", issueKey, auditOp,
                                   nlohmann::json{{"sprint_id", sprintId}});
    if (!EnsureTrackerAuthConfig(cfg, outError)) {
        BackendAuditTrail::AppendResult("issue_add_to_sprint", "jira_client", issueKey, auditOp, false, outError,
                                        nlohmann::json{{"sprint_id", sprintId}});
        return false;
    }
    if (issueKey.empty()) {
        outError = "Issue key is empty.";
        BackendAuditTrail::AppendResult("issue_add_to_sprint", "jira_client", issueKey, auditOp, false, outError,
                                        nlohmann::json{{"sprint_id", sprintId}});
        return false;
    }
    if (sprintId.empty()) {
        outError = "Sprint id is empty.";
        BackendAuditTrail::AppendResult("issue_add_to_sprint", "jira_client", issueKey, auditOp, false, outError,
                                        nlohmann::json{{"sprint_id", sprintId}});
        return false;
    }

    const std::string base = NormalizeBaseUrl(cfg.Domain);
    const cpr::Header headers = BuildTrackerHeaders(cfg, true);
    const std::string url = base + "/rest/agile/1.0/sprint/" + UrlEncode(sprintId) + "/issue";
    // Jira Agile API accepts either issue key or issue numeric ID in the `issues` array.
    // We pass the key (e.g. "PROJ-123"); if the board's Jira version requires a numeric ID,
    // resolve first via GET /rest/api/3/issue/{key}?fields=id and swap before this call.
    const nlohmann::json body = nlohmann::json{{"issues", nlohmann::json::array({issueKey})}};
    const std::string bodyStr = body.dump();
    auto response = TrackerPostLogged("JiraClient", url, headers, bodyStr);
    if (response.status_code != 204 && response.status_code != 200) {
        outError = "Failed to add issue to sprint: HTTP " + std::to_string(response.status_code);
        if (!response.text.empty()) {
            outError += " - " + TruncateForLog(response.text, 220);
        }
        LOG_ERROR("JiraClient: %s issue=%s sprint=%s", outError.c_str(), issueKey.c_str(), sprintId.c_str());
        BackendAuditTrail::AppendResult("issue_add_to_sprint", "jira_client", issueKey, auditOp, false, outError,
                                        nlohmann::json{{"sprint_id", sprintId}});
        return false;
    }
    BackendAuditTrail::AppendResult("issue_add_to_sprint", "jira_client", issueKey, auditOp, true, std::string(),
                                    nlohmann::json{{"sprint_id", sprintId}});
    return true;
}

bool JiraClient::UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values, std::string& outError) {
    auto payloadResult = BuildFieldPayload(field, values);
    if (!payloadResult) {
        outError = payloadResult.error().Detail;
        return false;
    }
    return UpdateIssueFields(issueId, payloadResult.value(), outError);
}

Result<nlohmann::json, TrackerError> JiraClient::BuildFieldPayload(const TrackerField& field,
                                                                   const std::vector<std::string>& values) {
    nlohmann::json builtValue;
    std::string buildErr;
    if (!TrackerFieldPayload::BuildValue(field, values, builtValue, buildErr)) {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(buildErr));
    }
    return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json::object({{field.Id, std::move(builtValue)}}));
}

Result<nlohmann::json, TrackerError> JiraClient::BuildCreatePayload(const IssueDraft& draft,
                                                                    const std::vector<TrackerField>& catalog) {
    nlohmann::json outPayload = nlohmann::json::object();

    if (draft.ProjectKey.empty()) {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest("Project key is empty."));
    }
    outPayload["project"] = nlohmann::json{{"key", draft.ProjectKey}};

    if (!draft.IssueTypeId.empty()) {
        outPayload["issuetype"] = nlohmann::json{{"id", draft.IssueTypeId}};
    } else if (!draft.IssueTypeName.empty()) {
        outPayload["issuetype"] = nlohmann::json{{"name", draft.IssueTypeName}};
    } else {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest("Issue type is empty."));
    }

    if (!draft.ParentKey.empty()) {
        outPayload["parent"] = nlohmann::json{{"key", draft.ParentKey}};
    }

    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_')
            continue;
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent" || fieldId == "status") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty())
            continue;

        const TrackerField* field = nullptr;
        auto fIt = std::find_if(catalog.begin(), catalog.end(), [&](const auto& f) { return f.Id == fieldId; });
        if (fIt != catalog.end()) {
            field = &(*fIt);
        }

        TrackerField synthetic;
        if (!field) {
            synthetic.Id = fieldId;
            synthetic.Type = "string";
            synthetic.Family = TrackerFieldFamily::Text;
            field = &synthetic;
        }

        const TrackerField TrackerField = *field;
        if (TrackerFieldPayload::IsSprintField(TrackerField))
            continue;

        nlohmann::json value;
        std::string err;
        if (!TrackerFieldPayload::BuildValue(TrackerField, {raw}, value, err)) {
            return Result<nlohmann::json, TrackerError>::Err(
                TrackerErrorInvalidRequest("Field '" + fieldId + "': " + err));
        }
        if (!value.is_null()) {
            outPayload[fieldId] = std::move(value);
        }
    }

    // Map timetracking estimates if present in FieldValues
    std::string originalEstimate;
    std::string remainingEstimate;
    auto oIt = draft.FieldValues.find("timeoriginalestimate");
    if (oIt != draft.FieldValues.end() && !oIt->second.empty()) {
        originalEstimate = oIt->second;
    }
    auto rIt = draft.FieldValues.find("timeestimate");
    if (rIt != draft.FieldValues.end() && !rIt->second.empty()) {
        remainingEstimate = rIt->second;
    }
    if (!originalEstimate.empty() || !remainingEstimate.empty()) {
        nlohmann::json timetrackingPayload = nlohmann::json::object();
        if (!originalEstimate.empty()) {
            timetrackingPayload["originalEstimate"] = originalEstimate;
        }
        if (!remainingEstimate.empty()) {
            timetrackingPayload["remainingEstimate"] = remainingEstimate;
        }
        outPayload["timetracking"] = std::move(timetrackingPayload);
    }

    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

Result<nlohmann::json, TrackerError> JiraClient::BuildUpdatePayload(const IssueDraft& draft,
                                                                    const std::vector<TrackerField>& catalog) {
    nlohmann::json outPayload = nlohmann::json::object();

    if (!draft.ParentKey.empty()) {
        outPayload["parent"] = nlohmann::json{{"key", draft.ParentKey}};
    }

    for (const auto& kv : draft.FieldValues) {
        const std::string& fieldId = kv.first;
        if (fieldId.size() >= 2 && fieldId[0] == '_' && fieldId[1] == '_')
            continue;
        if (fieldId.empty() || IssueDraftHelpers::IsCreateSuppressedFieldId(fieldId) ||
            IssueDraftHelpers::IsSpecialDraftFieldId(fieldId) || fieldId == "project" || fieldId == "issuetype" ||
            fieldId == "parent" || fieldId == "status") {
            continue;
        }
        const std::string raw = kv.second;
        if (raw.empty())
            continue;

        const TrackerField* field = nullptr;
        auto fIt = std::find_if(catalog.begin(), catalog.end(), [&](const auto& f) { return f.Id == fieldId; });
        if (fIt != catalog.end()) {
            field = &(*fIt);
        }

        TrackerField synthetic;
        if (!field) {
            synthetic.Id = fieldId;
            synthetic.Type = "string";
            synthetic.Family = TrackerFieldFamily::Text;
            field = &synthetic;
        }

        const TrackerField TrackerField = *field;
        if (TrackerFieldPayload::IsSprintField(TrackerField))
            continue;

        nlohmann::json value;
        std::string err;
        if (!TrackerFieldPayload::BuildValue(TrackerField, {raw}, value, err)) {
            return Result<nlohmann::json, TrackerError>::Err(
                TrackerErrorInvalidRequest("Field '" + fieldId + "': " + err));
        }
        if (!value.is_null()) {
            outPayload[fieldId] = std::move(value);
        }
    }

    // Map timetracking estimates if present in FieldValues
    std::string originalEstimate;
    std::string remainingEstimate;
    auto oIt = draft.FieldValues.find("timeoriginalestimate");
    if (oIt != draft.FieldValues.end() && !oIt->second.empty()) {
        originalEstimate = oIt->second;
    }
    auto rIt = draft.FieldValues.find("timeestimate");
    if (rIt != draft.FieldValues.end() && !rIt->second.empty()) {
        remainingEstimate = rIt->second;
    }
    if (!originalEstimate.empty() || !remainingEstimate.empty()) {
        nlohmann::json timetrackingPayload = nlohmann::json::object();
        if (!originalEstimate.empty()) {
            timetrackingPayload["originalEstimate"] = originalEstimate;
        }
        if (!remainingEstimate.empty()) {
            timetrackingPayload["remainingEstimate"] = remainingEstimate;
        }
        outPayload["timetracking"] = std::move(timetrackingPayload);
    }

    return Result<nlohmann::json, TrackerError>::Ok(std::move(outPayload));
}

std::string JiraClient::ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const {
    (void)fieldId;
    if (!field) {
        return value;
    }
    return TrackerFieldPayload::ResolveDisplayValueForSubmittedSelection(*field, value);
}
