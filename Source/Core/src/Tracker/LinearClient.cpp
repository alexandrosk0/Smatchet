#include "LinearClient.h"

#include "ConfigManager.h"
#include "Json/BoundedJsonParse.h"
#include "LinearClientHelpers.h"
#include "LinearIssueSearch.h"
#include "LinearQueryFromJql.h"
#include "Logger.h"
#include "TrackerFieldSchema.h"
#include "TrackerHttpPure.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include <cctype>
#include <cstddef>
#include <map>
#include <utility>

// LinearClient — fourth tracker backend (docs/plans/linear-tracker-backend.md).
// This TU holds the read shell: GraphQL transport + connectivity + field catalog +
// read sync. The mutation surface (issueUpdate/issueCreate/commentCreate) lives in
// LinearIssueMutation.cpp (slice 3), split out like JiraIssueMutation.cpp.
// Mirrors GitHubClient's shell precisely (per-request #979 credential resolution,
// thin-adapter/pure-TU split, role self-returns). All GraphQL POSTs route through
// the shared TrackerPostLogged helper — GraphQL is POST-only.

using smatchet::linear::BuildLinearHeaders;

namespace {

const char* const kApiKeyMissingError = "Linear API key not configured (set Preferences > Tracker > Linear API key)";

// Fixed Linear priority scale (Int 0–4). Stable IDs; the field catalog reuses
// these for the read-only priority column (plan § Approach: priority is the
// fixed enum, no fetch).
struct PriorityOption {
    const char* id;
    const char* label;
};
const PriorityOption kPriorityOptions[] = {
    {"0", "No priority"}, {"1", "Urgent"}, {"2", "High"}, {"3", "Medium"}, {"4", "Low"},
};

// Lowercase an ASCII string for the rate-limit header fold (cpr keys are
// mixed-case; the cpr-free parser keys on lowercase names).
std::string ToLowerAscii(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(s[i]))));
    }
    return out;
}

// True when a parsed `{ viewer { id name } }` response carries a non-empty
// viewer.id (the authenticated-reachable signal). `outName` receives viewer.name
// when present. Safe on any shape (Pillar 3).
bool ProbeViewerIdPresent(const nlohmann::json& parsed, std::string& outName) {
    outName.clear();
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("viewer") || !parsed["data"]["viewer"].is_object()) {
        return false;
    }
    const nlohmann::json& viewer = parsed["data"]["viewer"];
    outName = viewer.value("name", std::string());
    return !viewer.value("id", std::string()).empty();
}

// Log a single probe/catalog response's rate-limit headers at DEBUG.
void LogRateLimitHeaders(const cpr::Header& headers) {
    std::map<std::string, std::string> lowered;
    for (cpr::Header::const_iterator it = headers.begin(); it != headers.end(); ++it) {
        lowered[ToLowerAscii(it->first)] = it->second;
    }
    const smatchet::linear::LinearRateLimit rl = smatchet::linear::ParseLinearRateLimitHeaders(lowered);
    if (!rl.Present()) {
        return;
    }
    LOG_DEBUG("LinearClient: rate-limit requests=%ld/%ld reset=%lld complexity=%ld/%ld cost=%ld", rl.RequestsRemaining,
              rl.RequestsLimit, static_cast<long long>(rl.RequestsResetUnixSec), rl.ComplexityRemaining,
              rl.ComplexityLimit, rl.Complexity);
}

// Resolve the active view's query text, mirroring the Plane/Jira convention:
// the active view's Jql wins; an empty/absent active view falls back to the
// settled cfg.JqlQuery (the global query box). This is the text fed to the
// JQL-subset → IssueFilter translator.
std::string ActiveViewQueryFromStore(const ViewsStore* views, const TrackerConfig& cfg) {
    if (views != nullptr) {
        for (const ViewDefinition& view : views->Views) {
            if (view.Id == views->ActiveViewId) {
                if (!view.Jql.empty()) {
                    return view.Jql;
                }
                break;
            }
        }
    }
    return cfg.JqlQuery;
}

} // namespace

