#ifndef SMATCHET_TESTS_FAKE_SYNC_CACHE_H
#define SMATCHET_TESTS_FAKE_SYNC_CACHE_H

// FakeSyncCache — header-only in-memory ISyncCache for the doctest rig (ADR-0020, plan
// ilocalcache-seam). Replaces the real `:memory:` SQLite cache inside the deps fakes so
// the service tests are SQLite-free at the construction/direct-include level.
//
// FIDELITY CONTRACT: this fake is implemented TO tests/Core/SyncCacheContract.test.cpp (the
// executable spec, authored against the real LocalCacheManager first and run against BOTH
// impls). Behaviors replicated from the real cache, suite-pinned:
//   * tickets namespaced per backend key; GetAllTickets order UNSPECIFIED (set semantics);
//   * pending queues FIFO by enqueue id; restore appends a NEW (higher) id like AUTOINCREMENT;
//   * per-row backend key captured at enqueue (the #1081 latch), preserved through
//     archive -> dead-letter -> restore;
//   * dead-letter loads newest-first (archived_at DESC, dead_id DESC);
//   * ArchivePending*: last_error becomes terminalError when non-empty, else keeps the row's;
//   * RestoreDeadPendingCreate applies the SAME fresh-create scrub as production
//     (IssueDraftHelpers::ScrubFreshCreatePayload — shared helper, no semantic fork;
//     unparseable payloads restore verbatim); attempts reset to 0;
//   * ResolveFieldEditConflict clears the conflict flag/context AND both merge bases,
//     leaves HasOriginalValue untouched (mirrors the production UPDATE column list);
//   * meta flags are idempotent set-membership.
// Mutex on every method: LocalCacheManager is callable from streaming-sync worker threads
// (TicketSyncService tests join real std::threads), so the fake must be too.

#include "CachedTicketTypes.h"
#include "ISyncCache.h"
#include "IssueDraft.h" // IssueDraftHelpers::ScrubFreshCreatePayload (shared with production restore)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace smatchet_tests {

