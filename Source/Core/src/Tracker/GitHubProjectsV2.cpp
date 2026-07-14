#include "GitHubProjectsV2.h"

#include "GitHubClientHelpers.h" // ExtractGitHubErrorMessage
#include "GitHubIssueSearch.h"   // BuildGitHubHeaders / ResolveGraphQlEndpoint / ExtractGraphQlErrors
#include "Json/BoundedJsonParse.h"
#include "Logger.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>

namespace smatchet {
namespace github {

namespace {

// One GraphQL POST + bounded parse + HTTP/errors[] classification. `tolerateErrors`
// lets the dual org/user project query pass when one half resolved (its other half
// contributes an errors[] entry by design); the caller then judges the data shape.
Result<nlohmann::json, TrackerError> RunProjectV2Query(const std::string& baseUrl, const std::string& pat,
                                                       const char* document, const nlohmann::json& variables,
                                                       bool tolerateErrors) {
    using QueryResult = Result<nlohmann::json, TrackerError>;
    nlohmann::json body = nlohmann::json::object();
    body["query"] = document;
    body["variables"] = variables;
    const std::string endpoint = ResolveGraphQlEndpoint(baseUrl);
    /* PILLAR2_WORKER_ONLY */ // est-latency: 15000ms
    const cpr::Response resp = TrackerPostLogged("GitHubClient", endpoint, BuildGitHubHeaders(pat), body.dump());
    if (resp.status_code != 200) {
        const std::string msg = ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
        LOG_ERROR("GitHubProjectsV2: GraphQL HTTP %ld — %s", resp.status_code, msg.c_str());
        return QueryResult::Err(ClassifyRejectedHttpStatus(resp.status_code, msg));
    }
    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return QueryResult::Err(TrackerErrorParse("GitHub projectV2 GraphQL response was not a JSON object."));
    }
    const std::string gqlErrors = ExtractGraphQlErrors(parsed);
    if (!gqlErrors.empty() && !tolerateErrors) {
        LOG_ERROR("GitHubProjectsV2: GraphQL errors — %s", gqlErrors.c_str());
        return QueryResult::Err(TrackerErrorInvalidRequest("GitHub projectV2 GraphQL error: " + gqlErrors));
    }
    return QueryResult::Ok(std::move(parsed));
}

} // namespace

Result<ProjectV2Catalog, TrackerError> FetchProjectV2Catalog(const std::string& baseUrl, const std::string& pat,
                                                             const std::string& owner, int projectNumber) {
    using CatalogResult = Result<ProjectV2Catalog, TrackerError>;
    nlohmann::json variables = nlohmann::json::object();
    variables["owner"] = owner;
    variables["number"] = projectNumber;
    // tolerateErrors: the dual org/user query yields an errors[] entry for the
    // owner kind that doesn't match this login even on success.
    auto response = RunProjectV2Query(baseUrl, pat, ProjectV2FieldsQueryDocument(), variables, /*tolerateErrors=*/true);
    if (!response) {
        return CatalogResult::Err(response.error());
    }
    auto parsed = ParseProjectV2Catalog(response.value());
    if (!parsed) {
        const std::string gqlErrors = ExtractGraphQlErrors(response.value());
        std::string detail = parsed.error();
        if (!gqlErrors.empty()) {
            detail += " (GraphQL: " + gqlErrors + ")";
        }
        LOG_WARN("GitHubProjectsV2: catalog fetch for %s/#%d failed — %s", owner.c_str(), projectNumber,
                 detail.c_str());
        return CatalogResult::Err(TrackerErrorInvalidRequest(detail));
    }
    LOG_INFO("GitHubProjectsV2: catalog for '%s' — %zu representable field(s).", parsed.value().ProjectTitle.c_str(),
             parsed.value().Fields.size());
    return CatalogResult::Ok(std::move(parsed.value()));
}

Result<std::map<std::string, std::vector<std::pair<std::string, std::string>>>, TrackerError>
FetchProjectV2ItemValues(const std::string& baseUrl, const std::string& pat, const std::string& projectNodeId,
                         const std::vector<TrackerField>& catalogFields, std::string* outWarning) {
    using ValuesResult = Result<std::map<std::string, std::vector<std::pair<std::string, std::string>>>, TrackerError>;
    constexpr int kPerPage = 100;
    constexpr int kMaxPages = 5; // ~500 items; boards past that get a truncation warning
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> out;
    std::string cursor;
    for (int page = 1; page <= kMaxPages; ++page) {
        nlohmann::json variables = nlohmann::json::object();
        variables["project"] = projectNodeId;
        variables["first"] = kPerPage;
        if (cursor.empty()) {
            variables["after"] = nullptr;
        } else {
            variables["after"] = cursor;
        }
        auto response =
            RunProjectV2Query(baseUrl, pat, ProjectV2ItemsQueryDocument(), variables, /*tolerateErrors=*/false);
        if (!response) {
            return ValuesResult::Err(response.error());
        }
        auto parsed = ParseProjectV2ItemsPage(response.value(), catalogFields);
        if (!parsed) {
            return ValuesResult::Err(TrackerErrorParse(parsed.error()));
        }
        for (auto& item : parsed.value().Items) {
            out[item.TicketKey] = std::move(item.Values);
        }
        if (!parsed.value().HasNext || parsed.value().EndCursor.empty()) {
            return ValuesResult::Ok(std::move(out));
        }
        cursor = parsed.value().EndCursor;
    }
    if (outWarning != nullptr) {
        *outWarning = "GitHub project has more items than the " + std::to_string(kMaxPages * kPerPage) +
                      "-item cap; project-field columns beyond the cap are blank this sync.";
    }
    LOG_WARN("GitHubProjectsV2: item-values pagination hit the %d-page cap.", kMaxPages);
    return ValuesResult::Ok(std::move(out));
}

Result<std::string, TrackerError> ResolveProjectV2ItemId(const std::string& baseUrl, const std::string& pat,
                                                         const std::string& owner, const std::string& repo,
                                                         long long issueNumber, const std::string& projectNodeId) {
    using ItemResult = Result<std::string, TrackerError>;
    nlohmann::json variables = nlohmann::json::object();
    variables["owner"] = owner;
    variables["repo"] = repo;
    variables["number"] = issueNumber;
    auto response =
        RunProjectV2Query(baseUrl, pat, ProjectV2ItemForIssueQueryDocument(), variables, /*tolerateErrors=*/false);
    if (!response) {
        return ItemResult::Err(response.error());
    }
    auto parsed = ParseProjectV2ItemIdForProject(response.value(), projectNodeId);
    if (!parsed) {
        return ItemResult::Err(TrackerErrorParse(parsed.error()));
    }
    return ItemResult::Ok(std::move(parsed.value()));
}

TrackerError UpdateProjectV2FieldValue(const std::string& baseUrl, const std::string& pat,
                                       const std::string& projectNodeId, const std::string& itemNodeId,
                                       const std::string& fieldNodeId, const nlohmann::json& valueOrNull) {
    nlohmann::json variables = nlohmann::json::object();
    variables["project"] = projectNodeId;
    variables["item"] = itemNodeId;
    variables["field"] = fieldNodeId;
    const bool clear = valueOrNull.is_null();
    if (!clear) {
        variables["value"] = valueOrNull;
    }
    auto response = RunProjectV2Query(
        baseUrl, pat, clear ? ProjectV2ClearFieldValueMutationDocument() : ProjectV2UpdateFieldValueMutationDocument(),
        variables, /*tolerateErrors=*/false);
    if (!response) {
        return response.error();
    }
    return TrackerError::Ok();
}

} // namespace github
} // namespace smatchet
