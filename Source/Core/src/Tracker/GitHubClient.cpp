#include "GitHubClient.h"

#include "BackendAuditTrail.h"
#include "ConfigManager.h"
#include "GitHubClientHelpers.h"
#include "GitHubCommentMappingPure.h"
#include "GitHubIssueSearch.h"
#include "GitHubQueryFromJql.h"
#include "IssueDraft.h"
#include "Json/BoundedJsonParse.h"
#include "LabelEditDiffPure.h"
#include "Logger.h"
#include "TrackerFieldSchema.h"
#include "TrackerHttpUtils.h"

#include <cpr/cpr.h>

#include <iterator>
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

// PR-only columns populated by the per-PR /pulls/{n} enrichment loop
// in GitHubIssueSearch.cpp. Read-only on the grid; never targeted by
// UpdateField (mergeable + draft + head/base are not user-editable from
// Smatchet's grid).
const char* const kPrHeadLabel = "PR Head Branch";
const char* const kPrBaseLabel = "PR Base Branch";
const char* const kPrMergeableLabel = "PR Mergeable";
const char* const kPrDraftLabel = "PR Draft";

const char* const kPatMissingError = "GitHub PAT not configured (set Preferences > Tracker > GitHub PAT)";

void StubError(std::string& out, const char* method) {
    out = std::string(method) + ": GitHubClient HTTP impl deferred to a follow-up slice of "
                                "docs/plans/shipped/github-tracker-backend.md";
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

ITrackerIssueReader& GitHubClient::Reader() { return *this; }
ITrackerConnectivity& GitHubClient::Connectivity() { return *this; }
ITrackerFieldCatalog* GitHubClient::FieldCatalog() { return this; }
ITrackerIssueMutations* GitHubClient::Mutations() { return this; }
ITrackerCollaboration* GitHubClient::Collaboration() { return this; }
ITrackerActivity* GitHubClient::Activity() { return this; }

GitHubClient::GitHubClient(const std::string& baseUrl, const std::string& pat)
    : baseUrl_(baseUrl.empty() ? std::string("https://api.github.com") : baseUrl), pat_(pat),
      lastLoggedPatBytes_(pat.size()) {
    LOG_INFO("GitHubClient: ctor baseUrl='%s' pat_bytes=%zu", baseUrl_.c_str(), pat_.size());
}

smatchet::github::GitHubRequestAuth GitHubClient::ResolveAuth(const TrackerConfig* configOverride) const {
    // Issue #979 — per-request credential resolution (see header). cfg-less call paths
    // (UpdateField / CreateIssue) read the settled on-disk config, mirroring
    // JiraIssueMutation's per-request ConfigManager::Load() pattern; by mutation time the
    // debounced prefs save has long flushed.
    TrackerConfig loaded;
    const TrackerConfig* cfg = configOverride;
    if (!cfg) {
        loaded = ConfigManager::Load();
        cfg = &loaded;
    }
    const smatchet::github::GitHubRequestAuth auth =
        smatchet::github::ResolveGitHubRequestAuth(cfg->GitHubBaseUrl, cfg->GitHubPat, baseUrl_);
    // Rotation visibility — pairs with the ctor `pat_bytes=` line so the fixed flow
    // (stale ctor snapshot superseded by the live cfg) is verifiable in logs. atomic
    // exchange: this const method runs concurrently on the sync worker, the
    // connectivity-probe async, and the UI thread (review 2026-06-07).
    const std::size_t prev = lastLoggedPatBytes_.exchange(auth.Pat.size(), std::memory_order_relaxed);
    if (auth.Pat.size() != prev) {
        LOG_INFO("GitHubClient: per-request credential resolution pat_bytes=%zu (ctor snapshot=%zu, source=%s)",
                 auth.Pat.size(), pat_.size(), configOverride ? "live-cfg" : "disk");
    }
    return auth;
}

std::string GitHubClient::GetTrackerType() const { return "github"; }

TrackerReachabilityProbeResult GitHubClient::ProbeReachability(const TrackerConfig& cfg) {
    // GET /rate_limit — cheap, always available even on free PATs, returns auth
    // status + remaining quota. Maps HTTP codes to TrackerReachabilityProbeKind
    // the same shape JiraClient::ProbeReachability uses so the connectivity
    // banner doesn't care which backend is active.
    // Issue #979 — resolve from the caller's live cfg so a PAT entered after this
    // client was constructed makes the very next probe succeed.
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    TrackerReachabilityProbeResult out;
    if (auth.Pat.empty()) {
        out.Kind = TrackerReachabilityProbeKind::ReachableAuthOrConfigError;
        out.Diagnostic = kPatMissingError;
        return out;
    }

    const std::string url = auth.BaseUrl + "/rate_limit";
    const cpr::Header headers = BuildGitHubHeaders(auth.Pat);
    const cpr::Response resp =
        TrackerGetLogged("GitHubClient", url, headers, kTrackerProbeConnectTimeoutMs, kTrackerProbeOverallTimeoutMs);

    const long sc = resp.status_code;
    if (sc == 200) {
        out.Kind = TrackerReachabilityProbeKind::AuthenticatedReachable;
        // Extract core rate-limit numbers if the body parses; non-fatal on parse failure.
        std::ostringstream oss;
        oss << "HTTP 200";
        try {
            // Bounded parse — the try can't catch a depth-bomb's destructor-time SIGSEGV
            // (audit: unbounded-recursion-DoS). Discarded on failure → the guards below skip.
            const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
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
    // Delegate to the standalone fetcher (mirrors JiraIssueSearch /
    // PlaneIssueSearch convention; the per-class FetchIssues is just a thin
    // shim that resolves config + forwards). Called from
    // TicketSyncService::SyncWithBackend on a worker thread (never the UI
    // thread, per Pillar 2).
    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg); // issue #979 — live-cfg credentials
    return smatchet::github::FetchIssuesViaRestApi(auth.BaseUrl, auth.Pat, cfg.GitHubOwner, cfg.GitHubRepo,
                                                   cfg.JqlQuery, outFullSyncCompleted, outFetchError, outWarning);
}

TrackerIssueFetchSummary GitHubClient::FetchIssuesStreamed(const BatchCallback& onBatch,
                                                           const CancelCallback& shouldCancel,
                                                           const TrackerConfig* configOverride,
                                                           const ViewsStore* /*viewsOverride*/) {
    // Latency fix — per-page emission. Each GraphQL page's mapped tickets
    // are posted to `onBatch` immediately, so the UI grid populates
    // progressively instead of waiting for all 4 pages (~6s wall-clock).
    TrackerIssueFetchSummary summary;
    const TrackerConfig cfg = configOverride ? *configOverride : ConfigManager::Load();

    std::string fetchError;
    std::string fetchWarning;
    bool fullSyncCompleted = false;
    std::size_t pageCount = 0;
    std::size_t totalEmitted = 0;

    auto onPage = [&](const std::vector<CachedTicket>& page, bool isLast) {
        ++pageCount;
        totalEmitted += page.size();
        LOG_INFO("GitHubClient::FetchIssuesStreamed: emitting page #%zu size=%zu isLast=%d", pageCount, page.size(),
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
        // Copy into a movable buffer — the per-page helper returns const& so we
        // can't move directly. Per-batch copy is cheap (≤100 tickets) and the
        // alternative is duplicating the helper's signature.
        std::vector<CachedTicket> batch = page;
        onBatch(std::move(batch));
    };

    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg); // issue #979 — live-cfg credentials
    smatchet::github::FetchIssuesViaRestApi(auth.BaseUrl, auth.Pat, cfg.GitHubOwner, cfg.GitHubRepo, cfg.JqlQuery,
                                            &fullSyncCompleted, &fetchError, &fetchWarning, onPage);

    summary.FetchedCount = totalEmitted;
    summary.FullSyncCompleted = fullSyncCompleted;
    summary.FetchError = fetchError;
    summary.Warning = fetchWarning;
    LOG_INFO("GitHubClient::FetchIssuesStreamed: done pages=%zu total=%zu fullSync=%d err='%s'", pageCount,
             totalEmitted, fullSyncCompleted ? 1 : 0, fetchError.c_str());
    return summary;
}

Result<std::vector<CachedTicket>, TrackerError>
GitHubClient::FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                 const ViewsStore& /*views*/) {
    // Single-issue GET loop per key. Credentials resolve from the live cfg
    // (issue #979); owner/repo aren't consulted because the canonical owner/repo
    // is already embedded in each key (`owner/repo#N`).
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    if (auth.Pat.empty()) {
        return Result<std::vector<CachedTicket>, TrackerError>::Err(TrackerErrorAuth(kPatMissingError));
    }
    return smatchet::github::FetchIssuesForKeysViaRestApi(auth.BaseUrl, auth.Pat, issueKeys);
}

Result<TrackerFieldCatalogResult, TrackerError> GitHubClient::FetchFieldCatalog(const TrackerConfig& /*cfg*/,
                                                                                const std::string& /*projectKey*/) {
    // 6 native fields. Static — no per-project enumeration like Jira's
    // create-meta or Plane's custom fields.
    TrackerFieldCatalogResult outCatalog;
    auto addField = [&outCatalog](const char* id, const char* label, const char* type) {
        TrackerField f;
        f.Id = id;
        f.Name = label;
        f.Type = type;
        outCatalog.Fields.push_back(f);
    };
    // Field IDs must match what GitHubIssueSearchMapping writes into
    // CachedTicket::fieldValues (summary/description/status/assignee/labels/
    // author/created/updated) — otherwise the Views UI fields-picker shows
    // entries that have no underlying data, and Jira-shaped column IDs
    // (which the data IS keyed by) are absent from the picker. The kFooLabel
    // strings remain GitHub-native display names.
    addField("summary", kTitleLabel, "string");
    addField("description", kBodyLabel, "string");
    addField("status", kStateLabel, "string");
    addField("assignee", kAssigneesLabel, "string");
    addField("labels", kLabelsLabel, "array");
    addField("author", "Author", "string");
    addField("created", "Created", "datetime");
    addField("updated", "Updated", "datetime");
    addField("milestone", kMilestoneLabel, "string");
    // issue-comments PR-A — read-only comment count. Unprefixed `comments`
    // (plural) by design — GitHub's catalog uses unprefixed ids for common
    // fields and there's no collision with Jira's singular `comment` blob.
    // Non-editable in editmeta below (the UI intercepts the cell).
    addField("comments", "Comments", "number");
    // PR-only fields. Strings ("true"/"false"/"computing"/"" for
    // mergeable; "true"/"false"/"" for draft) consistent with the existing
    // bool-as-string pattern used by other tracker catalogs.
    addField("pr.head", kPrHeadLabel, "string");
    addField("pr.base", kPrBaseLabel, "string");
    addField("pr.mergeable", kPrMergeableLabel, "string");
    addField("pr.draft", kPrDraftLabel, "string");
    // github-commit-tracker-rows — row-kind discriminator + commit-only columns.
    // All read-only (commits are immutable history); they appear in the field
    // picker so commit rows render their git metadata in the grid.
    addField("github.kind", "Kind", "string");
    addField("commit.sha", "Commit SHA", "string");
    addField("commit.short_sha", "Commit Short SHA", "string");
    addField("commit.author_name", "Commit Author", "string");
    addField("commit.author_email", "Commit Author Email", "string");
    addField("commit.committer_name", "Commit Committer", "string");
    addField("commit.committer_email", "Commit Committer Email", "string");
    addField("commit.url", "Commit URL", "string");
    addField("commit.parents", "Commit Parents", "string");
    addField("commit.verified", "Commit Verified", "string");
    return Result<TrackerFieldCatalogResult, TrackerError>::Ok(std::move(outCatalog));
}

Result<std::unordered_map<std::string, bool>, TrackerError>
GitHubClient::FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& issueKeyOrId) {
    // GitHub has no per-issue editmeta endpoint — the 6 native fields are uniformly
    // editable when the PAT has repo write scope. Return success with all-true so
    // AppController caches the result and doesn't refetch every UI frame (the
    // default `ITrackerIssueReader::FetchIssueEditMeta` returns false + an "unsupported"
    // error, which AppController treats as a transient failure and retries forever).
    // github-commit-tracker-rows — commit rows are immutable git history. Return
    // success with every field non-editable so the grid never offers an edit
    // affordance and offline replay never enqueues against a commit key.
    std::unordered_map<std::string, bool> outFieldIdCanEdit;
    {
        smatchet::github::ParsedCommitKey ck;
        if (smatchet::github::ParseGitHubCommitKey(issueKeyOrId, ck)) {
            // Every catalog field id maps to false so the grid offers no edit
            // affordance on any column of a commit row (CR #504).
            const char* const kCommitReadOnlyFields[] = {"summary",
                                                         "description",
                                                         "status",
                                                         "assignee",
                                                         "labels",
                                                         "milestone",
                                                         "comments",
                                                         "author",
                                                         "created",
                                                         "updated",
                                                         "github.kind",
                                                         "commit.sha",
                                                         "commit.short_sha",
                                                         "commit.author_name",
                                                         "commit.author_email",
                                                         "commit.committer_name",
                                                         "commit.committer_email",
                                                         "commit.url",
                                                         "commit.parents",
                                                         "commit.verified"};
            for (const char* id : kCommitReadOnlyFields) {
                outFieldIdCanEdit[id] = false;
            }
            return Result<std::unordered_map<std::string, bool>, TrackerError>::Ok(std::move(outFieldIdCanEdit));
        }
    }
    // Field IDs aligned with the catalog (summary/description/status/assignee).
    outFieldIdCanEdit["summary"] = true;
    outFieldIdCanEdit["description"] = true;
    outFieldIdCanEdit["status"] = true;
    outFieldIdCanEdit["assignee"] = true;
    outFieldIdCanEdit["labels"] = true;
    outFieldIdCanEdit["milestone"] = true;
    outFieldIdCanEdit["author"] = false; // immutable on GitHub
    outFieldIdCanEdit["created"] = false;
    outFieldIdCanEdit["updated"] = false;
    // issue-comments PR-A — comment count is read-only; the UI intercepts the
    // cell, but editmeta must agree so the grid never offers an edit affordance.
    outFieldIdCanEdit["comments"] = false;
    return Result<std::unordered_map<std::string, bool>, TrackerError>::Ok(std::move(outFieldIdCanEdit));
}

