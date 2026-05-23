#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
#include "LocalCacheManager.h" // For CachedTicket struct
#include "TrackerFieldSchema.h"

struct TrackerConfig;
struct ViewsStore;
struct IssueDraft;
struct RequiredFieldSet;

struct TrackerIssueFetchSummary {
    size_t FetchedCount = 0;
    bool FullSyncCompleted = false;
    // Hard failure: sync did not complete usefully. Drives the failure banner / error toast.
    std::string FetchError;
    // Soft warning: sync did produce useful data but with a caveat (e.g. pagination cap reached).
    // Distinct from FetchError so the UI can show "Sync Warning" without suppressing the success
    // notification and without flipping connectivity state to TransportDown.
    std::string Warning;
};

enum class TrackerReachabilityProbeKind {
    AuthenticatedReachable,
    ReachableAuthOrConfigError,
    TransportDown,
    ServiceUnavailable,
};

struct TrackerReachabilityProbeResult {
    TrackerReachabilityProbeKind Kind = TrackerReachabilityProbeKind::TransportDown;
    std::string Diagnostic;
};

/**
 * One backend comment surfaced to the agentic-flow triage half. Backend-agnostic shape so
 * GitHub issue comments, Jira ADF comments, and Plane comments can land in a single
 * AgentProposal.context bucket. Always opaque to UI — UI never renders rich formatting from
 * `Body`; it's plain text for prompt-builder consumption.
 *
 * `Id` is `std::string` to fit GitHub's int64 issue-comment id and Jira's stringly-typed
 * `1234`-style id without a discriminator field. Time fields are unix epoch seconds because
 * the consumers (poll cursor in `agent_poll_cursor`, prompt-builder sort) need integer
 * comparison, not formatted strings.
 */
struct TrackerIssueComment {
    std::string Id;                // backend-stable comment id
    std::string Author;            // username / handle (GitHub user.login, Jira author.displayName)
    std::string Body;              // raw comment body (markdown for GitHub; plain-text for Jira/Plane)
    std::int64_t CreatedAtSec = 0; // unix epoch seconds
    std::int64_t UpdatedAtSec = 0; // unix epoch seconds (== CreatedAtSec if never edited)
};

class ITrackerClient {
  public:
    virtual ~ITrackerClient() = default;
    virtual std::string GetTrackerType() const = 0;

    /**
     * Periodic connectivity check.
     */
    virtual TrackerReachabilityProbeResult ProbeReachability(const TrackerConfig& cfg) = 0;

    /**
     * Pull issues for the current JQL / tracker query.
     * @param outFullSyncCompleted If non-null, set true only when the backend finished pagination
     *        without an aborted page (safe to drop local rows not present in the returned set).
     * @param configOverride If non-null, use instead of ConfigManager::Load() (JQL, credentials, project).
     * @param viewsOverride If non-null, use instead of reloading views from disk (active view fields).
     */
    /**
     * @param outFetchError If non-null, set to a short diagnostic when the sync did not complete
     *        cleanly (e.g. first-page HTTP non-200). Empty on full success. Used for transport vs
     *        hard-failure UX (cached grid).
     * @param outWarning If non-null, set to a soft caveat (e.g. pagination cap reached) when the
     *        sync produced useful data but with a partial-coverage warning. Empty on clean completion.
     */
    virtual std::vector<CachedTicket> FetchIssues(bool* outFullSyncCompleted = nullptr,
                                                  const TrackerConfig* configOverride = nullptr,
                                                  const ViewsStore* viewsOverride = nullptr,
                                                  std::string* outFetchError = nullptr,
                                                  std::string* outWarning = nullptr) = 0;

    using BatchCallback = std::function<void(std::vector<CachedTicket>&&)>;
    using CancelCallback = std::function<bool()>;

