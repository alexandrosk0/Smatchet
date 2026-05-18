#include "GitHubClient.h"

#include "AiErrorRedact.h"
#include "GitHubClientHelpers.h"
#include "Logger.h"

#include <cpr/cpr.h>

#include <sstream>
#include <utility>

namespace {

constexpr int kGitHubConnectTimeoutMs = 5000;
constexpr int kGitHubOverallTimeoutMs = 15000;

std::string StripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

// PAT must never reach user-visible surfaces verbatim. The redactor is intentionally
// shared with the AI provider error path (defense-in-depth — provider 4xx bodies have
// been observed echoing the Authorization header).
std::string RedactForLog(const std::string& body) {
    return smatchet::ai::pure::RedactProviderErrorBody(body);
}

} // namespace

GitHubClient::GitHubClient(std::string baseUrl, std::string personalAccessToken)
    : baseUrl_(StripTrailingSlash(baseUrl.empty() ? std::string("https://api.github.com") : std::move(baseUrl))),
      pat_(std::move(personalAccessToken)) {}

GitHubClient::~GitHubClient() = default;

std::string GitHubClient::GetTrackerType() const {
    return "GitHub";
}

TrackerReachabilityProbeResult GitHubClient::ProbeReachability(const TrackerConfig& /*cfg*/) {
    TrackerReachabilityProbeResult out;
    out.Kind = TrackerReachabilityProbeKind::TransportDown;
    out.Diagnostic = "GitHub ProbeReachability is not supported yet (lands in a later slice).";
    return out;
}

std::vector<CachedTicket> GitHubClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* /*configOverride*/,
                                                   const ViewsStore* /*viewsOverride*/, std::string* outFetchError,
                                                   std::string* /*outWarning*/) {
    if (outFullSyncCompleted) {
        *outFullSyncCompleted = false;
    }
    if (outFetchError) {
        *outFetchError = "FetchIssues is not supported on GitHub backend yet.";
    }
    return {};
}

bool GitHubClient::FetchIssuesForKeys(const TrackerConfig& /*cfg*/, const std::vector<std::string>& /*issueKeys*/,
                                      const ViewsStore& /*views*/, std::vector<CachedTicket>& outTickets,
                                      std::string& outError) {
    outTickets.clear();
    outError = "FetchIssuesForKeys is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::UpdateIssueFields(const std::string& /*issueId*/, const nlohmann::json& /*fields*/,
                                     std::string& outError) {
    outError = "UpdateIssueFields is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::UpdateField(const std::string& /*issueId*/, const TrackerField& /*field*/,
                               const std::vector<std::string>& /*values*/, std::string& outError) {
    outError = "UpdateField is not supported on GitHub backend yet.";
    return false;
}

bool GitHubClient::BuildFieldPayload(const TrackerField& /*field*/, const std::vector<std::string>& /*values*/,
                                     nlohmann::json& /*outPayload*/, std::string& outError) {
    outError = "BuildFieldPayload is not supported on GitHub backend yet.";
    return false;
}

std::string GitHubClient::ResolveDisplayValue(const std::string& /*fieldId*/, const TrackerField* /*field*/,
                                              const std::string& value) const {
    // No field catalog yet — identity is the safest displayable value pre-catalog.
    return value;
}

bool GitHubClient::FetchIssueComments(const std::string& issueKey, std::vector<TrackerIssueComment>& outComments,
                                      std::string& outError) {
    outComments.clear();
    outError.clear();

    if (pat_.empty()) {
        outError = "GitHub PAT not configured (set cfg.GitHubPat).";
        return false;
    }

    GitHubClientHelpers::ParsedIssueKey parsed;
    std::string parseErr;
    if (!GitHubClientHelpers::ParseGitHubIssueKey(issueKey, parsed, parseErr)) {
        outError = parseErr;
        return false;
    }

    std::ostringstream numOss;
    numOss << parsed.Number;
    const std::string url = baseUrl_ + "/repos/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + numOss.str() +
                            "/comments";

    cpr::Header headers{
        {"Accept", "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
        {"User-Agent", "Smatchet/agentic-flow"},
        {"Authorization", std::string("Bearer ") + pat_},
    };
    cpr::Response resp = cpr::Get(cpr::Url{url}, headers,
                                  cpr::ConnectTimeout{kGitHubConnectTimeoutMs},
                                  cpr::Timeout{kGitHubOverallTimeoutMs});

    if (resp.status_code != 200) {
        std::ostringstream oss;
        oss << "GitHub /comments HTTP " << resp.status_code;
        if (!resp.error.message.empty()) {
            oss << " (" << resp.error.message << ")";
        }
        // Body redaction is defense-in-depth: GitHub 401/403 bodies have been observed
        // echoing the Authorization header in nginx-style upstream error pages.
        if (!resp.text.empty()) {
            const std::string redacted = RedactForLog(resp.text);
            LOG_WARN("GitHubClient::FetchIssueComments failed: %s — body=%s", oss.str().c_str(), redacted.c_str());
        } else {
            LOG_WARN("GitHubClient::FetchIssueComments failed: %s", oss.str().c_str());
        }
        outError = oss.str();
        return false;
    }

    nlohmann::json arr;
    try {
        arr = nlohmann::json::parse(resp.text);
    } catch (const nlohmann::json::parse_error& e) {
        outError = std::string("GitHub /comments response is not valid JSON: ") + e.what();
        return false;
    }
    if (!arr.is_array()) {
        outError = "GitHub /comments response is not a JSON array.";
        return false;
    }

    outComments.reserve(arr.size());
    bool isoParseWarned = false;
    for (const auto& item : arr) {
        if (!item.is_object()) {
            continue;
        }
        TrackerIssueComment c;
        if (item.contains("id") && item["id"].is_number_integer()) {
            std::ostringstream idOss;
            idOss << item["id"].get<std::int64_t>();
            c.Id = idOss.str();
        }
        if (item.contains("user") && item["user"].is_object()) {
            const auto& user = item["user"];
            if (user.contains("login") && user["login"].is_string()) {
                c.Author = user["login"].get<std::string>();
            }
        }
        if (item.contains("body") && item["body"].is_string()) {
            c.Body = item["body"].get<std::string>();
        }
        if (item.contains("created_at") && item["created_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["created_at"].get<std::string>(), ts, isoErr)) {
                c.CreatedAtSec = ts;
            } else if (!isoParseWarned) {
                LOG_WARN("GitHubClient::FetchIssueComments: created_at parse failed: %s", isoErr.c_str());
                isoParseWarned = true;
            }
        }
        if (item.contains("updated_at") && item["updated_at"].is_string()) {
            std::int64_t ts = 0;
            std::string isoErr;
            if (GitHubClientHelpers::ParseIso8601ToUnixSec(item["updated_at"].get<std::string>(), ts, isoErr)) {
                c.UpdatedAtSec = ts;
            } else {
                c.UpdatedAtSec = c.CreatedAtSec;
            }
        } else {
            c.UpdatedAtSec = c.CreatedAtSec;
        }
        outComments.push_back(std::move(c));
    }
    return true;
}