Result<std::vector<TrackerIssueComment>, TrackerError>
GitHubClient::FetchIssueComments(const std::string& issueKey) {
    using CommentsResult = Result<std::vector<TrackerIssueComment>, TrackerError>;
    // No cfg parameter on this interface — resolve from the settled on-disk config
    // (issue #979; same per-request pattern UpdateField / CreateIssue use).
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(nullptr);
    if (auth.Pat.empty()) {
        return CommentsResult::Err(TrackerErrorAuth(kPatMissingError));
    }
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueKey, parsed)) {
        return CommentsResult::Err(TrackerErrorInvalidRequest(
            "GitHubClient::FetchIssueComments: invalid issueKey shape (expected owner/repo#N): " + issueKey));
    }
    const cpr::Header headers = BuildGitHubHeaders(auth.Pat);
    const std::string baseSuffix =
        smatchet::github::BuildIssuePatchUrlSuffix(parsed.Owner, parsed.Repo, parsed.Number) + "/comments";

    constexpr int kPerPage = 100;
    constexpr int kMaxPages = 50; // safety ceiling — 5000 comments is well past any real issue.
    std::vector<TrackerIssueComment> all;
    for (int page = 1; page <= kMaxPages; ++page) {
        std::ostringstream urlOut;
        urlOut << auth.BaseUrl << baseSuffix << "?per_page=" << kPerPage << "&page=" << page;
        const std::string url = urlOut.str();
        const cpr::Response resp =
            TrackerGetLogged("GitHubClient", url, headers, kTrackerConnectTimeoutMs, kTrackerOverallTimeoutMs);
        if (resp.status_code != 200) {
            const std::string msg =
                smatchet::github::ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
            LOG_ERROR("GitHubClient::FetchIssueComments: HTTP %ld on page %d for %s — %s", resp.status_code, page,
                      issueKey.c_str(), msg.c_str());
            // A 2xx-non-200 (201/202/204) reaches this failure branch; for a 2xx
            // TrackerErrorFromHttpStatus returns Ok() (Kind==None, detail discarded).
            // Carry the verbatim detail under an explicit non-OK kind (DR20).
            // SMATCHET_DEVIATION(rule=duplication; reason=the 2xx-non-200 guard idiom (LOG_ERROR + `if 2xx return TrackerErrorUnknown(detail,status)` else FromHttpStatus) is deliberately uniform across the tracker read paths (mirrors GitHubIssueSearch + GitHubClient::CreateIssue + JiraUserAndMeta) so the swallow-as-Ok bug is fixed identically everywhere; extracting a helper would need a Result-type-generic wrapper spanning independent client TUs; owner=deep-review; revisit=2026-10-01)
            if (resp.status_code >= 200 && resp.status_code < 300) {
                return CommentsResult::Err(TrackerErrorUnknown(msg, static_cast<int>(resp.status_code)));
            }
            return CommentsResult::Err(TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), msg));
        }
        // Bounded parse of the untrusted HTTP body (discarded on failure) — audit: unbounded-recursion-DoS.
        const nlohmann::json parsedJson = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
        if (parsedJson.is_discarded() || !parsedJson.is_array()) {
            LOG_ERROR("GitHubClient::FetchIssueComments: invalid JSON / not an array on page %d for %s", page,
                      issueKey.c_str());
            return CommentsResult::Err(TrackerErrorParse("GitHub issue-comments response was not a JSON array."));
        }
        const std::size_t pageCount = parsedJson.size();
        std::vector<TrackerIssueComment> mapped = smatchet::github::MapGitHubIssueComments(parsedJson);
        all.insert(all.end(), std::make_move_iterator(mapped.begin()), std::make_move_iterator(mapped.end()));
        // Short page (< per_page) means the last page was reached — stop paging.
        if (pageCount < static_cast<std::size_t>(kPerPage)) {
            break;
        }
    }
    LOG_INFO("GitHubClient::FetchIssueComments: %s → %zu comments", issueKey.c_str(), all.size());
    return CommentsResult::Ok(std::move(all));
}

