#include "SmatchetCommentsModalUi.h"

#include "AiChatTimestamp.h"
#include "AppController.h"
#include "ITrackerCollaboration.h"
#include "SmatchetLocalization.h"
#include "Ui/SmatchetCommentsModalGenPure.h"
#include "Ui/SmatchetToast.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

/// Singleton state for the comments read/post modal. Decoupled from the grid cell so the modal
/// survives the originating cell scrolling out of view: the cell triggers `JustOpened`, then the
/// top-level `RenderCommentsModal` owns the lifecycle. Mirrors the long-text modal's file-static
/// state choice (`ActiveLongTextEditorState`).
struct CommentsModalState {
    std::string IssueId;
    std::vector<TrackerIssueComment> Comments;
    bool FetchInFlight = false;
    bool PostInFlight = false;
    std::string Error;
    bool Active = false;
    bool JustOpened = false;
    /// Generation token of the in-flight dispatch. Workers compare against the current Gen on
    /// post-back and discard on mismatch (re-open / issue-switch / a newer re-fetch). Sourced from
    /// the file-static `s_genCounter` (NOT reset with this struct) so it is genuinely monotonic
    /// across opens — see SmatchetCommentsModalGenPure.h (#1713).
    int Gen = 0;

    static constexpr size_t kPostBufferSize = 16 * 1024;
    std::vector<char> PostBuf;
};

CommentsModalState s_CommentsState;

/// Monotonic generation counter, deliberately OUTSIDE CommentsModalState so it survives the
/// `s_CommentsState = CommentsModalState{}` reset on every open/close — the reset is exactly what
/// pinned Gen to 1 and made the stale-guard inert (#1713). Each dispatch takes the next value.
int s_genCounter = 0;

constexpr const char* kCommentsModalPopupId = "IssueCommentsModal";

void CloseCommentsModal() { s_CommentsState = CommentsModalState{}; }

/// Pillar 2 — fetch the comment list on a worker thread, then post the result back to the UI thread.
/// Mirrors the SmatchetGridUiSupport.cpp worker pattern (capture by value; AppController& outlives
/// the app; results hop back via MainThreadDispatcher). Discards the post-back if the user opened a
/// different issue's modal in the meantime (Gen mismatch).
void KickCommentsFetch(AppController& app, const std::string& issueId, int gen) {
    AppController* appPtr = &app;
    const std::string capturedIssueId = issueId;
    app.LaunchBackgroundTask([appPtr, capturedIssueId, gen]() {
        std::vector<TrackerIssueComment> comments;
        std::string err;
        bool ok = false;
        UnpackResult(appPtr->FetchIssueComments(capturedIssueId), ok, comments, err);
        appPtr->PostToMainThread([appPtr, gen, capturedIssueId, ok, comments = std::move(comments), err]() mutable {
            if (SmatchetCommentsModalGen::CallbackIsStale(s_CommentsState.Active, s_CommentsState.Gen, gen,
                                                          s_CommentsState.IssueId, capturedIssueId)) {
                return;
            }
            s_CommentsState.FetchInFlight = false;
            if (ok) {
                s_CommentsState.Comments = std::move(comments);
                s_CommentsState.Error.clear();
                // issue-comments fix (#1291, extended) — runs on every fetch: modal-open AND the
                // post-success re-fetch. Pushes the observed thread into the cached ticket (count +
                // flattened tooltip blob) so the grid Comments cell AND its hover tooltip reflect a
                // just-posted comment without a full re-sync. UI thread (post-back).
                appPtr->UpdateCachedCommentsFromThread(capturedIssueId, s_CommentsState.Comments);
            } else {
                s_CommentsState.Error =
                    err.empty()
                        ? std::string(SmatchetLocalization::T("comments.fetch_failed", "Failed to load comments."))
                        : err;
            }
        });
    });
}

