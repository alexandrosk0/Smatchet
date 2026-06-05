#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>

// POD types shared between the SQLite-backed cache (`LocalCacheManager`) and callers
// that only consume the cached payload — IssueDraft, IssueCreatePipeline, the offline
// queue services, tracker UI editors, etc. Lifted into their own SQLite-free header so
// downstream test targets that touch the PODs without exercising the cache class can
// skip the SQLiteCpp link. `LocalCacheManager.h` re-includes this file so existing
// users see no API change.

struct CachedTicket {
    std::string id;
    std::unordered_map<std::string, std::string> fieldValues;
    /// Original rich-content payload for fields that round-trip through ADF/HTML — keyed by the
    /// same field id as `fieldValues`. Populated by `JiraIssueSearch` (ADF JSON-stringified) and
    /// `PlaneClient` (description_html). Used by the long-text modal editor to seed Markdown
    /// fidelity and avoid silent format loss when an unedited field is later re-saved. See
    /// RICH_TEXT_EDITING_V2_PLAN.md.
    std::unordered_map<std::string, std::string> fieldRichValues;

    std::string GetFieldValue(const std::string& key) const {
        const auto it = fieldValues.find(key);
        return (it != fieldValues.end()) ? it->second : std::string();
    }

    /// Zero-copy view of a field value (a stable reference to the stored string, or a static
    /// empty string when absent). Use on hot read paths — e.g. the grid sort comparator, which
    /// runs O(n log n) per sort — to avoid the per-call std::string copy GetFieldValue() makes.
    /// The reference is valid until the ticket (or its fieldValues entry) is mutated/destroyed.
    /// (memory-budget-and-lifetime-hardening § Phase 5 pull-forward.)
    const std::string& GetFieldValueRef(const std::string& key) const {
        static const std::string kEmpty;
        const auto it = fieldValues.find(key);
        return (it != fieldValues.end()) ? it->second : kEmpty;
    }

    std::string GetFieldRichValue(const std::string& key) const {
        const auto it = fieldRichValues.find(key);
        return (it != fieldRichValues.end()) ? it->second : std::string();
    }
};

/**
 * Offline-queued issue create. `Payload` is the JSON-serialized IssueDraft
 * (see IssueDraftHelpers::ToJson). `Attempts` is bumped on each replay to
 * enforce a retry cap, and `LastError` shows the most recent failure reason
 * (surfaced in the UI so the user can decide to retry / drop).
 */
struct PendingCreate {
    std::int64_t Id = 0;
    std::string Payload;
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
};

/**
 * Offline-queued issue field update. `FieldsPayloadJson` is the JSON object Jira expects under
 * `fields` in PUT /rest/api/2/issue/{key} (i.e. only the inner field map, not wrapped).
 */
struct PendingFieldEditRecord {
    std::int64_t Id = 0;
    std::string IssueKey;
    std::string FieldId;
    std::string FieldsPayloadJson;
    /// Original rich-content payload (ADF JSON or HTML) at edit-open time. Used by
    /// `TickOfflineFieldEdits` to perform a 3-way merge with the current server content
    /// before replaying (base=this, mine=FieldsPayloadJson, theirs=server). Empty for
    /// non-ADF fields and edits queued before this field was introduced.
    std::string OriginalRichValue;
    /// Original scalar DISPLAY value (CachedTicket::GetFieldValue) at edit-commit time — the
    /// scalar twin of OriginalRichValue. Used by `TickOfflineFieldEdits` to detect a concurrent
    /// server change for non-rich fields (base=this vs theirs=re-fetched display) via
    /// OfflineFieldConflictPolicy::ServerMovedFromBase. Empty for rich fields and for edits
    /// queued before this column existed (legacy rows keep last-write-wins). See ADR-0016.
    std::string OriginalValue;
    /// True when a scalar base was CAPTURED at queue time — independent of whether that base was
    /// blank. Conflict detection keys on this PRESENCE flag (not OriginalValue.empty()) so a
    /// genuinely blank-but-captured base is still conflict-checked. False for rich fields and for
    /// legacy rows queued before this column existed (those keep last-write-wins). See ADR-0016.
    bool HasOriginalValue = false;
    /// True when the offline-replay conflict gate (rich 3-way merge OR scalar 2-way compare OR
    /// an unverifiable re-fetch) suspended this row. The record stays in the queue and is not
    /// retried until the user resolves via the conflict UI.
    bool HasMergeConflict = false;
    /// JSON blob populated when HasMergeConflict is true. Top-level `kind` ∈ {text, scalar,
    /// unverified} selects the conflict category (absent = legacy rich "text"); the rich `text`
    /// shape additionally carries `richKind` ∈ {adf, html} for reconversion. See ADR-0016 /
    /// RICH_TEXT_EDITING_V2_PLAN.md.
    std::string ConflictContextJson;
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
};

struct DeadPendingFieldEdit {
    std::int64_t DeadId = 0;
    std::int64_t OriginalId = 0;
    std::string IssueKey;
    std::string FieldId;
    std::string FieldsPayloadJson;
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
    std::int64_t ArchivedAtEpochSec = 0;
    std::string TerminalReason;
};

struct DeadPendingCreate {
    /** Primary key in `pending_creates_dead` (unique per archived row). */
    std::int64_t DeadId = 0;
    std::int64_t OriginalId = 0;
    std::string Payload;
    int Attempts = 0;
    std::string LastError;
    std::int64_t CreatedAtEpochSec = 0;
    std::int64_t ArchivedAtEpochSec = 0;
    std::string TerminalReason;
};