ITrackerIssueReader& LinearClient::Reader() { return *this; }
ITrackerConnectivity& LinearClient::Connectivity() { return *this; }
ITrackerFieldCatalog* LinearClient::FieldCatalog() { return this; }
ITrackerIssueMutations* LinearClient::Mutations() { return this; }
// Collaboration (comment posting) self-returns as of slice 3 (AddIssueCommentPlain
// in LinearIssueMutation.cpp). Activity (feed) stays nullptr — the activity-feed
// role is out of MVP scope (facade contract is "nullptr if unsupported",
// ITrackerBackend.h).
ITrackerCollaboration* LinearClient::Collaboration() { return this; }
ITrackerActivity* LinearClient::Activity() { return nullptr; }

LinearClient::LinearClient(const std::string& baseUrl, const std::string& apiKey)
    : baseUrl_(baseUrl.empty() ? std::string(smatchet::linear::kDefaultLinearApiUrl) : baseUrl), apiKey_(apiKey),
      lastLoggedKeyBytes_(apiKey.size()) {
    LOG_INFO("LinearClient: ctor baseUrl='%s' key_bytes=%zu", baseUrl_.c_str(), apiKey_.size());
}

smatchet::linear::LinearRequestAuth LinearClient::ResolveAuth(const TrackerConfig* configOverride) const {
    // Issue #979 — per-request credential resolution (see header). cfg-less call
    // paths read the settled on-disk config; by mutation/probe time the debounced
    // prefs save has flushed. The live key is authoritative (empty live key =
    // user cleared it), the API URL falls back to the ctor snapshot.
    TrackerConfig loaded;
    const TrackerConfig* cfg = configOverride;
    if (!cfg) {
        loaded = ConfigManager::Load();
        cfg = &loaded;
    }
    const smatchet::linear::LinearRequestAuth auth =
        smatchet::linear::ResolveLinearRequestAuth(cfg->LinearBaseUrl, cfg->LinearApiKey, baseUrl_);
    // Rotation visibility — pairs with the ctor `key_bytes=` line. atomic
    // exchange: this const method runs concurrently on the sync worker, the
    // connectivity-probe async, and the UI thread (mirrors GitHubClient).
    const std::size_t prev = lastLoggedKeyBytes_.exchange(auth.ApiKey.size(), std::memory_order_relaxed);
    if (auth.ApiKey.size() != prev) {
        LOG_INFO("LinearClient: per-request credential resolution key_bytes=%zu (ctor snapshot=%zu, source=%s)",
                 auth.ApiKey.size(), apiKey_.size(), configOverride ? "live-cfg" : "disk");
    }
    return auth;
}

std::string LinearClient::GetTrackerType() const { return "Linear"; }