/// Draws the scrollable read-only comment thread. Each comment: author • formatted time • PLAIN-TEXT
/// body (never markdown). Time formatting reuses smatchet::ai::FormatRelativeTime / FormatAbsoluteTime
/// (both take unix-epoch milliseconds; TrackerIssueComment times are seconds → ×1000).
void DrawCommentsThread() {
    if (s_CommentsState.Comments.empty()) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.none", "No comments yet."));
        return;
    }
    const std::int64_t nowMs = smatchet::ai::NowUnixMs();
    for (size_t i = 0; i < s_CommentsState.Comments.size(); ++i) {
        const TrackerIssueComment& c = s_CommentsState.Comments[i];
        ImGui::PushID(static_cast<int>(i));
        // Header line: author (emphasised) • relative time, absolute on hover.
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.85f, 1.0f, 1.0f));
        ImGui::TextUnformatted(c.Author.empty() ? SmatchetLocalization::T("comments.unknown_author", "(unknown)")
                                                : c.Author.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        const std::int64_t thenMs = c.CreatedAtSec * 1000;
        const std::string rel = smatchet::ai::FormatRelativeTime(nowMs, thenMs);
        ImGui::TextDisabled("\xe2\x80\xa2 %s", rel.c_str());
        if (ImGui::IsItemHovered()) {
            const std::string abs = smatchet::ai::FormatAbsoluteTime(thenMs);
            if (!abs.empty()) {
                ImGui::SetTooltip("%s", abs.c_str());
            }
        }
        // Body is plain text only and opaque to the UI per the interface contract — never render
        // markdown or rich formatting. TextWrapped wraps to the child width.
        ImGui::TextWrapped("%s", c.Body.c_str());
        ImGui::Separator();
        ImGui::PopID();
    }
}

/// Draws the post box + Post button. Disabled (with hint) while read-only/offline or a post is
/// in flight. Post → fetch-worker-style call to AddIssueCommentPlain; on success re-fetch the
/// thread + success toast + clear the box; on failure error toast.
void DrawCommentsPostBox(AppController& app, bool readOnlyMode) {
    const bool disabled = readOnlyMode || s_CommentsState.PostInFlight;

    ImGui::Separator();
    if (readOnlyMode) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("comments.disabled_readonly", "(disabled while offline/read-only)"));
    } else if (s_CommentsState.PostInFlight) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.posting", "Posting comment..."));
    }

    if (disabled) {
        ImGui::BeginDisabled();
    }
    ImGui::InputTextMultiline("##CommentsPostBox", s_CommentsState.PostBuf.data(), CommentsModalState::kPostBufferSize,
                              ImVec2(-FLT_MIN, 80.0f));
    const bool hasText = s_CommentsState.PostBuf.data()[0] != '\0';
    if (!hasText) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("comments.post_placeholder", "Write a comment (Markdown supported)..."));
    }
    const bool post =
        ImGui::Button(SmatchetLocalization::T("comments.post_button", "Post Comment"), ImVec2(140, 0)) && hasText;
    if (disabled) {
        ImGui::EndDisabled();
    }

    if (post) {
        const std::string body(s_CommentsState.PostBuf.data());
        const std::string capturedIssueId = s_CommentsState.IssueId;
        const int gen = s_CommentsState.Gen;
        s_CommentsState.PostInFlight = true;
        AppController* appPtr = &app;
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("comments.queued_title", "Comment Queued"),
                                              SmatchetLocalization::T("comments.posting", "Posting comment..."),
                                              ToastType::Info);
        app.LaunchBackgroundTask([appPtr, capturedIssueId, body, gen]() {
            const VoidResult r = appPtr->AddIssueCommentPlain(capturedIssueId, body);
            const bool ok = r.has_value();
            const std::string err = ok ? std::string() : r.error();
            appPtr->PostToMainThread([appPtr, capturedIssueId, gen, ok, err]() {
                if (ok) {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("toast.comment_posted", "Comment Posted"),
                        SmatchetLocalization::T("comments.posted_body", "Comment added."), ToastType::Success);
                } else {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("toast.comment_failed", "Comment Failed"),
                        err.empty()
                            ? std::string(SmatchetLocalization::T("comments.post_failed", "Failed to post comment."))
                            : err,
                        ToastType::Error);
                }
                // Only act on the still-open modal for this issue (Gen guards against a re-open).
                if (SmatchetCommentsModalGen::CallbackIsStale(s_CommentsState.Active, s_CommentsState.Gen, gen,
                                                              s_CommentsState.IssueId, capturedIssueId)) {
                    return;
                }
                s_CommentsState.PostInFlight = false;
                if (ok) {
                    // Clear the post box and re-fetch the thread so the new comment shows. Take a FRESH
                    // generation for the re-fetch so the original-open fetch (older Gen), if it is still
                    // in flight and lands afterward, is discarded instead of overwriting this fresher
                    // list and momentarily dropping the just-posted comment (#1713).
                    s_CommentsState.PostBuf.assign(CommentsModalState::kPostBufferSize, '\0');
                    s_CommentsState.Comments.clear();
                    s_CommentsState.Gen = SmatchetCommentsModalGen::AllocGen(s_genCounter);
                    s_CommentsState.FetchInFlight = true;
                    KickCommentsFetch(*appPtr, capturedIssueId, s_CommentsState.Gen);
                }
            });
        });
    }
}

} // namespace