    virtual TrackerIssueFetchSummary FetchIssuesStreamed(const BatchCallback& onBatch,
                                                         const CancelCallback& shouldCancel,
                                                         const TrackerConfig* configOverride = nullptr,
                                                         const ViewsStore* viewsOverride = nullptr) {

        TrackerIssueFetchSummary summary;
        std::string fetchError;
        bool fullSyncCompleted = false;
        std::vector<CachedTicket> tickets = FetchIssues(&fullSyncCompleted, configOverride, viewsOverride, &fetchError);
        summary.FetchedCount = tickets.size();
        summary.FullSyncCompleted = fullSyncCompleted;
        summary.FetchError = fetchError;
        if (!tickets.empty() && onBatch && (!shouldCancel || !shouldCancel())) {
            onBatch(std::move(tickets));
        }
        return summary;
    }

    /**
     * Fetch a specific set of issues by their keys.
     */
    virtual bool FetchIssuesForKeys(const TrackerConfig& cfg, const std::vector<std::string>& issueKeys,
                                    const ViewsStore& views, std::vector<CachedTicket>& outTickets,
                                    std::string& outError) = 0;

    // Fetch fields/options for the active tracker. Default impl is unsupported.
    // PR 6: `projectKey` is the per-operation project for create-meta enrichment (Jira project
    // key, or Plane project UUID). Empty means unscoped — backend returns the global field list.
    // Pre-PR-6 callers used to read this from `cfg.ProjectKey` / `cfg.PlaneProjectId`; those
    // fields are gone and the project is now passed explicitly.
    virtual bool FetchFieldCatalog(const TrackerConfig& /*cfg*/, const std::string& /*projectKey*/,
                                   TrackerFieldCatalogResult& /*outCatalog*/, std::string& outError) {
        outError = "FetchFieldCatalog is not supported by this backend.";
        return false;
    }

    // Build a web URL for an issue if the backend supports one.
    virtual std::string BuildBrowseUrl(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/) const {
        return {};
    }

    // Update one or more tracker field values on an issue.
    // `fields` is the backend-specific object payload under the update root.
    virtual bool UpdateIssueFields(const std::string& issueId, const nlohmann::json& fields, std::string& outError) = 0;

    virtual bool UpdateField(const std::string& issueId, const TrackerField& field,
                             const std::vector<std::string>& values, std::string& outError) = 0;

    virtual bool BuildFieldPayload(const TrackerField& field, const std::vector<std::string>& values,
                                   nlohmann::json& outPayload, std::string& outError) = 0;

