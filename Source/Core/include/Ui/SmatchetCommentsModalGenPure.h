#pragma once

#include <string>

/// Pure, unit-testable core of the comments-modal generation-token stale-guard (#1713).
/// Every comments fetch/post dispatch is tagged with a generation token so a slow post-back
/// that lands after the modal moved on (re-opened, switched issue, or a newer re-fetch) is
/// discarded instead of overwriting fresher results. The token MUST be genuinely monotonic
/// across dispatches: the original bug reset a per-open counter to 0 and pre-incremented it,
/// so every open produced Gen==1 and the guard could never discriminate a stale original-open
/// fetch from the fresh post-triggered re-fetch — the stale one won the race and momentarily
/// dropped a just-posted comment. Keeping the counter OUT of the per-open state reset fixes it.
namespace SmatchetCommentsModalGen {

/// Allocate the next monotonic generation token. `counter` lives outside the per-open modal
/// state struct so it survives the struct reset — each dispatch (open OR post-refetch) gets a
/// strictly-greater, distinct token.
inline int AllocGen(int& counter) { return ++counter; }

/// A fetch/post post-back is stale (must be dropped) if the modal is closed, its generation no
/// longer matches the dispatch that started it, or the user switched to a different issue.
/// Mirrors the two live guard sites in SmatchetCommentsModalUi.cpp.
inline bool CallbackIsStale(bool active, int currentGen, int dispatchGen, const std::string& currentIssueId,
                            const std::string& dispatchIssueId) {
    return !active || currentGen != dispatchGen || currentIssueId != dispatchIssueId;
}

/// A post result that is stale for the open showing it must still clear the box when that open restored
/// the comment being posted as a draft (#2459): close and reopen the same issue while the post runs, and
/// the stored draft is the posted text. Clear only when the post went through (sent or queued), the open
/// is on the same draft scope, the box is still marked restored, and it holds exactly the posted body, so
/// text typed since stays.
inline bool PostedBodyRestoredAsDraft(bool postSucceeded, bool active, const std::string& currentDraftScope,
                                      const std::string& postDraftScope, bool draftRestored, const std::string& boxText,
                                      const std::string& postedBody) {
    return postSucceeded && active && !postDraftScope.empty() && currentDraftScope == postDraftScope && draftRestored &&
           boxText == postedBody;
}

} // namespace SmatchetCommentsModalGen