TrackerReachabilityProbeResult LinearClient::ProbeReachability(const TrackerConfig& cfg) {
    // `query { viewer { id name } }` — the cheapest authenticated Linear call.
    // viewer.id present → AuthenticatedReachable; auth/4xx or GraphQL errors →
    // ReachableAuthOrConfigError; a transport-shaped error → TransportDown (the
    // same shape Jira/GitHub probes use so the connectivity banner is uniform).
    // Issue #979 — resolve from the caller's live cfg so a key entered after this
    // client was constructed makes the very next probe succeed.
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg);
    TrackerReachabilityProbeResult out;
    if (auth.ApiKey.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = kApiKeyMissingError;
        return out;
    }

    const std::string body =
        smatchet::linear::BuildGraphQLBody("query { viewer { id name } }", nlohmann::json::object());
    const cpr::Response resp = TrackerPostLogged("LinearClient", auth.ApiUrl, BuildLinearHeaders(auth.ApiKey), body);
    LogRateLimitHeaders(resp.header);

    const long sc = resp.status_code;
    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    const bool hasErrors = !parsed.is_discarded() && smatchet::linear::LinearResponseHasErrors(parsed, errorMessage);

    std::string viewerName;
    if (sc == 200 && !hasErrors && ProbeViewerIdPresent(parsed, viewerName)) {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        out.Diagnostic = viewerName.empty() ? std::string("HTTP 200") : (std::string("HTTP 200 (") + viewerName + ")");
        return out;
    }

    // A transport-shaped failure (HTTP 0 / timeout / DNS) → offline banner. N12 item 13b: the
    // cpr-level signal (status <= 0, or a curl error message) is the transport authority — the
    // old text sniff could misread a GraphQL error BODY mentioning "timeout" (HTTP 200) as an
    // outage and flip the offline banner on a reachable server.
    const std::string transportMsg = resp.error.message.empty() ? errorMessage : resp.error.message;
    if (sc <= 0 || !resp.error.message.empty()) {
        out.Kind = TrackerReachabilityProbeKind::TransportDown;
        out.Diagnostic = transportMsg.empty() ? std::string("Unknown network error") : transportMsg;
        return out;
    }

    if (sc >= 500 && sc < 600) {
        out.Kind = TrackerReachabilityProbeKind::ServiceUnavailable;
        out.Diagnostic = std::string("HTTP ") + std::to_string(sc);
        return out;
    }

    // Everything else — auth (401/403), 200-with-errors, or a 4xx config error —
    // is "reached us but auth/config is off". Same banner shape across backends.
    out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
    const std::string detail =
        hasErrors ? errorMessage : smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(sc), resp.text);
    out.Diagnostic = std::string("HTTP ") + std::to_string(sc) + (detail.empty() ? "" : (" (" + detail + ")"));
    return out;
}

std::string LinearClient::ExtractProjectFromQuery(const std::string& query) const {
    // Linear's draft scope anchor is the team key — best-effort extract from a
    // `team = X` clause in the JQL-shaped query. The translator owns the full
    // filter; here we only need the bare team key for project-resolution plumbing.
    const auto translated = smatchet::linear::TranslateJqlToLinearFilter(query);
    if (translated.HasFilter()) {
        nlohmann::json::const_iterator teamIt = translated.Filter.find("team");
        if (teamIt != translated.Filter.end() && teamIt->is_object()) {
            nlohmann::json::const_iterator keyIt = teamIt->find("key");
            if (keyIt != teamIt->end() && keyIt->is_object()) {
                nlohmann::json::const_iterator eqIt = keyIt->find("eq");
                if (eqIt != keyIt->end() && eqIt->is_string()) {
                    return eqIt->get<std::string>();
                }
            }
        }
    }
    return "";
}

std::vector<RemoteProject> LinearClient::ListProjects() {
    // Linear "projects" in Smatchet's draft-scope sense ARE Linear teams (plan §
    // Approach). cfg-less interface — resolve from the settled on-disk config.
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(nullptr);
    std::vector<RemoteProject> out;
    if (auth.ApiKey.empty()) {
        LOG_WARN("LinearClient::ListProjects: no API key configured");
        return out;
    }
    const std::string body = smatchet::linear::BuildGraphQLBody("query { teams(first: 100) { nodes { id key name } } }",
                                                                nlohmann::json::object());
    const cpr::Response resp = TrackerPostLogged("LinearClient", auth.ApiUrl, BuildLinearHeaders(auth.ApiKey), body);
    LogRateLimitHeaders(resp.header);

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    if (resp.status_code != 200 || parsed.is_discarded() ||
        smatchet::linear::LinearResponseHasErrors(parsed, errorMessage)) {
        LOG_ERROR(
            "LinearClient::ListProjects: HTTP %ld — %s", resp.status_code,
            errorMessage.empty()
                ? smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text).c_str()
                : errorMessage.c_str());
        return out;
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("teams") || !parsed["data"]["teams"].is_object()) {
        LOG_ERROR("LinearClient::ListProjects: response missing data.teams");
        return out;
    }
    const nlohmann::json& nodes = parsed["data"]["teams"].value("nodes", nlohmann::json::array());
    if (!nodes.is_array()) {
        return out;
    }
    for (nlohmann::json::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (!it->is_object()) {
            continue;
        }
        RemoteProject p;
        p.id = it->value("id", std::string());
        p.key = it->value("key", std::string());
        p.displayName = it->value("name", std::string());
        out.push_back(p);
    }
    LOG_INFO("LinearClient::ListProjects: %zu teams", out.size());
    return out;
}