TrackerError GitHubClient::AddIssueCommentPlain(const TrackerConfig& cfg, const std::string& issueKey,
                                                const std::string& plainText) {
    // cfg-carrying mutation — resolve credentials from the live cfg (matches
    // CreateIssue / UpdateField). Direct-post: comments are exempt from the
    // offline-queue + audit-trail wiring (mirrors the existing Jira comment post).
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(&cfg);
    if (auth.Pat.empty()) {
        return TrackerErrorAuth(kPatMissingError);
    }
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueKey, parsed)) {
        return TrackerErrorInvalidRequest(
            "GitHubClient::AddIssueCommentPlain: invalid issueKey shape (expected owner/repo#N): " + issueKey);
    }
    const std::string url = auth.BaseUrl +
                            smatchet::github::BuildIssuePatchUrlSuffix(parsed.Owner, parsed.Repo, parsed.Number) +
                            "/comments";
    nlohmann::json body = nlohmann::json::object();
    body["body"] = plainText;
    const cpr::Response resp = TrackerPostLogged("GitHubClient", url, BuildGitHubHeaders(auth.Pat), body.dump());
    if (resp.status_code != 201 && resp.status_code != 200) {
        const std::string msg =
            smatchet::github::ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
        LOG_ERROR("GitHubClient::AddIssueCommentPlain: HTTP %ld for %s — %s", resp.status_code, issueKey.c_str(),
                  msg.c_str());
        return TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), msg);
    }
    return TrackerError::Ok();
}