class FakeSyncCache : public ISyncCache {
  public:
    // --- Tickets -----------------------------------------------------------------------
    void SaveTicket(const std::string& backendKey, const CachedTicket& ticket) override {
        std::lock_guard<std::mutex> lk(m_);
        tickets_[backendKey][ticket.id] = ticket;
    }
    void SaveTickets(const std::string& backendKey, const std::vector<CachedTicket>& batch) override {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < batch.size(); ++i) {
            tickets_[backendKey][batch[i].id] = batch[i];
        }
    }
    bool TryGetTicket(const std::string& backendKey, const std::string& ticketId, CachedTicket& out) override {
        std::lock_guard<std::mutex> lk(m_);
        const auto ns = tickets_.find(backendKey);
        if (ns == tickets_.end()) {
            return false;
        }
        const auto it = ns->second.find(ticketId);
        if (it == ns->second.end()) {
            return false;
        }
        out = it->second;
        return true;
    }
    void DeleteTicket(const std::string& backendKey, const std::string& ticketId) override {
        std::lock_guard<std::mutex> lk(m_);
        const auto ns = tickets_.find(backendKey);
        if (ns != tickets_.end()) {
            ns->second.erase(ticketId);
        }
    }
    std::vector<CachedTicket> GetAllTickets(const std::string& backendKey) override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<CachedTicket> out;
        const auto ns = tickets_.find(backendKey);
        if (ns != tickets_.end()) {
            out.reserve(ns->second.size());
            for (auto it = ns->second.begin(); it != ns->second.end(); ++it) {
                out.push_back(it->second);
            }
        }
        return out;
    }
    std::vector<std::string> GetAllTicketIds(const std::string& backendKey) override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<std::string> out;
        const auto ns = tickets_.find(backendKey);
        if (ns != tickets_.end()) {
            out.reserve(ns->second.size());
            for (auto it = ns->second.begin(); it != ns->second.end(); ++it) {
                out.push_back(it->first);
            }
        }
        return out;
    }

    // --- Pending creates ----------------------------------------------------------------
    std::int64_t EnqueuePendingCreate(const std::string& backendKey, const std::string& payload) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingCreate row;
        row.Id = nextCreateId_++;
        row.Payload = payload;
        row.BackendKey = backendKey;
        row.Attempts = 0;
        row.CreatedAtEpochSec = NowEpochSec();
        creates_.push_back(row);
        return row.Id;
    }
    std::vector<PendingCreate> LoadPendingCreates() override {
        std::lock_guard<std::mutex> lk(m_);
        return creates_; // append-only + restore-appends-new-id => already FIFO by id ASC
    }
    void UpdatePendingCreate(std::int64_t id, int attempts, const std::string& lastError) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingCreate* row = FindCreate(id);
        if (row) {
            row->Attempts = attempts;
            row->LastError = lastError;
        }
    }
    void DeletePendingCreate(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(m_);
        EraseById(creates_, id);
    }
    void ArchivePendingCreate(std::int64_t id, const std::string& terminalReason,
                              const std::string& terminalError) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingCreate* row = FindCreate(id);
        if (!row) {
            return; // production throws on a missing row; no test depends on that
        }
        DeadPendingCreate dead;
        dead.DeadId = nextDeadCreateId_++;
        dead.OriginalId = row->Id;
        dead.Payload = row->Payload;
        dead.BackendKey = row->BackendKey;
        dead.Attempts = row->Attempts;
        dead.LastError = terminalError.empty() ? row->LastError : terminalError;
        dead.CreatedAtEpochSec = row->CreatedAtEpochSec;
        dead.ArchivedAtEpochSec = NowEpochSec();
        dead.TerminalReason = terminalReason;
        deadCreates_.push_back(dead);
        EraseById(creates_, id);
    }
    void UpdatePendingCreatePayload(std::int64_t id, const std::string& payload) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingCreate* row = FindCreate(id);
        if (row) {
            row->Payload = payload;
        }
    }
    std::vector<DeadPendingCreate> LoadDeadPendingCreates() override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<DeadPendingCreate> out = deadCreates_;
        std::sort(out.begin(), out.end(), [](const DeadPendingCreate& a, const DeadPendingCreate& b) {
            if (a.ArchivedAtEpochSec != b.ArchivedAtEpochSec) {
                return a.ArchivedAtEpochSec > b.ArchivedAtEpochSec;
            }
            return a.DeadId > b.DeadId; // newest-first; dead_id DESC tiebreak
        });
        return out;
    }
    std::size_t GetDeadPendingCreateCount() override {
        std::lock_guard<std::mutex> lk(m_);
        return deadCreates_.size();
    }
    bool RestoreDeadPendingCreate(std::int64_t originalPendingId) override {
        std::lock_guard<std::mutex> lk(m_);
        // Latest dead row for this original id (dead_id DESC LIMIT 1).
        int idx = -1;
        for (int i = static_cast<int>(deadCreates_.size()) - 1; i >= 0; --i) {
            if (deadCreates_[static_cast<size_t>(i)].OriginalId == originalPendingId) {
                if (idx < 0 ||
                    deadCreates_[static_cast<size_t>(i)].DeadId > deadCreates_[static_cast<size_t>(idx)].DeadId) {
                    idx = i;
                }
            }
        }
        if (idx < 0) {
            return false;
        }
        const DeadPendingCreate dead = deadCreates_[static_cast<size_t>(idx)];
        std::string parseErr;
        const std::string payload = IssueDraftHelpers::ScrubFreshCreatePayload(dead.Payload, parseErr);
        PendingCreate row;
        row.Id = nextCreateId_++;
        row.Payload = payload; // scrub helper returns the input verbatim on parse error
        row.BackendKey = dead.BackendKey;
        row.Attempts = 0;
        row.CreatedAtEpochSec = NowEpochSec();
        creates_.push_back(row);
        deadCreates_.erase(deadCreates_.begin() + idx);
        return true;
    }
    void DeleteDeadPendingCreate(std::int64_t deadId) override {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < deadCreates_.size(); ++i) {
            if (deadCreates_[i].DeadId == deadId) {
                deadCreates_.erase(deadCreates_.begin() + i);
                return;
            }
        }
    }

    // --- Pending field edits --------------------------------------------------------------
    // Defaults are IDENTICAL to ISyncCache's + LocalCacheManager's (ADR-0020: default args on
    // virtuals bind by the call-site's STATIC type — concrete-typed test callers use short arity,
    // so divergent/missing defaults here would fork or break the API).
    std::int64_t EnqueuePendingFieldEdit(const std::string& backendKey, const std::string& issueKey,
                                         const std::string& fieldId, const std::string& fieldsPayloadJson,
                                         const std::string& originalRichValue = std::string(),
                                         const std::string& originalValue = std::string(),
                                         bool hasOriginalValue = false) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingFieldEditRecord row;
        row.Id = nextEditId_++;
        row.IssueKey = issueKey;
        row.FieldId = fieldId;
        row.FieldsPayloadJson = fieldsPayloadJson;
        row.BackendKey = backendKey;
        row.OriginalRichValue = originalRichValue;
        row.OriginalValue = originalValue;
        row.HasOriginalValue = hasOriginalValue;
        row.Attempts = 0;
        row.CreatedAtEpochSec = NowEpochSec();
        edits_.push_back(row);
        return row.Id;
    }
    std::vector<PendingFieldEditRecord> LoadPendingFieldEdits() override {
        std::lock_guard<std::mutex> lk(m_);
        return edits_; // FIFO by id ASC, conflicted rows included (no WHERE filter)
    }
    void UpdatePendingFieldEdit(std::int64_t id, int attempts, const std::string& lastError) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingFieldEditRecord* row = FindEdit(id);
        if (row) {
            row->Attempts = attempts;
            row->LastError = lastError;
        }
    }
    void DeletePendingFieldEdit(std::int64_t id) override {
        std::lock_guard<std::mutex> lk(m_);
        EraseById(edits_, id);
    }
    void MarkFieldEditConflict(std::int64_t id, const std::string& contextJson) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingFieldEditRecord* row = FindEdit(id);
        if (row) {
            row->HasMergeConflict = true;
            row->ConflictContextJson = contextJson;
        }
    }
    void ResolveFieldEditConflict(std::int64_t id, const std::string& resolvedPayloadJson) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingFieldEditRecord* row = FindEdit(id);
        if (row) {
            // Mirrors the production UPDATE column list: flag + context + BOTH bases cleared,
            // HasOriginalValue deliberately untouched.
            row->HasMergeConflict = false;
            row->ConflictContextJson.clear();
            row->OriginalRichValue.clear();
            row->OriginalValue.clear();
            row->FieldsPayloadJson = resolvedPayloadJson;
        }
    }
    void ArchivePendingFieldEdit(std::int64_t id, const std::string& terminalReason,
                                 const std::string& terminalError) override {
        std::lock_guard<std::mutex> lk(m_);
        PendingFieldEditRecord* row = FindEdit(id);
        if (!row) {
            return;
        }
        DeadPendingFieldEdit dead;
        dead.DeadId = nextDeadEditId_++;
        dead.OriginalId = row->Id;
        dead.IssueKey = row->IssueKey;
        dead.FieldId = row->FieldId;
        dead.FieldsPayloadJson = row->FieldsPayloadJson;
        dead.BackendKey = row->BackendKey;
        dead.Attempts = row->Attempts;
        dead.LastError = terminalError.empty() ? row->LastError : terminalError;
        dead.CreatedAtEpochSec = row->CreatedAtEpochSec;
        dead.ArchivedAtEpochSec = NowEpochSec();
        dead.TerminalReason = terminalReason;
        deadEdits_.push_back(dead);
        // Restore needs both merge bases + the presence flag; the dead POD doesn't carry
        // them, so park them keyed by dead id (production keeps the columns in the twin table).
        ParkedBases bases;
        bases.Rich = row->OriginalRichValue;
        bases.Scalar = row->OriginalValue;
        bases.HasScalar = row->HasOriginalValue;
        parkedBases_[dead.DeadId] = bases;
        EraseById(edits_, id);
    }
    std::vector<DeadPendingFieldEdit> LoadDeadPendingFieldEdits() override {
        std::lock_guard<std::mutex> lk(m_);
        std::vector<DeadPendingFieldEdit> out = deadEdits_;
        std::sort(out.begin(), out.end(), [](const DeadPendingFieldEdit& a, const DeadPendingFieldEdit& b) {
            if (a.ArchivedAtEpochSec != b.ArchivedAtEpochSec) {
                return a.ArchivedAtEpochSec > b.ArchivedAtEpochSec;
            }
            return a.DeadId > b.DeadId;
        });
        return out;
    }
    bool RestoreDeadPendingFieldEdit(std::int64_t originalPendingId) override {
        std::lock_guard<std::mutex> lk(m_);
        int idx = -1;
        for (int i = static_cast<int>(deadEdits_.size()) - 1; i >= 0; --i) {
            if (deadEdits_[static_cast<size_t>(i)].OriginalId == originalPendingId) {
                if (idx < 0 ||
                    deadEdits_[static_cast<size_t>(i)].DeadId > deadEdits_[static_cast<size_t>(idx)].DeadId) {
                    idx = i;
                }
            }
        }
        if (idx < 0) {
            return false;
        }
        const DeadPendingFieldEdit dead = deadEdits_[static_cast<size_t>(idx)];
        PendingFieldEditRecord row;
        row.Id = nextEditId_++;
        row.IssueKey = dead.IssueKey;
        row.FieldId = dead.FieldId;
        row.FieldsPayloadJson = dead.FieldsPayloadJson;
        row.BackendKey = dead.BackendKey; // original key, never a focused context's
        const auto parked = parkedBases_.find(dead.DeadId);
        if (parked != parkedBases_.end()) {
            row.OriginalRichValue = parked->second.Rich;
            row.OriginalValue = parked->second.Scalar;
            row.HasOriginalValue = parked->second.HasScalar;
            parkedBases_.erase(parked);
        }
        row.Attempts = 0;
        row.CreatedAtEpochSec = NowEpochSec();
        edits_.push_back(row);
        deadEdits_.erase(deadEdits_.begin() + idx);
        return true;
    }
    void DeleteDeadPendingFieldEdit(std::int64_t deadId) override {
        std::lock_guard<std::mutex> lk(m_);
        for (size_t i = 0; i < deadEdits_.size(); ++i) {
            if (deadEdits_[i].DeadId == deadId) {
                parkedBases_.erase(deadId);
                deadEdits_.erase(deadEdits_.begin() + i);
                return;
            }
        }
    }

    // --- Meta flags -------------------------------------------------------------------
    bool HasCacheMetaFlag(const std::string& key) override {
        std::lock_guard<std::mutex> lk(m_);
        return metaFlags_.count(key) != 0;
    }
    void SetCacheMetaFlag(const std::string& key) override {
        std::lock_guard<std::mutex> lk(m_);
        metaFlags_.insert(key);
    }

  private:
    struct ParkedBases {
        std::string Rich;
        std::string Scalar;
        bool HasScalar = false;
    };

    static std::int64_t NowEpochSec() {
        return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
    PendingCreate* FindCreate(std::int64_t id) {
        for (size_t i = 0; i < creates_.size(); ++i) {
            if (creates_[i].Id == id) {
                return &creates_[i];
            }
        }
        return nullptr;
    }
    PendingFieldEditRecord* FindEdit(std::int64_t id) {
        for (size_t i = 0; i < edits_.size(); ++i) {
            if (edits_[i].Id == id) {
                return &edits_[i];
            }
        }
        return nullptr;
    }
    template <typename Row> static void EraseById(std::vector<Row>& rows, std::int64_t id) {
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].Id == id) {
                rows.erase(rows.begin() + i);
                return;
            }
        }
    }

    mutable std::mutex m_;
    std::map<std::string, std::map<std::string, CachedTicket>> tickets_;
    std::vector<PendingCreate> creates_;
    std::vector<PendingFieldEditRecord> edits_;
    std::vector<DeadPendingCreate> deadCreates_;
    std::vector<DeadPendingFieldEdit> deadEdits_;
    std::map<std::int64_t, ParkedBases> parkedBases_;
    std::set<std::string> metaFlags_;
    std::int64_t nextCreateId_ = 1;
    std::int64_t nextEditId_ = 1;
    std::int64_t nextDeadCreateId_ = 1;
    std::int64_t nextDeadEditId_ = 1;
};

} // namespace smatchet_tests

#endif // SMATCHET_TESTS_FAKE_SYNC_CACHE_H
