#ifndef SMATCHET_ISSUE_CREATE_PIPELINE_H
#define SMATCHET_ISSUE_CREATE_PIPELINE_H

#include "IssueDraft.h"
#include "TrackerFieldSchema.h"

// Fan-in Phase 2 (docs/plans/appcontroller-fan-in.md): json_fwd, not json.hpp — this header names
// nlohmann::json only as `nlohmann::json& outFields` in the BuildFieldsPayload decl (L43, by-ref).
// It is reached by AppController.h's includers (AppController.h includes IssueCreatePipeline.h), so
// the full-json door is closed here; the .cpp that defines BuildFieldsPayload includes json.hpp.
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

class ITrackerIssueMutations;
class ISyncCache;

/**
 * Outcome of a single IssueCreatePipeline::Run call.
 */
struct IssueCreateResult {
    bool Ok = false;
    std::string IssueKey; // populated on success
    std::string Error;    // single-line summary
    /// The backend's error kind was retryable (TrackerError::IsRetryable: transport, rate limit, 5xx),
    /// classified where the pipeline flattens it (N12 item 13b). Validation / payload-build failures
    /// and the "created, key unknown" shape keep false.
    bool ErrorTransient = false;
    /// Sending the draft again could create the issue twice: the failed create may have been applied
    /// with only its response lost. Cleared only when the tracker provably did not apply it
    /// (TrackerError::ProvablyNotApplied) and for an update, which replays as a set-replace.
    bool ReplayMayDuplicate = true;
    std::vector<std::string> MissingFieldIds;                            // populated when validation failed
    std::vector<std::pair<std::string, std::string>> AttachmentFailures; // path -> reason
    /** On create: new row. On update: merged ticket written to SQLite when cache is non-null. */
    CachedTicket SeededTicket;
    /// > 0 when the tracker could not be reached and the draft was saved to the offline queue instead
    /// (AppController::CreateOrQueueIssueAsync, Quality Pillar 6): the queue row that creates it on reconnect.
    std::int64_t QueuedOfflineId = 0;
};

/// True when a failed create / update may go to the offline queue: the failure is retryable and sending
/// the draft again cannot duplicate the issue. A rejection the user must fix is reported instead.
inline bool IsOfflineQueueableFailure(const IssueCreateResult& result) {
    return !result.Ok && result.ErrorTransient && !result.ReplayMayDuplicate;
}

/// True when a failed create may nonetheless have created the issue (e.g. a timeout after the request
/// went out, or a 5xx). It is neither queued nor resent automatically: the user checks the tracker first.
inline bool IsAmbiguousCreateFailure(const IssueCreateResult& result) {
    return !result.Ok && result.ErrorTransient && result.ReplayMayDuplicate;
}

/// Shown with an ambiguous create failure (IsAmbiguousCreateFailure).
constexpr const char* kAmbiguousCreateHint =
    "The issue may have been created; check the tracker before sending it again.";

/**
 * Reusable create/update flow: validate draft -> build Jira payload -> POST (create) or PUT
 * (when `IssueDraft::ExistingIssueKey` or legacy `FieldValues["key"]` is set) -> attach -> seed cache.
 *
 * Intentionally decoupled from AppController so the same logic can back the
 * single-draft "Create" button, bulk CSV/JSON/TSV import, clipboard paste,
 * and offline-queue replay.
 */
namespace IssueCreatePipeline {

/**
 * Convert a validated draft into a Jira `fields` payload (same shape
 * POST /rest/api/3/issue expects under the "fields" key). Does NOT include
 * sprint assignments - those must be applied after create via AddIssueToSprint.
 */
bool BuildFieldsPayload(const IssueDraft& draft, const std::vector<TrackerField>& catalog, nlohmann::json& outFields,
                        std::string& outError);

/**
 * Seed a CachedTicket from a draft so the grid can display the new row
 * immediately without waiting for the next Jira sync. `issueKey` may be empty
 * for pending-offline rows (we mark them with a synthetic `__pending__` id).
 */
CachedTicket SeedCachedTicketFromDraft(const IssueDraft& draft, const std::vector<TrackerField>& catalog,
                                       const std::string& issueKey);

/**
 * Execute the full flow. `cache` may be null (no seeding). `cacheBackendKey` is the
 * backend-key namespace every cache read/write is scoped to (multi-grid Slice 1b —
 * `ConfigManager::NormalizeViewsBackendKey` output; ignored when `cache` is null).
 * Attachment failures do NOT flip `Ok` to false when the issue was created - they are
 * reported in `AttachmentFailures` so callers can choose to retry or warn.
 */
IssueCreateResult Run(ITrackerIssueMutations& client, ISyncCache* cache, const std::string& cacheBackendKey,
                      const IssueDraft& draft, const RequiredFieldSet& required,
                      const std::vector<TrackerField>& catalog);

} // namespace IssueCreatePipeline

#endif