std::string GitHubClient::BuildBrowseUrl(const TrackerConfig& cfg, const std::string& issueKey) const {
    // Resolve the browse host once (api.github.com → github.com, or strip the
    // Enterprise `/api/v3` suffix). Shared by issue/PR and commit keys.
    // Issue #979 — base URL resolves from the live cfg like every other request.
    const std::string resolvedBaseUrl = ResolveAuth(&cfg).BaseUrl;
    auto browseHost = [&resolvedBaseUrl]() -> std::string {
        std::string host = resolvedBaseUrl;
        const std::string apiSuffix = "/api/v3";
        if (host.size() > apiSuffix.size() &&
            host.compare(host.size() - apiSuffix.size(), apiSuffix.size(), apiSuffix) == 0) {
            host.resize(host.size() - apiSuffix.size());
        } else if (host == "https://api.github.com") {
            host = "https://github.com";
        }
        return host;
    };

    // github-commit-tracker-rows — commit keys (`owner/repo@sha`) route to the
    // /commit/<sha> path. Checked first because the issue-key parser rejects the
    // `@` shape anyway.
    smatchet::github::ParsedCommitKey commitKey;
    if (smatchet::github::ParseGitHubCommitKey(issueKey, commitKey)) {
        return browseHost() +
               smatchet::github::BuildCommitBrowseUrlSuffix(commitKey.Owner, commitKey.Repo, commitKey.Sha);
    }

    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueKey, parsed)) {
        return "";
    }
    // https://github.com/<owner>/<repo>/issues/<n>.
    return browseHost() + "/" + parsed.Owner + "/" + parsed.Repo + "/issues/" + std::to_string(parsed.Number);
}