std::vector<CachedTicket> LinearClient::FetchIssues(bool* outFullSyncCompleted, const TrackerConfig* configOverride,
                                                    const ViewsStore* viewsOverride, std::string* outFetchError,
                                                    std::string* outWarning, TrackerError* outFetchErrorStructured) {
    // Thin shim — resolve config + active-view query, then delegate to the
    // standalone GraphQL fetcher (mirrors GitHubClient::FetchIssues). Called on a
    // worker thread by TicketSyncService (never the UI thread, Pillar 2).
    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg); // issue #979 — live-cfg credentials
    if (auth.ApiKey.empty()) {
        if (outFetchError) {
            *outFetchError = kApiKeyMissingError;
        }
        if (outFetchErrorStructured) {
            // Same classification the key-less FetchIssuesForKeys path already uses.
            *outFetchErrorStructured = TrackerErrorAuth(kApiKeyMissingError);
        }
        return {};
    }
    const std::string viewQuery = ActiveViewQueryFromStore(viewsOverride, cfg);
    return smatchet::linear::FetchIssuesViaGraphQl(auth.ApiUrl, auth.ApiKey, cfg.LinearTeamId, cfg.LinearTeamKey,
                                                   viewQuery, outFullSyncCompleted, outFetchError, outWarning, nullptr,
                                                   outFetchErrorStructured);
}

TrackerIssueFetchSummary LinearClient::FetchIssuesStreamed(const BatchCallback& onBatch,
                                                           const CancelCallback& shouldCancel,
                                                           const TrackerConfig* configOverride,
                                                           const ViewsStore* viewsOverride) {
    // Per-page emission (mirrors GitHubClient::FetchIssuesStreamed) — each mapped
    // page is posted to onBatch immediately so the grid populates progressively.
    TrackerIssueFetchSummary summary;
    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg); // issue #979 — live-cfg credentials
    if (auth.ApiKey.empty()) {
        summary.FetchError = kApiKeyMissingError;
        summary.Error = TrackerErrorAuth(kApiKeyMissingError);
        return summary;
    }

    std::string fetchError;
    TrackerError fetchErrorStructured;
    std::string fetchWarning;
    bool fullSyncCompleted = false;
    std::size_t pageCount = 0;
    std::size_t totalEmitted = 0;

    auto onPage = [&](const std::vector<CachedTicket>& page, bool isLast) {
        ++pageCount;
        totalEmitted += page.size();
        LOG_INFO("LinearClient::FetchIssuesStreamed: emitting page #%zu size=%zu isLast=%d", pageCount, page.size(),
                 isLast ? 1 : 0);
        if (!onBatch) {
            return;
        }
        if (shouldCancel && shouldCancel()) {
            return;
        }
        if (page.empty()) {
            return;
        }
        // The fetcher returns const& pages; per-batch copy is cheap (≤50 tickets)
        // and avoids duplicating the fetcher signature for a movable buffer.
        std::vector<CachedTicket> batch = page;
        onBatch(std::move(batch));
    };

    const std::string viewQuery = ActiveViewQueryFromStore(viewsOverride, cfg);
    smatchet::linear::FetchIssuesViaGraphQl(auth.ApiUrl, auth.ApiKey, cfg.LinearTeamId, cfg.LinearTeamKey, viewQuery,
                                            &fullSyncCompleted, &fetchError, &fetchWarning, onPage,
                                            &fetchErrorStructured);

    summary.FetchedCount = totalEmitted;
    summary.FullSyncCompleted = fullSyncCompleted;
    summary.FetchError = fetchError;
    summary.Error = fetchErrorStructured;
    summary.Warning = fetchWarning;
    LOG_INFO("LinearClient::FetchIssuesStreamed: done pages=%zu total=%zu fullSync=%d err='%s'", pageCount,
             totalEmitted, fullSyncCompleted ? 1 : 0, fetchError.c_str());
    return summary;
}

