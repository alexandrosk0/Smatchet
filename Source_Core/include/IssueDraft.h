#ifndef SMATCHET_ISSUE_DRAFT_H
#define SMATCHET_ISSUE_DRAFT_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "LocalCacheManager.h"
#include "TrackerFieldSchema.h"

struct StagedAttachment {
    std::string AbsPath;
    std::string FileName;
    std::uint64_t SizeBytes = 0;
};

/**
 * Work-in-progress new-issue payload. Shared between the end-of-grid draft row,
 * bulk import, and offline-replay. Intentionally stringly-typed so the existing
 * grid cell editors (which write back raw strings) plug in without translation.
 */
struct IssueDraft {
    std::string ProjectKey;
    std::string IssueTypeId;   // numeric Tracker id when known
    std::string IssueTypeName; // display name (used as fallback when id unknown)
    std::string ParentKey;     // PROJ-123 for subtask parent; empty otherwise
    /** When set (e.g. bulk import "key" column), pipeline updates this issue instead of creating. */
    std::string ExistingIssueKey;
    std::unordered_map<std::string, std::string> FieldValues;
    std::vector<StagedAttachment> StagedAttachments;
};

struct RequiredFieldSet {
    std::unordered_set<std::string> FieldIds;
    bool IsSubtask = false;
    bool RequiresIssueType = true;
};

namespace IssueDraftHelpers {

/** Default Tracker field ids copied from the last row into a new-issue draft (config fallback). */
std::vector<std::string> DefaultNewIssueInheritFieldIds();

/**
 * Fields that should NOT be inherited when cloning the last row into a new draft
 * (server-managed or meaningless on create).
 */
bool IsCreateSuppressedFieldId(const std::string& fieldId);

/** Project-scoped fields the user can always set (project, issuetype, parent). */
bool IsSpecialDraftFieldId(const std::string& fieldId);

/**
 * Build an IssueDraft from `ticket`. Copies only fields listed in
 * `inheritFieldIds` (or, if that vector is empty, `DefaultNewIssueInheritFieldIds()`).
 * Summary is never copied. `project`, `issuetype`, and `parent` may appear in the
 * list: they populate `FieldValues` for the grid and gate whether project key,
 * issue type, and parent key are taken from the last row (when `inheritFieldIds`
 * is empty, legacy behavior keeps copying those three from the row even though
 * they are not in the default id list).
 * When `ticket.id` is empty (no last row), returns a draft seeded with the
 * fallback project key and issue type only.
 */
IssueDraft FromCachedTicket(const CachedTicket& ticket, const std::vector<TrackerField>& catalog,
                            const std::string& fallbackProjectKey, const std::string& fallbackIssueTypeId,
                            const std::string& fallbackIssueTypeName, const std::vector<std::string>& inheritFieldIds);

/**
 * Resolve the Tracker issue type id for a ticket using the catalog's allowed
 * options (CachedTicket stores the display name).
 */
std::string ResolveIssueTypeIdFromTicket(const CachedTicket& ticket, const std::vector<TrackerField>& catalog,
                                         std::string* outName = nullptr);

/**
 * Returns ids of required fields that are empty / missing in the draft.
 * Always includes the hard minimum (projectKey / issueTypeId / summary) when
 * they are empty, even if `required` is empty.
 */
std::vector<std::string> MissingRequiredFields(const IssueDraft& draft, const RequiredFieldSet& required);

/** JSON (de)serialization for the offline queue + JSON bulk import. */
std::string ToJson(const IssueDraft& draft);
bool FromJson(const std::string& json, IssueDraft& outDraft, std::string& outError);

/** Per-field diff between a draft and an existing cached ticket (for update preview). */
struct FieldChange {
    std::string FieldId;
    std::string OldValue; // value from `existing`
    std::string NewValue; // value from `draft`
};

/**
 * Compare draft against `existing`. Returns only fields whose new value differs
 * (trimmed, case-sensitive) from the cached value. Includes "parent" and
 * "issuetype" when they were set on the draft and differ. Empty draft values
 * that match empty cached values are not reported.
 */
std::vector<FieldChange> ComputeFieldChanges(const IssueDraft& draft, const CachedTicket& existing);

/**
 * Strip draft fields whose value matches `existing` (see ComputeFieldChanges).
 * Keeps `ProjectKey` / `IssueTypeId` / `IssueTypeName` untouched (used for
 * routing), but clears `ParentKey` when it matches and clears matching entries
 * from `FieldValues`. Attachments are never pruned.
 * Fields absent from `existing.fieldValues` compare against an empty baseline (implicit add / not loaded).
 */
void PruneUnchangedFields(IssueDraft& draft, const CachedTicket& existing);

/**
 * Map internal field markers (__project__, etc.) to user-friendly names
 * and look up other field names in the catalog.
 */
std::vector<std::string> MapFieldIdsToNames(const std::vector<std::string>& ids,
                                           const std::vector<TrackerField>& catalog);

} // namespace IssueDraftHelpers

#endif