// github-commit-tracker-rows — shared read-only message for commit-key mutations.
static const char* const kCommitReadOnlyError = "GitHub commit rows are immutable history and cannot be edited";

TrackerError GitHubClient::UpdateIssueFields(const std::string& issueId, const nlohmann::json& /*fields*/) {
    smatchet::github::ParsedCommitKey ck;
    if (smatchet::github::ParseGitHubCommitKey(issueId, ck)) {
        return TrackerErrorInvalidRequest(kCommitReadOnlyError);
    }
    std::string outError;
    StubError(outError, "UpdateIssueFields");
    return TrackerErrorInvalidRequest(outError);
}

TrackerError GitHubClient::UpdateField(const std::string& issueId, const TrackerField& field,
                                       const std::vector<std::string>& values) {
    // github-commit-tracker-rows — reject commit keys before issue-key parsing;
    // the defensive last line behind editmeta's all-false map.
    smatchet::github::ParsedCommitKey commitKey;
    if (smatchet::github::ParseGitHubCommitKey(issueId, commitKey)) {
        return TrackerErrorInvalidRequest(kCommitReadOnlyError);
    }
    smatchet::github::ParsedIssueKey parsed;
    if (!smatchet::github::ParseGitHubIssueKey(issueId, parsed)) {
        return TrackerErrorInvalidRequest("GitHubClient::UpdateField: invalid issueId shape (expected owner/repo#N): " +
                                          issueId);
    }
    // No cfg parameter on this interface — resolve from the settled on-disk config
    // (issue #979; same per-request pattern JiraIssueMutation uses).
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(nullptr);
    if (auth.Pat.empty()) {
        return TrackerErrorAuth(kPatMissingError);
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
    std::string outError;
    StubError(outError, "UpdateField");
    return TrackerErrorInvalidRequest(outError);
}

Result<nlohmann::json, TrackerError> GitHubClient::BuildFieldPayload(const TrackerField& field,
                                                                     const std::vector<std::string>& values) {
    if (field.Id == "labels" || field.Id == "assignees") {
        return Result<nlohmann::json, TrackerError>::Ok(nlohmann::json(values)); // array of strings
    }
    if (field.Id == "state" || field.Id == "milestone" || field.Id == "title" || field.Id == "body") {
        return Result<nlohmann::json, TrackerError>::Ok(
            nlohmann::json(values.empty() ? std::string() : values.front()));
    }
    return Result<nlohmann::json, TrackerError>::Err(
        TrackerErrorInvalidRequest(std::string("GitHubClient::BuildFieldPayload: unknown field '") + field.Id + "'"));
}

Result<nlohmann::json, TrackerError> GitHubClient::BuildCreatePayload(const IssueDraft& draft,
                                                                      const std::vector<TrackerField>& /*catalog*/) {
    // catalog ignored — GitHub's create surface is title/body/labels/assignees,
    // not a configurable customfield schema. Resolve target repo from ProjectKey
    // ("owner/repo") and delegate the body shape to the cpr-free pure helper.
    const std::string& projectKey = draft.ProjectKey;
    const std::size_t slash = projectKey.find('/');
    if (slash == std::string::npos || slash == 0 || slash + 1 >= projectKey.size()) {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(
            "GitHub create requires ProjectKey shaped 'owner/repo' (got '" + projectKey + "')"));
    }
    const std::string owner = projectKey.substr(0, slash);
    const std::string repo = projectKey.substr(slash + 1);

    auto fieldOr = [&draft](const char* key) -> std::string {
        const auto it = draft.FieldValues.find(key);
        return it == draft.FieldValues.end() ? std::string() : it->second;
    };

    auto payloadResult = smatchet::github::BuildGitHubCreatePayload(
        fieldOr("summary"), fieldOr("description"), fieldOr("labels"), fieldOr("assignees"), owner, repo);
    if (!payloadResult) {
        return Result<nlohmann::json, TrackerError>::Err(TrackerErrorInvalidRequest(payloadResult.error()));
    }
    return Result<nlohmann::json, TrackerError>::Ok(std::move(payloadResult.value()));
}

