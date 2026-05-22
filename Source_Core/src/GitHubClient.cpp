#include "GitHubClient.h"

#include "GitHubClientHelpers.h"
#include "GitHubIssueSearch.h"
#include "LabelEditDiffPure.h"
#include "Logger.h"
#include "TrackerFieldSchema.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>

#include <sstream>

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

// GitHub REST API auth + content-negotiation headers. Bearer-PAT auth (fine-grained
// or classic both work), JSON-only response, pinned API version (2022-11-28) so the
// server can't silently flip our response shape on us. Defined here, declared in
// GitHubIssueSearch.h so GitHubIssueSearch.cpp shares the helper.
namespace smatchet {
namespace github {

cpr::Header BuildGitHubHeaders(const std::string& pat) {
    return cpr::Header{
        {"Authorization", std::string("Bearer ") + pat},
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet-GitHubClient"},
    };
}

} // namespace github
} // namespace smatchet

using smatchet::github::BuildGitHubHeaders;

GitHubClient::GitHubClient(const std::string& baseUrl, const std::string& pat)
    : baseUrl_(baseUrl.empty() ? std::string("https://api.github.com") : baseUrl), pat_(pat) {
    LOG_INFO("GitHubClient: ctor baseUrl='%s' pat_bytes=%zu", baseUrl_.c_str(), pat_.size());
}

std::string GitHubClient::GetTrackerType() const { return "github"; }

TrackerReachabilityProbeResult GitHubClient::ProbeReachability(const TrackerConfig& /*cfg*/) {
    // GET /rate_limit — cheap, always available even on free PATs, returns auth
    // status + remaining quota. Maps HTTP codes to TrackerReachabilityProbeKind
    // the same shape JiraClient::ProbeReachability uses so the connectivity
    // banner doesn't care which backend is active.
    TrackerReachabilityProbeResult out;
    if (pat_.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = kPatMissingError;
        return out;
    }

    const std::string url = baseUrl_ + "/rate_limit";
    const cpr::Header headers = BuildGitHubHeaders(pat_);
    const cpr::Response resp =
        TrackerGetLogged("GitHubClient", url, headers, kTrackerProbeConnectTimeoutMs, kTrackerProbeOverallTimeoutMs);

    const long sc = resp.status_code;
    if (sc == 200) {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        // Extract core rate-limit numbers if the body parses; non-fatal on parse failure.
        std::ostringstream oss;
        oss << "HTTP 200";
        try {
            const nlohmann::json j = nlohmann::json::parse(resp.text);
            if (j.contains("resources") && j["resources"].contains("core")) {
                const auto& core = j["resources"]["core"];
                const int limit = core.value("limit", 0);
                const int remaining = core.value("remaining", 0);
                oss << " (core " << remaining << "/" << limit << ")";
            }
        } catch (const std::exception&) {
            // Body wasn't JSON or didn't carry the expected shape — banner still passes.
        }
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc == 401 || sc == 403) {
        // 401 = bad PAT. 403 = scope-missing OR secondary rate-limit. Both are
        // "you reached us but auth is off"; the connectivity banner shape is
        // the same.
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        if (sc == 401) {
            oss << " (invalid or expired PAT)";
        } else {
            oss << " (PAT missing scope, or secondary rate-limit)";
        }
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc >= 500 && sc < 600) {
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        std::ostringstream oss;
        oss << "HTTP " << sc;
        out.Diagnostic = oss.str();
        return out;
    }

    if (sc > 0 && sc < 500) {
        // 404 / 4xx that aren't auth — typically a malformed Base URL.
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        std::ostringstream oss;
        oss << "HTTP " << sc << " (check Base URL — expected https://api.github.com or "
            << "https://<enterprise>/api/v3)";
        out.Diagnostic = oss.str();
        return out;
    }

    out.Kind = TrackerReachabilityProbeKind::TransportDown;
    out.Diagnostic = resp.error.message.empty() ? std::string("Unknown network error") : resp.error.message;
    return out;
}

std::vector<CachedTicket> GitHubClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                                   const ViewsStore* /*viewsOverride*/, std::string* outFetchError,
                                                   std::string* outWarning) {
    // PR4 — delegate to the standalone fetcher (mirrors JiraIssueSearch /
    // PlaneIssueSearch convention; the per-class FetchIssues is just a thin
    // shim that resolves config + forwards). Called from
    // TicketSyncService::SyncWithBackend on a worker thread (never the UI
    // thread, per Pillar 2).
    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();
    return smatchet::github::FetchIssuesViaRestApi(baseUrl_, pat_, cfg.GitHubOwner, cfg.GitHubRepo, cfg.JqlQuery,
                                                    outFullSyncCompleted, outFetchError, outWarning);
}

bool GitHubClient::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& issueKeys,
                                      const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                      std::string& outError) {
    // PR4 — single-issue GET loop per key. baseUrl_/pat_ snapshots captured
    // at ctor time; the cfg/views overrides aren't consulted here because the
    // canonical owner/repo is already embedded in each key (`owner/repo#N`).
    if (pat_.empty()) {
        outError = kPatMissingError;
        return false;
    }
    return smatchet::github::FetchIssuesForKeysViaRestApi(baseUrl_, pat_, issueKeys, outTickets, outError);
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
