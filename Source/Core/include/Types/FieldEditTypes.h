#ifndef SMATCHET_TYPES_FIELD_EDIT_TYPES_H
#define SMATCHET_TYPES_FIELD_EDIT_TYPES_H

// Types/FieldEditTypes.h — FieldEditResult relocated out of AppController.h (fan-in
// Phase 3, docs/plans/appcontroller-fan-in.md). Was nested as AppController::FieldEditResult;
// moved to the global namespace here (rank-0 leaf) and re-exported via
// `using FieldEditResult = ::FieldEditResult;` inside AppController, so the ~115 includers
// that name AppController::FieldEditResult see an identical type with zero source change.
// Field-edit is an actively-developed surface, so isolating this struct's edits from the
// full includer recompile is the highest-value Phase-3 relocation.

#include <string>
#include <unordered_map>

struct FieldEditResult {
    bool Ok = false;
    std::string Error;
    /// Transport-shaped Error (retryable per TrackerError::IsRetryable), classified where the
    /// pipeline flattens the mutation's TrackerError (N12 item 13b). Local-validation errors
    /// keep the default false — they are never offline-queueable.
    bool ErrorTransient = false;
    std::unordered_map<std::string, std::string> UpdatedDisplayValues;
};

#endif // SMATCHET_TYPES_FIELD_EDIT_TYPES_H
