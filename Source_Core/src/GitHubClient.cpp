#include "GitHubClient.h"

#include "GitHubClientHelpers.h"
#include "LabelEditDiffPure.h"
#include "Logger.h"
#include "TrackerFieldSchema.h"

namespace {

// Static field catalog — the 6 native GitHub-issue fields. Static because GitHub
// issues don't have user-configurable schemas like Jira customfields. Projects
// V2 custom fields are a separate API (deferred per plan § Out of scope).
const char* const kStateLabel = "State";
const char* const kLabelsLabel = "Labels";
const char* const kAssigneesLabel = "Assignees";
const char* const kMilestoneLabel = "Milestone";
const char* const kTitleLabel = "Title";
const char* const kBodyLabel = "Body";

const char* const kPatMissingError = "GitHub PAT not configured (set Preferences > Tracker > GitHub PAT)";

void StubError(std::string& out, const char* method) {
    out = std::string(method) + ": GitHubClient HTTP impl deferred to a follow-up slice of "
                                "docs/design/github-tracker-backend.md PR2";
}

} // namespace

GitHubClient::GitHubClient(const std::string& baseUrl, const std::string& pat)
    : baseUrl_(baseUrl.empty() ? std::string("https://api.github.com") : baseUrl), pat_(pat) {
    LOG_INFO("GitHubClient: ctor baseUrl='%s' pat_bytes=%zu", baseUrl_.c_str(), pat_.size());
}

std::string GitHubClient::GetTrackerType() const { return "github"; }

TrackerReachabilityProbeResult GitHubClient::ProbeReachability(const TrackerConfig& /*cfg*/) {
    // Deferred: real impl is a GET /rate_limit with the PAT. For now report
    // "config OK" only when the PAT is present so the connectivity banner
    // distinguishes "configure PAT" from "GitHub unreachable".
    TrackerReachabilityProbeResult out;
    if (pat_.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = kPatMissingError;
    } else {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = "GitHubClient probe: PAT present (HTTP probe deferred)";
    }
    return out;
}

std::vector<CachedTicket> GitHubClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* /*configOverride*/,
                                                   const ViewsStore* /*viewsOverride*/, std::string* outFetchError,
                                                   std::string* /*outWarning*/) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }
    if (outFetchError) {
        StubError(*outFetchError, "FetchIssues");
    }
    return {};
}

bool GitHubClient::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& /*issueKeys*/,
                                      const ViewsStore& /*views*/, std::vector<CachedTicket>& /*outTickets*/,
                                      std::string& outError) {
    StubError(outError, "FetchIssuesForKeys");
    return false;
}

bool GitHubClient::FetchFieldCatalog(const TrackerConfig& /*cfg*/, const std::string& /*projectKey*/,
                                     TrackerFieldCatalogResult& outCatalog, std::string& outError) {
    // 6 native fields. Static — no per-project enumeration like Jira's
    // create-meta or Plane's custom fields.
    outError.clear();
    outCatalog = TrackerFieldCatalogResult{};
    auto addField = [&outCatalog](const char* id, const char* label, const char* type) {
        TrackerField f;
        f.Id = id;
        f.Name = label;
        f.Type = type;
        outCatalog.Fields.push_back(f);
    };
    addField("state", kStateLabel, "string");
    addField("labels", kLabelsLabel, "array");
    addField("assignees", kAssigneesLabel, "array");
    addField("milestone", kMilestoneLabel, "string");
    addField("title", kTitleLabel, "string");
    addField("body", kBodyLabel, "string");
    return true;
}

std::string GitHubClient::BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& issueKey) const {
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueKey, parsed)) {
        return "";
    }
    // https://github.com/<owner>/<repo>/issues/<n>. Enterprise users with a
    // non-api.github.com base URL get the same shape — replace `/api/v3`
    // suffix with empty.
    std::string host = baseUrl_;
    const std::string apiSuffix = "/api/v3";
    if (host.size() > apiSuffix.size() && host.compare(host.size() - apiSuffix.size(), apiSuffix.size(), apiSuffix) == 0) {
        host = host.substr(0, host.size() - apiSuffix.size());
    } else if (host == "https://api.github.com") {
        host = "https://github.com";
    }
    return host + "/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
}

bool GitHubClient::UpdateIssueFields(const std::string& /*issueId*/, const nlohmann::json& /*fields*/,
                                     std::string& outError) {
    StubError(outError, "UpdateIssueFields");
    return false;
}

bool GitHubClient::UpdateField(const std::string& issueId, const TrackerField& field,
                               const std::vector<std::string>& values, std::string& outError) {
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueId, parsed)) {
        outError = "GitHubClient::UpdateField: invalid issueId shape (expected owner/repo#N): " + issueId;
        return false;
    }
    if (pat_.empty()) {
        outError = kPatMissingError;
        return false;
    }
    // Label-field set-replace: real impl pre-fetches current labels, computes
    // diff via LabelEditDiffPure, then issues POST/DELETE per element. Stub
    // logs the diff intent so the audit trail still shows the attempted edit.
    if (field.Id == "labels") {
        std::vector<std::string> current;
        const smatchet::github::LabelEditDiff diff = smatchet::github::ComputeLabelEditDiff(current, values);
        LOG_INFO("GitHubClient::UpdateField labels stub: %s toAdd=%zu toRemove=%zu", issueId.c_str(), diff.ToAdd.size(),
                 diff.ToRemove.size());
    }
    StubError(outError, "UpdateField");
    return false;
}

bool GitHubClient::BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                                     nlohmann::json& outPayload, std::string& outError) {
    outError.clear();
    if (field.Id == "labels" || field.Id == "assignees") {
        outPayload = values;  // array of strings
        return true;
    }
    if (field.Id == "state" || field.Id == "milestone" || field.Id == "title" || field.Id == "body") {
        outPayload = values.empty() ? std::string() : values.front();
        return true;
    }
    outError = std::string("GitHubClient::BuildFieldPayload: unknown field '") + field.Id + "'";
    return false;
}

bool GitHubClient::BuildCreatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                      nlohmann::json& /*outPayload*/, std::string& outError) {
    StubError(outError, "BuildCreatePayload");
    return false;
}

std::string GitHubClient::ResolveDisplayValue(const std::string& fieldId, const TrackerField* /*field*/,
                                              const std::string& value) const {
    // GitHub's enum-like fields (state, milestone) carry display-ready strings
    // server-side, so identity is correct for the 6-field catalog.
    (void)fieldId;
    return value;
}

std::string GitHubClient::CreateIssue(const nlohmann::json& /*fields*/, std::string& outError) {
    StubError(outError, "CreateIssue");
    return "";
}

std::string GitHubClient::ExtractProjectFromQuery(const std::string& query) const {
    // GitHub backend's "project" anchor is `owner/repo` — extracted from the
    // query string if formatted as such (e.g. user typed `owner/repo` in the
    // tracker query field). No multi-repo cross product yet.
    const std::size_t slash = query.find('/');
    if (slash != std::string::npos && slash > 0 && slash < query.size() - 1) {
        return query.substr(0, query.find(' '));
    }
    return "";
}

std::vector<RemoteProject> GitHubClient::ListProjects() {
    // Deferred — `GET /user/repos` paginated. Empty for now.
    return {};
}