    virtual bool BuildCreatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildCreatePayload is not supported by this backend.";
        return false;
    }

    virtual bool BuildUpdatePayload(const IssueDraft& /*draft*/, const std::vector<TrackerField>& /*catalog*/,
                                    nlohmann::json& /*outPayload*/, std::string& outError) {
        outError = "BuildUpdatePayload is not supported by this backend.";
        return false;
    }

    virtual std::string ResolveDisplayValue(const std::string& fieldId, const TrackerField* field,
                                            const std::string& value) const = 0;

    /**
     * Create a new issue and return its key (e.g. "PROJ-42") on success.
     * Returns an empty string + sets `outError` on failure.
     * Default impl errors out for backends without create support.
     */
    virtual std::string CreateIssue(const nlohmann::json& /*fields*/, std::string& outError) {
        outError = "CreateIssue is not supported by this backend.";
        return {};
    }

    /**
     * Attach one or more local files to an existing issue.
     * Per-file errors are reported via `outFailures` (path -> message) and do
     * not abort the batch. Default impl errors out for backends without support.
     */
    virtual bool AttachFilesToIssue(const std::string& /*issueKey*/, const std::vector<std::string>& /*absolutePaths*/,
                                    std::vector<std::pair<std::string, std::string>>& /*outFailures*/,
                                    std::string& outError) {
        outError = "AttachFilesToIssue is not supported by this backend.";
        return false;
    }

    /**
     * Add an existing issue to a sprint (Jira Agile). Default impl is unsupported.
     */
    virtual bool AddIssueToSprint(const std::string& /*issueKey*/, const std::string& /*sprintId*/,
                                  std::string& outError) {
        outError = "AddIssueToSprint is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueEditMeta(const TrackerConfig& /*cfg*/, const std::string& /*issueKeyOrId*/,
                                    std::unordered_map<std::string, bool>& /*outFieldIdCanEdit*/,
                                    std::string& outError) {
        outError = "FetchIssueEditMeta is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueWatchers(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                    std::vector<TrackerUser>& /*outWatchers*/, std::string& outError) {
        outError = "FetchIssueWatchers is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueWatcher(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/, std::string& outError) {
        outError = "AddIssueWatcher is not supported by this backend.";
        return false;
    }

    virtual bool FetchIssueVotes(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                 std::vector<TrackerUser>& /*outVoters*/, std::string& outError,
                                 int* /*outVoteCount*/ = nullptr, bool* /*outHasVoted*/ = nullptr,
                                 bool* /*outVotersInResponse*/ = nullptr) {
        outError = "FetchIssueVotes is not supported by this backend.";
        return false;
    }

    virtual bool SearchUsersByQuery(const TrackerConfig& /*cfg*/, const std::string& /*query*/,
                                    std::vector<TrackerUser>& /*outUsers*/, std::string& outError) {
        outError = "SearchUsersByQuery is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentPlain(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                      const std::string& /*plainText*/, std::string& outError) {
        outError = "AddIssueCommentPlain is not supported by this backend.";
        return false;
    }

    /**
     * Read all comments on a single issue. Caller-stable ordering — implementations either
     * return newest-first or oldest-first; the agentic-flow prompt-builder sorts on
     * `CreatedAtSec`. Backends without comment support (or with the feature disabled) return
     * `false` and set `outError` to the documented sentinel.
     *
     * `issueKey` shape is backend-specific. GitHub uses `owner/repo#N` (see
     * `GitHubClientHelpers::ParseGitHubIssueKey`). Jira / Plane use their native key forms.
     */
    virtual bool FetchIssueComments(const std::string& /*issueKey*/, std::vector<TrackerIssueComment>& /*outComments*/,
                                    std::string& outError) {
        outError = "FetchIssueComments is not supported by this backend.";
        return false;
    }

    virtual bool AddWorklog(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                            const std::string& /*timeSpent*/, const std::string& /*timeRemaining*/,
                            const std::string& /*adjustEstimate*/, const std::string& /*workDescription*/,
                            const std::string& /*startedDate*/, std::string& outError) {
        outError = "AddWorklog is not supported by this backend.";
        return false;
    }

    virtual bool AddIssueCommentBlameContext(const TrackerConfig& /*cfg*/, const std::string& /*issueKey*/,
                                             const std::string& /*p4User*/, const std::string& /*functionName*/,
                                             const std::string& /*filePath*/, int /*lineNumber*/,
                                             const std::string& /*changelist*/, const std::string& /*date*/,
                                             bool /*approximated*/, const std::string& /*codeSnippet*/,
                                             std::string& outError) {
        outError = "AddIssueCommentBlameContext is not supported by this backend.";
        return false;
    }

    virtual bool FetchUserGroupNames(const TrackerConfig& /*cfg*/, const std::string& /*accountId*/,
                                     std::vector<std::string>& /*outGroupNames*/, std::string& outError) {
        outError = "FetchUserGroupNames is not supported by this backend.";
        return false;
    }

    // Best-effort extract of a single project from a backend-specific query.
    // Returns "" when no project clause is present OR when multiple projects are referenced
    // (sentinel for the "ambiguous" case — callers must surface a picker).
    virtual std::string ExtractProjectFromQuery(const std::string& /*query*/) const { return ""; }

    // List projects visible to the current credentials. Default empty;
    // real impls land in PR 4 of the remove-global-project-key rollout.
    virtual std::vector<RemoteProject> ListProjects() { return {}; }
};