Result<std::vector<CachedTicket>, TrackerError>
LinearClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                 const ViewsStore& /*views*/) {
    // Single batched `or:[{team,number}]` query per call (issue #979 — live-cfg
    // credentials). The team scope is embedded per-key in the filter, so the
    // configured team isn't consulted here.
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg);
    if (auth.ApiKey.empty()) {
        return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorAuth(kApiKeyMissingError));
    }
    return smatchet::linear::FetchIssuesForKeysViaGraphQl(auth.ApiUrl, auth.ApiKey, issueKeys);
}

std::string LinearClient::ResolveDisplayValue(const std::string& /*fieldId*/, const TrackerField* field,
                                              const std::string& value) const {
    // Mirror GitHub: select-like values are already display-ready strings on the
    // mapped ticket. When the grid hands us an option Id (UUID), resolve it to
    // the option's display text via the field's AllowedValueOptions.
    if (field != nullptr) {
        for (std::size_t i = 0; i < field->AllowedValueOptions.size(); ++i) {
            if (field->AllowedValueOptions[i].Id == value) {
                return field->AllowedValueOptions[i].Value;
            }
        }
    }
    return value;
}

std::string LinearClient::BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const {
    // Slice 1: the persisted `url` field (LinearIssueMappingPure writes the issue
    // `url` into CachedTicket::fieldValues["url"]) drives the browse action, so
    // there is no key→URL reconstruction here. Returning empty avoids guessing a
    // broken workspace URL (plan § Approach). Slice 2+ may compose from the
    // workspace URL if a key arrives without a cached url.
    return "";
}

namespace {

// Append a select-like field built from a Linear `{nodes:[{id,name|displayName}]}`
// collection. `valueKey` selects the display source ("name" or "displayName").
void AddCatalogSelectField(TrackerFieldCatalogResult& catalog, const char* id, const char* name,
                           TrackerFieldFamily family, const nlohmann::json& containerObj, const char* valueKey) {
    TrackerField f;
    f.Id = id;
    f.Name = name;
    f.Type = "string";
    f.Family = family;
    if (containerObj.is_object()) {
        const nlohmann::json& nodes = containerObj.value("nodes", nlohmann::json::array());
        if (nodes.is_array()) {
            for (nlohmann::json::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
                if (!it->is_object()) {
                    continue;
                }
                TrackerFieldOption opt;
                opt.Id = it->value("id", std::string());
                opt.Value = it->value(valueKey, std::string());
                if (opt.Value.empty()) {
                    opt.Value = it->value("name", std::string());
                }
                f.AllowedValueOptions.push_back(opt);
                if (!opt.Value.empty()) {
                    f.AllowedValues.push_back(opt.Value);
                }
            }
        }
    }
    catalog.Fields.push_back(std::move(f));
}

// Append the fixed 0–4 priority field (no fetch — plan § Approach).
void AddCatalogPriorityField(TrackerFieldCatalogResult& catalog) {
    TrackerField f;
    f.Id = "priority";
    f.Name = "Priority";
    f.Type = "string";
    f.Family = TrackerFieldFamily::SelectSingle;
    for (std::size_t i = 0; i < sizeof(kPriorityOptions) / sizeof(kPriorityOptions[0]); ++i) {
        TrackerFieldOption opt;
        opt.Id = kPriorityOptions[i].id;
        opt.Value = kPriorityOptions[i].label;
        f.AllowedValueOptions.push_back(opt);
        f.AllowedValues.push_back(opt.Value);
    }
    catalog.Fields.push_back(std::move(f));
}

// Resolve the team UUID for the catalog query: cfg.LinearTeamId wins; otherwise
// look up the team whose key matches projectKey (preferred) or cfg.LinearTeamKey
// among the workspace teams. Returns empty when nothing resolves.
std::string ResolveCatalogTeamId(const std::string& apiUrl, const std::string& apiKey, const std::string& teamIdCfg,
                                 const std::string& teamKeyCfg, const std::string& projectKey) {
    if (!teamIdCfg.empty()) {
        return teamIdCfg;
    }
    const std::string wantKey = !projectKey.empty() ? projectKey : teamKeyCfg;
    if (wantKey.empty()) {
        return "";
    }
    const std::string body = smatchet::linear::BuildGraphQLBody("query { teams(first: 100) { nodes { id key name } } }",
                                                                nlohmann::json::object());
    const cpr::Response resp = TrackerPostLogged("LinearClient", apiUrl, BuildLinearHeaders(apiKey), body);
    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    if (resp.status_code != 200 || parsed.is_discarded() ||
        smatchet::linear::LinearResponseHasErrors(parsed, errorMessage) || !parsed.is_object() ||
        !parsed.contains("data") || !parsed["data"].is_object() || !parsed["data"].contains("teams")) {
        return "";
    }
    const nlohmann::json& nodes = parsed["data"]["teams"].value("nodes", nlohmann::json::array());
    if (!nodes.is_array()) {
        return "";
    }
    for (nlohmann::json::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        if (it->is_object() && it->value("key", std::string()) == wantKey) {
            return it->value("id", std::string());
        }
    }
    return "";
}

} // namespace