std::string GitHubClient::ResolveDisplayValue(const std::string& fieldId, const TrackerField* /*field*/,
                                              const std::string& value) const {
    // GitHub's enum-like fields (state, milestone) carry display-ready strings
    // server-side, so identity is correct for the 6-field catalog.
    (void)fieldId;
    return value;
}

Result<std::string, TrackerError> GitHubClient::CreateIssue(const nlohmann::json& fields) {
    std::string outError;
    const std::string auditOp = BackendAuditTrail::MakeOperationId("issue-create");
    BackendAuditTrail::AppendBegin("issue_create", "github_client", std::string(), auditOp,
                                   nlohmann::json{{"diff", BackendAuditTrail::MakeFieldDiffUnknownBefore(fields)}});

    // No cfg parameter on this interface — resolve from the settled on-disk config
    // (issue #979; same per-request pattern JiraIssueMutation uses).
    const smatchet::github::GitHubRequestAuth auth = ResolveAuth(nullptr);
    if (auth.Pat.empty()) {
        outError = kPatMissingError;
        BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
        return Result<std::string, TrackerError>::Err(TrackerErrorAuth(outError));
    }
    if (!fields.is_object() || !fields.contains("__target")) {
        outError = "GitHubClient::CreateIssue: payload missing __target (call BuildCreatePayload first)";
        BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
        return Result<std::string, TrackerError>::Err(TrackerErrorInvalidRequest(outError));
    }

    // Extract + strip the out-of-band target before POSTing the body.
    nlohmann::json body = fields;
    const nlohmann::json target = body["__target"];
    body.erase("__target");
    const std::string owner = target.value("owner", std::string());
    const std::string repo = target.value("repo", std::string());
    if (owner.empty() || repo.empty()) {
        outError = "GitHubClient::CreateIssue: __target missing owner/repo";
        BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
        return Result<std::string, TrackerError>::Err(TrackerErrorInvalidRequest(outError));
    }

    const std::string url = auth.BaseUrl + "/repos/" + owner + "/" + repo + "/issues";
    const cpr::Response resp = TrackerPostLogged("GitHubClient", url, BuildGitHubHeaders(auth.Pat), body.dump());

    if (resp.status_code != 201 && resp.status_code != 200) {
        outError = smatchet::github::ExtractGitHubErrorMessage(static_cast<int>(resp.status_code), resp.text);
        LOG_ERROR("GitHubClient::CreateIssue: HTTP %ld — %s", resp.status_code, outError.c_str());
        BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
        // 2xx-but-not-200/201 reaches this failure branch; guard before FromHttpStatus (FIX-1 / Slice-2).
        if (resp.status_code >= 200 && resp.status_code < 300) {
            return Result<std::string, TrackerError>::Err(
                TrackerErrorUnknown(outError, static_cast<int>(resp.status_code)));
        }
        return Result<std::string, TrackerError>::Err(
            TrackerErrorFromHttpStatus(static_cast<int>(resp.status_code), outError));
    }

    try {
        // Bounded parse — the try can't catch a depth-bomb's destructor-time SIGSEGV
        // (audit: unbounded-recursion-DoS). Discarded on failure → value() below hits the catch.
        const nlohmann::json j = smatchet::json_safe::ParseBoundedOrDiscarded(resp.text);
        const std::int64_t number = j.value("number", static_cast<std::int64_t>(0));
        if (number <= 0) {
            outError = "GitHubClient::CreateIssue: response missing issue 'number'";
            LOG_ERROR("GitHubClient::CreateIssue: %s", outError.c_str());
            BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
            return Result<std::string, TrackerError>::Err(TrackerErrorParse(outError));
        }
        const std::string key = smatchet::github::FormatGitHubIssueKey(owner, repo, number);
        LOG_INFO("GitHubClient: created GitHub issue %s.", key.c_str());
        BackendAuditTrail::AppendResult("issue_create", "github_client", key, auditOp, true, std::string());
        return Result<std::string, TrackerError>::Ok(key);
    } catch (const std::exception& ex) {
        outError = std::string("GitHubClient::CreateIssue: failed to parse response: ") + ex.what();
        LOG_ERROR("GitHubClient::CreateIssue: %s", outError.c_str());
        BackendAuditTrail::AppendResult("issue_create", "github_client", std::string(), auditOp, false, outError);
        return Result<std::string, TrackerError>::Err(TrackerErrorParse(outError));
    }
}

std::string GitHubClient::ExtractProjectFromQuery(const std::string& query) const {
    // GitHub backend's "project" anchor is `owner/repo` — extracted from the
    // query string if formatted as such (e.g. user typed `owner/repo` in the
    // tracker query field). Delegates to the pure, unit-tested anchor extractor,
    // which anchors on the leading `owner/repo` token by STRUCTURE so a stray '/'
    // inside a value (e.g. a `ui/ux` label) is never mistaken for a repo anchor.
    return smatchet::github::ExtractGitHubProjectAnchor(query);
}

std::vector<RemoteProject> GitHubClient::ListProjects() {
    // Deferred — `GET /user/repos` paginated. Empty for now.
    return {};
}