void OpenCommentsModal(AppController& app, const std::string& issueId, const std::string& prefillBody) {
    s_CommentsState = CommentsModalState{};
    s_CommentsState.IssueId = issueId;
    s_CommentsState.Active = true;
    s_CommentsState.JustOpened = true;
    s_CommentsState.FetchInFlight = true;
    // Take the next monotonic token from the file-static counter (survives the reset above) so this
    // open is distinguishable from a prior open's still-in-flight fetch (#1713).
    s_CommentsState.Gen = SmatchetCommentsModalGen::AllocGen(s_genCounter);
    s_CommentsState.PostBuf.assign(CommentsModalState::kPostBufferSize, '\0');
    if (!prefillBody.empty()) {
        const std::size_t copyLen = (std::min)(prefillBody.size(), CommentsModalState::kPostBufferSize - 1);
        std::memcpy(s_CommentsState.PostBuf.data(), prefillBody.data(), copyLen);
    }
    KickCommentsFetch(app, issueId, s_CommentsState.Gen);
}

void RenderCommentsModal(AppController& app, bool readOnlyMode) {
    if (!s_CommentsState.Active) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (s_CommentsState.JustOpened) {
        // OpenPopup must be called at the same window-depth as BeginPopupModal — both here, outside
        // the table, so ImGui can anchor the popup to this stable top-level window.
        ImGui::OpenPopup(kCommentsModalPopupId);
        const ImVec2 modalSize(viewport->Size.x * 0.5f, viewport->Size.y * 0.6f);
        ImGui::SetNextWindowSize(modalSize, ImGuiCond_Always);
        ImGui::SetNextWindowPos(
            ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f, viewport->Pos.y + viewport->Size.y * 0.5f),
            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        s_CommentsState.JustOpened = false;
    }

    bool modalOpen = true;
    if (ImGui::BeginPopupModal(kCommentsModalPopupId, &modalOpen,
                               ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::Text("%s — %s", SmatchetLocalization::T("comments.title", "Comments"), s_CommentsState.IssueId.c_str());
        ImGui::Separator();

        if (!s_CommentsState.Error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.5f, 1.0f));
            ImGui::TextWrapped("%s", s_CommentsState.Error.c_str());
            ImGui::PopStyleColor();
        }

        // Reserve room for the footer (post box + button + status line).
        const float footerH = 80.0f + ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y * 3.0f;
        const float threadH = ImGui::GetContentRegionAvail().y - footerH;
        ImGui::BeginChild("##CommentsThread", ImVec2(-FLT_MIN, threadH > 0.0f ? threadH : 0.0f), true);
        if (s_CommentsState.FetchInFlight) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.loading", "Loading comments..."));
        } else {
            DrawCommentsThread();
        }
        ImGui::EndChild();

        DrawCommentsPostBox(app, readOnlyMode);

        ImGui::Separator();
        bool wantClose = ImGui::Button("Close", ImVec2(100, 0));
        // P2-M3: Esc while the post box (or any input) is being edited only defocuses it
        // (ImGui's own revert-and-deactivate) — it must not also tear down the modal
        // around the draft. Only a second, unfocused Esc reaches the close path.
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::GetIO().WantTextInput) {
            wantClose = true;
        }
        if (!modalOpen) { // title-bar X — same guard as Close/Esc (the local resets next frame)
            wantClose = true;
        }
        if (wantClose) {
            if (s_CommentsState.PostBuf.data()[0] != '\0') {
                ImGui::OpenPopup("Discard comment?###CommentsDiscardConfirm");
            } else {
                ImGui::CloseCurrentPopup();
                CloseCommentsModal();
            }
        }
        bool discardConfirmed = false;
        if (ImGui::BeginPopupModal("Discard comment?###CommentsDiscardConfirm", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(
                SmatchetLocalization::T("comments.discard_confirm", "Discard the comment you're writing?"));
            if (ImGui::Button("Discard comment")) {
                discardConfirmed = true;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep writing")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        if (discardConfirmed) {
            ImGui::CloseCurrentPopup();
            CloseCommentsModal();
        }

        ImGui::EndPopup();
    } else {
        // Modal dismissed without our intervention (title-bar X) — stay self-consistent.
        CloseCommentsModal();
    }
}
