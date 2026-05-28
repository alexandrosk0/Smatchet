#ifndef SMATCHET_LABEL_EDIT_DIFF_PURE_H
#define SMATCHET_LABEL_EDIT_DIFF_PURE_H

#include <string>
#include <vector>

// LabelEditDiffPure — pure set-diff helper for GitHub-style additive label
// APIs. Slice 1 of docs/design/archive/github-tracker-backend.md PR2.
//
// `ITrackerIssueMutations::UpdateField` is set-replace semantically (per CONTEXT.md
// glossary): callers pass the intended full set after edit. GitHub's native
// label API is additive (POST .../labels for add, DELETE for remove), so
// `GitHubClient::UpdateField("labels", ...)` reconciles internally by
// pre-fetching the current set + computing the diff against the intended set
// + issuing per-element POST/DELETE.
//
// This pure helper holds the diff logic so it can be exhaustively tested
// without HTTP. Case-sensitive (GitHub labels are case-sensitive); order-
// independent; tolerant of duplicates in either input (treated as set).

namespace smatchet {
namespace github {

struct LabelEditDiff {
    std::vector<std::string> ToAdd;    // present in intended, absent from current
    std::vector<std::string> ToRemove; // present in current, absent from intended
};

/// Compute the additive diff to take `current` to `intended`.
/// Output vectors are sorted ascending for deterministic test pinning.
/// Duplicates in either input are de-duped before diffing.
LabelEditDiff ComputeLabelEditDiff(const std::vector<std::string>& current, const std::vector<std::string>& intended);

} // namespace github
} // namespace smatchet

#endif // SMATCHET_LABEL_EDIT_DIFF_PURE_H
