#pragma once
#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>

/** Max replay attempts for offline `pending_creates` before archive or drop. */
namespace OfflineCreateQueue {
constexpr int kMaxReplayAttempts = 5;
}

/** Max replay attempts for offline `pending_field_edits` before dead-letter archive. */
namespace OfflineFieldEditQueue {
constexpr int kMaxReplayAttempts = 5;
}

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

class LocalCacheManager {
  public:
    LocalCacheManager(const std::string& dbPath);

    void SaveTicket(const CachedTicket& ticket);
    /** @return false if `ticketId` is not present in `tickets`. */
    bool TryGetTicket(const std::string& ticketId, CachedTicket& out);
    void DeleteTicket(const std::string& ticketId);
    std::vector<CachedTicket> GetAllTickets();
    std::vector<std::string> GetAllTicketIds();

    /** @return generated row id. */
    std::int64_t EnqueuePendingCreate(const std::string& payload);
    std::vector<PendingCreate> LoadPendingCreates();
    void UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError);
    void DeletePendingCreate(std::int64_t id);
    void ArchivePendingCreate(std::int64_t id, const std::string& terminalReason, const std::string& terminalError);
    std::vector<DeadPendingCreate> LoadDeadPendingCreates();
    size_t GetDeadPendingCreateCount();

    /**
     * One-time: delete legacy `pending_creates` rows already at retry cap (pre dead-letter).
     * Persists a meta flag per database file.
     */
    size_t RunOneTimeLegacyDropPendingAtMaxAttempts();

    /** Restore latest dead-letter row for this original pending id back to active queue (attempts=0). */
    bool RestoreDeadPendingCreate(std::int64_t originalPendingId);

    /** Permanently remove a dead-letter row (user discard). */
    void DeleteDeadPendingCreate(std::int64_t deadId);

    std::int64_t EnqueuePendingFieldEdit(const std::string& issueKey, const std::string& fieldId,
                                         const std::string& fieldsPayloadJson);
    std::vector<PendingFieldEditRecord> LoadPendingFieldEdits();
    void UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError);
    void DeletePendingFieldEdit(std::int64_t id);
    void ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason, const std::string& terminalError);
    std::vector<DeadPendingFieldEdit> LoadDeadPendingFieldEdits();
    void DeleteDeadPendingFieldEdit(std::int64_t deadId);

  private:
    SQLite::Database db;
};