Result<TrackerFieldCatalogResult, TrackerError> LinearClient::FetchFieldCatalog(const TrackerConfig& cfg,
                                                                                const std::string& projectKey) {
    using CatalogResult = Result<TrackerFieldCatalogResult, TrackerError>;
    // Per-team catalog: status (states), assignee (members), labels, project, +
    // the fixed priority enum. Option UUIDs in TrackerFieldOption::Id, display in
    // ::Value (plan § Approach). cfg-carrying — resolve from the live cfg (#979).
    const smatchet::linear::LinearRequestAuth auth = ResolveAuth(&cfg);
    if (auth.ApiKey.empty()) {
        return CatalogResult::Err(TrackerErrorAuth(kApiKeyMissingError));
    }
    const std::string teamId =
        ResolveCatalogTeamId(auth.ApiUrl, auth.ApiKey, cfg.LinearTeamId, cfg.LinearTeamKey, projectKey);
    if (teamId.empty()) {
        return CatalogResult::Err(TrackerErrorInvalidRequest(
            "LinearClient::FetchFieldCatalog: no Linear team configured (set Preferences > Tracker > Linear team)"));
    }

    const char* const kCatalogQuery =
        "query($id:String!){ team(id:$id){ id key name "
        "states{nodes{id name type}} labels{nodes{id name}} members{nodes{id name displayName}} "
        "projects{nodes{id name}} } }";
    nlohmann::json variables = nlohmann::json::object();
    variables["id"] = teamId;
    const std::string body = smatchet::linear::BuildGraphQLBody(kCatalogQuery, variables);
    const cpr::Response resp = TrackerPostLogged("LinearClient", auth.ApiUrl, BuildLinearHeaders(auth.ApiKey), body);
    LogRateLimitHeaders(resp.header);

    // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
    nlohmann::json parsed = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
    std::string errorMessage;
    if (resp.status_code != 200 || parsed.is_discarded() ||
        smatchet::linear::LinearResponseHasErrors(parsed, errorMessage)) {
        const std::string msg =
            errorMessage.empty()
                ? smatchet::linear::ExtractLinearErrorMessage(static_cast<int>(resp.status_code), resp.text)
                : errorMessage;
        LOG_ERROR("LinearClient::FetchFieldCatalog: HTTP %ld — %s", resp.status_code, msg.c_str());
        // This branch also fires on a 2xx (200-with-GraphQL-errors, or a
        // 2xx-non-200) — cases TrackerErrorFromHttpStatus would classify as Ok()
        // (Kind==None, detail discarded), silently swallowing the failure. Carry
        // the detail under an explicit non-OK kind (DR20).
        // SMATCHET_DEVIATION(rule=duplication; reason=the 2xx-non-200 guard idiom (LOG_ERROR + `if 2xx return TrackerErrorUnknown(detail,status)` else FromHttpStatus) is deliberately uniform across the tracker read paths (mirrors GitHubIssueSearch + GitHubClient::CreateIssue + JiraUserAndMeta) so the swallow-as-Ok bug is fixed identically everywhere; extracting a helper would need a Result-type-generic wrapper spanning independent client TUs; owner=deep-review; revisit=2026-10-01)
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return CatalogResult::Err(TrackerErrorUnknown(msg, static_cast<int>(resp.status_code)));
        }
        return CatalogResult::Err(TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), msg));
    }
    if (!parsed.is_object() || !parsed.contains("data") || !parsed["data"].is_object() ||
        !parsed["data"].contains("team") || !parsed["data"]["team"].is_object()) {
        return CatalogResult::Err(TrackerErrorParse("Linear field-catalog response missing data.team"));
    }

    const nlohmann::json& team = parsed["data"]["team"];
    TrackerFieldCatalogResult outCatalog;
    AddCatalogSelectField(outCatalog, "status", "Status", TrackerFieldFamily::Status,
                          team.value("states", nlohmann::json::object()), "name");
    AddCatalogSelectField(outCatalog, "assignee", "Assignee", TrackerFieldFamily::UserSingle,
                          team.value("members", nlohmann::json::object()), "displayName");
    AddCatalogSelectField(outCatalog, "labels", "Labels", TrackerFieldFamily::Labels,
                          team.value("labels", nlohmann::json::object()), "name");
    AddCatalogPriorityField(outCatalog);
    AddCatalogSelectField(outCatalog, "project", "Project", TrackerFieldFamily::SelectSingle,
                          team.value("projects", nlohmann::json::object()), "name");
    LOG_INFO("LinearClient::FetchFieldCatalog: team=%s fields=%zu", teamId.c_str(), outCatalog.Fields.size());
    return CatalogResult::Ok(std::move(outCatalog));
}

