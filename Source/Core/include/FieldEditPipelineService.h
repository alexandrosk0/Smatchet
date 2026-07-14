#pragma once

// FieldEditPipelineService — owns the field-edit NETWORK pipeline extracted verbatim from
// `AppController` per the AppController god-object decomposition plan (Phase 2), mirroring the
// Phase-1 EditMetaCacheService extraction. The service holds an `IFieldEditDeps&` (typically
// backed by `GridContextDepsAdapter`) for the AppController-side state it reaches (backend handle,
// cache predicate, active-tickets snapshot, optimistic-update + refresh + deferred-notify hooks),
// and an `EditMetaCacheService&` DIRECTLY (ctor-injected, NOT via deps) for the editmeta
// ensure/can-edit/refresh checks the regular + network-only branches perform.
// AppController's public surface keeps the same shape but its field-edit bodies are thin delegators
// that forward into this service. The grid layer (SmatchetGridFieldEditPipeline.cpp) also calls
// FieldEditSupportsOfflineQueue / SubmitFieldEditNetworkOnly / TryPrepareOfflineFieldEdit /
// ApplyFieldEditResult through those delegators.
// Concurrency: the service holds no long-lived mutex. The SubmitFieldEditCtx is a call-frame-scoped
// POD of references built on the stack inside SubmitFieldEdit; all mutation happens on the calling
// (UI) thread inside one call frame.
// Lifetime contract mirrors EditMetaCacheService: AppController owns the service via
// `std::unique_ptr` and outlives it; the `IFieldEditDeps&` (GridContextDepsAdapter) and the
// `EditMetaCacheService&` both outlive this service (declared after it, destroyed after it).

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "CachedTicketTypes.h"    // CachedTicket (SubmitFieldEditCtx ticketsSnap)
#include "SmatchetResult.h"       // VoidResult (SubmitFieldEdit* branch helpers)
#include "Types/FieldEditTypes.h" // FieldEditResult

class IFieldEditDeps;
class EditMetaCacheService;
class ITrackerBackend;
class ITrackerIssueMutations;
struct TrackerField;

class FieldEditPipelineService {
  public:
    FieldEditPipelineService(IFieldEditDeps& deps, EditMetaCacheService& editMeta);

    /** Safe field families for offline-queued field edits (transport failures only). */
    static bool FieldEditSupportsOfflineQueue(const TrackerField& field);

    /// Online field-edit entry point (used by the Lua binding + legacy callers). Submits to the
    /// backend and applies an optimistic local-cache update on success. Branches sprint /
    /// timetracking / regular. Signature preserved verbatim — the Lua forwarder depends on it.
    bool SubmitFieldEdit(const std::string& issueId, const TrackerField& field,
                         const std::vector<std::string>& rawValues, std::string& outError);

    /// Network-only field edit (no local-cache write) — the grid pipeline calls this on a worker,
    /// then applies the result on the UI thread via ApplyFieldEditResult.
    bool SubmitFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult);

    /**
     * Build the Jira fields payload + optimistic display map without calling the network.
     * Used when a network save failed with a transport error and the edit should be queued offline
     * (the DB write itself is grid-layer: AppController::QueueFieldEditOffline).
     */
    bool TryPrepareOfflineFieldEdit(const std::string& issueId, const TrackerField& field,
                                    const std::vector<std::string>& rawValues,
                                    const std::string& originalEstimateSnapshot,
                                    const std::string& remainingEstimateSnapshot,
                                    const std::string& issueTypeKeySnapshot, FieldEditResult& outResult,
                                    std::string& outFieldsPayloadJson, std::string& outError);

    /// Apply a successful FieldEditResult (from SubmitFieldEditNetworkOnly or an offline replay) to
    /// the local cache + grid model. UI thread.
    VoidResult ApplyFieldEditResult(const std::string& issueId, const FieldEditResult& result);

  private:
    /// Shared context for the three SubmitFieldEdit branch helpers. Holds references only —
    /// lifetime is bounded to the SubmitFieldEdit call frame that builds the ctx on the stack.
    struct SubmitFieldEditCtx {
        const std::string& issueId;
        const TrackerField& field;
        const std::vector<std::string>& rawValues; ///< original, unfiltered
        const std::vector<std::string>& values;    ///< filtered (non-empty entries only)
        ITrackerIssueMutations* mutations;
        const std::shared_ptr<ITrackerBackend>& backend;
        const std::shared_ptr<const std::vector<CachedTicket>>& ticketsSnap;
        const std::string& fieldEditAuditOp;
        const char* fieldEditAuditSource;
    };

    /// Sprint-field branch of SubmitFieldEdit (AddIssueToSprint + local-cache sync).
    VoidResult SubmitFieldEditSprint(const SubmitFieldEditCtx& ctx);
    /// Editable timetracking estimate branch of SubmitFieldEdit (UpdateIssueFields timetracking wrapper).
    VoidResult SubmitFieldEditTimetracking(const SubmitFieldEditCtx& ctx);
    /// Regular field branch of SubmitFieldEdit (editmeta check + UpdateIssueFields + 400-retry).
    VoidResult SubmitFieldEditRegular(const SubmitFieldEditCtx& ctx);

    /// Build the network field payload (editmeta-gated, backend BuildFieldPayload) + optimistic
    /// display map. Shared by SubmitFieldEditNetworkOnly + TryPrepareOfflineFieldEdit.
    bool TryBuildFieldEditPayloadForNetwork(const std::string& issueId, const TrackerField& field,
                                            const std::vector<std::string>& rawValues,
                                            const std::string& originalEstimateSnapshot,
                                            const std::string& remainingEstimateSnapshot,
                                            const std::string& issueTypeKeySnapshot, nlohmann::json& outFieldsPayload,
                                            std::unordered_map<std::string, std::string>& outDisplayValues,
                                            std::string& outError);

    /// SubmitFieldEditNetworkOnly helper — push the built payload, retrying once after a 400 with
    /// a refreshed editmeta + edit-permission re-check. Returns true on a successful update.
    bool ApplyFieldUpdateWithEditMetaRetry(const std::string& issueId, const TrackerField& field,
                                           const nlohmann::json& fieldsPayload, const std::string* issueTypeKeyOpt,
                                           ITrackerIssueMutations& mutations, FieldEditResult& outResult);

    /// SubmitFieldEditNetworkOnly helper — apply a sprint-field edit (add-to-sprint mutation +
    /// optimistic display value). `handled` is set true when the field is a sprint field.
    bool SubmitSprintFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                          const std::vector<std::string>& values, ITrackerIssueMutations& mutations,
                                          FieldEditResult& outResult, bool& handled);

    /// SubmitFieldEditNetworkOnly helper — apply a Jira timetracking-estimate edit. `handled` is
    /// set true when the field is an editable timetracking estimate.
    bool SubmitTimetrackingFieldEditNetworkOnly(const std::string& issueId, const TrackerField& field,
                                                const std::vector<std::string>& values,
                                                const std::string& originalEstimateSnapshot,
                                                const std::string& remainingEstimateSnapshot,
                                                ITrackerIssueMutations& mutations, FieldEditResult& outResult,
                                                bool& handled);

    IFieldEditDeps& deps_;
    EditMetaCacheService& editMeta_;
};
