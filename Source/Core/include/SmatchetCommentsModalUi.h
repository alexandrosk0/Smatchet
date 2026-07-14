#pragma once

#include <string>

class AppController;

/// issue-comments PR-A — backend-agnostic "Comments" read/post modal.
/// Lives at top-level (mirrors `TicketFieldEditor::RenderLongTextModal`) so the modal survives the
/// originating cell scrolling out of view: the cell calls `OpenCommentsModal` (which kicks an
/// off-UI fetch worker), then the once-per-frame top-level `RenderCommentsModal` owns the lifecycle.
/// Pillar 2 — NO network/file-IO on the UI thread: the comment list is fetched only on modal open
/// via `AppController::FetchIssueComments` on a worker; posting routes through
/// `AppController::AddIssueCommentPlain` on a worker. The cell render path itself does zero network
/// (the comment count is read from the cached `fieldValues["comments"]`).

/// Captures `issueId`, resets modal state, marks it active + just-opened, and kicks the fetch
/// worker (sets the in-flight flag). Safe to call from inside a table cell — the actual
/// `OpenPopup`/`BeginPopupModal` happens at top-level depth in `RenderCommentsModal`.
/// `prefillBody` (optional) seeds the post box — quick comment templates open here for
/// review/editing instead of posting to the tracker sight-unseen (P2-M2).
void OpenCommentsModal(AppController& app, const std::string& issueId, const std::string& prefillBody = std::string());

/// Renders the comments modal once per frame from a stable top-level location. On just-opened runs
/// `OpenPopup`. Body: in-flight spinner while fetching, then a scrollable read-only thread (each
/// comment: author • formatted time • PLAIN-TEXT body — never markdown), a separator, a post box,
/// and a Post button. Post box + button are disabled when `readOnlyMode` (offline / read-only) or a
/// post is already in flight. Drains its own state on close.
void RenderCommentsModal(AppController& app, bool readOnlyMode);
