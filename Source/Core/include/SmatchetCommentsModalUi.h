#pragma once

#include <cstddef>
#include <string>

class AppController;

/// issue-comments PR-A — backend-agnostic "Comments" read/post modal.
/// Its popup lifecycle (top-level OpenPopup, Close / Esc / X, the discard-comment confirm) is owned by
/// the generic field-viewer shell (`Ui/SmatchetFieldViewerModalUi.h`); this unit supplies the Comments
/// content. It lives at top-level (mirrors `TicketFieldEditor::RenderLongTextModal`) so the modal
/// survives the originating cell scrolling out of view: the cell calls `OpenCommentsModal` (which kicks
/// an off-UI fetch worker), then the once-per-frame top-level `RenderFieldViewerModal` draws it.
/// Pillar 2 — NO network/file-IO on the UI thread: the comment list is fetched only on modal open
/// via `AppController::FetchIssueCommentsTyped` on a worker; posting routes through
/// `AppController::SubmitOrQueueComment` on a worker (sent now, or saved while offline and replayed on
/// reconnect). The cell render path itself does zero network (the comment count is read from the
/// cached `fieldValues["comments"]`).
/// Pillar 6 (offline-first) — the modal first shows the thread saved with the ticket (the structured
/// `kCommentThreadRichKey` copy, else the tooltip blob summary), skips the network while the tracker
/// is offline, and marks saved data with a DataFreshnessCue; a live fetch replaces it.

/// Captures `issueId`, installs the Comments content in the field-viewer shell, resets the comments
/// state + marks it active, and kicks the load worker with the ticket's saved thread (sets the in-flight
/// flag once the worker is launched). Safe to call from inside a table cell — the actual
/// `OpenPopup`/`BeginPopupModal` happens at top-level depth in `RenderFieldViewerModal`.
/// `prefillBody` (optional) seeds the post box — quick comment templates open here for
/// review/editing instead of posting to the tracker sight-unseen (P2-M2).
/// `focus` says who gets the keyboard: an open the user asked for (a click on the comment cell, a
/// template pick) puts the caret in the post box so they can type at once; an open from code leaves
/// the keyboard where it was.
enum class CommentsOpenFocus : unsigned char { Keep, Composer };
void OpenCommentsModal(AppController& app, const std::string& issueId, const std::string& prefillBody = std::string(),
                       CommentsOpenFocus focus = CommentsOpenFocus::Keep);

// The modal body (drawn by the shell): a freshness cue (with Retry after a failed load), then a
// scrollable read-only thread (each comment: author • formatted time • Markdown body), "Loading
// comments..." only while nothing is saved, this issue's queued comments, a separator, a post box and a
// Post button. Ctrl+Enter in the box posts, the same as the button. Post is disabled while the box is
// blank, `readOnlyMode` is set or a post is already in flight; offline a post is saved to the
// pending-action queue. Esc in the box leaves it with the typed text intact, and closes the viewer
// when nothing was typed. The state drains when the shell closes the viewer.

/// Test hook: a copy of the modal's load state. UI thread only (reads the file-static state the
/// render loop and post-backs mutate).
struct CommentsModalSnapshot {
    bool Active = false;
    bool FetchInFlight = false;
    bool Seeded = false;
    bool SeedPartial = false;
    bool FetchFailed = false;
    std::size_t CommentCount = 0;
    /// This issue's comments in the pending-action queue (waiting or failed), as the modal lists them.
    std::size_t PendingCommentCount = 0;
    std::string FirstAuthor;
    /// The caret is in the post box (as of the last draw).
    bool ComposerActive = false;
    /// The Post button would act on a click (as of the last draw).
    bool PostEnabled = false;
    bool PostInFlight = false;
    /// The text in the post box.
    std::string PostDraft;
    /// The post box was filled from a draft kept by an earlier open, and nothing was typed since.
    bool DraftRestored = false;
};
CommentsModalSnapshot GetCommentsModalSnapshotForTests();