Result<std::unordered_map<std::string, bool>, TrackerError>
LinearClient::FetchIssueEditMeta(const TrackerConfig& cfg, const std::string& /*issueKeyOrId*/) {
    using EditMetaResult = Result<std::unordered_map<std::string, bool>, TrackerError>;
    // Linear has no per-issue editmeta/permission API — every catalog field is
    // editable with a write-scoped key. Return an all-true map for the catalog
    // field ids so AppController caches a positive result and stops re-fetching
    // per UI frame (mirrors GitHubClient's all-true rationale).
    std::unordered_map<std::string, bool> outFieldIdCanEdit;
    Result<TrackerFieldCatalogResult, TrackerError> catalog = FetchFieldCatalog(cfg, std::string());
    if (catalog) {
        const std::vector<TrackerField>& fields = catalog.value().Fields;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            outFieldIdCanEdit[fields[i].Id] = true;
        }
    } else {
        // Catalog fetch failed (no team / transport) — fall back to the static
        // field ids the mapper always writes so the grid still has a positive map.
        const char* const kStaticEditable[] = {"summary", "description", "status", "assignee",
                                               "labels",  "priority",    "project"};
        for (const char* id : kStaticEditable) {
            outFieldIdCanEdit[id] = true;
        }
    }
    return EditMetaResult::Ok(std::move(outFieldIdCanEdit));
}

// UpdateIssueFields / UpdateField / BuildFieldPayload / BuildCreatePayload /
// CreateIssue (ITrackerIssueMutations) + AddIssueCommentPlain (ITrackerCollaboration)
// live in LinearIssueMutation.cpp — the GraphQL mutation surface is split out of
// this read shell exactly like JiraIssueMutation.cpp / PlaneIssueMutation.cpp.
