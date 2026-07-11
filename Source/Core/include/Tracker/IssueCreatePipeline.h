#ifndef SMATCHET_ISSUE_CREATE_PIPELINE_H
#define SMATCHET_ISSUE_CREATE_PIPELINE_H

#include "IssueDraft.h"
#include "TrackerFieldSchema.h"

// Fan-in Phase 2 (docs/plans/appcontroller-fan-in.md): json_fwd, not json.hpp — this header names
// nlohmann::json only as `nlohmann::json& outFields` in the BuildFieldsPayload decl (L43, by-ref).
// It is reached by AppController.h's includers (AppController.h includes IssueCreatePipeline.h), so
// the full-json door is closed here; the .cpp that defines BuildFieldsPayload includes json.hpp.
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

class ITrackerIssueMutations;
class ISyncCache;

/**
 * Outcome of a single IssueCreatePipeline::Run call.
 */
struct IssueCreateResult {
    bool Ok = false;
    std::string IssueKey;                                                // populated on success
    std::string Error;                                                   // single-line summary
    /// Transport-shaped Error (retryable per TrackerError::IsRetryable), classified where the
    /// pipeline flattens the backend's TrackerError (N12 item 13b). Validation/payload-build
    /// failures keep the default false — never offline-queueable. Deliberately NOT set for the
    /// "created, key unknown" shape (the create succeeded server-side; queueing would duplicate).
    bool ErrorTransient = false;
    std::vector<std::string> MissingFieldIds;                            // populated when validation failed
    std::vector<std::pair<std::string, std::string>> AttachmentFailures; // path -> reason
    /** On create: new row. On update: merged ticket written to SQLite when cache is non-null. */
    CachedTicket SeededTicket;
};

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
