#include "SmatchetCommentsModalUi.h"

// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "AiChatTimestamp.h"
#include "AppController.h"
#include "CachedTicketTypes.h"
#include "DataFreshnessCue.h"
#include "ITrackerCollaboration.h"
#include "Logger.h"
#include "MarkdownPreviewRender.h"
#include "OfflineFirstPure.h"
#include "PendingActionPolicyPure.h"
#include "PendingActionTypes.h"
#include "ScopeExit.h"
#include "SmatchetLocalization.h"
#include "Tracker/CommentBlobFormatPure.h"
#include "Ui/SmatchetCommentsModalGenPure.h"
#include "Ui/SmatchetCommentsModalSeedPure.h"
#include "Ui/SmatchetToast.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// A comment on this issue that sits in the pending-action queue instead of on the tracker.
struct PendingCommentView {
    std::int64_t QueueId = 0;
    std::string Body;
    bool Failed = false; ///< in the failed list: it will not be retried automatically
    MarkdownPreviewRender::PreviewPlanPtr Plan;
};

/// Singleton state for the comments read/post modal. Decoupled from the grid cell so the modal
/// survives the originating cell scrolling out of view: the cell triggers `JustOpened`, then the
/// top-level `RenderCommentsModal` owns the lifecycle. Mirrors the long-text modal's file-static
/// state choice (`ActiveLongTextEditorState`).
struct CommentsModalState {
    std::string IssueId;
    std::vector<TrackerIssueComment> Comments;
    /// Parsed Markdown per comment body, index-aligned with `Comments`; rebuilt when the sizes
    /// diverge or the font size changes (a plan caches word widths), so the thread is not
    /// re-parsed every frame the modal is open.
    std::vector<MarkdownPreviewRender::PreviewPlanPtr> BodyPlans;
    float BodyPlansFontSize = 0.0f;
    /// Last drawn height of each comment (header + body + separator, incl. trailing item
    /// spacing), or <= 0 when not yet measured at `BodyHeightsWidth`. Off-screen comments with a
    /// known height draw as a spacer instead of re-emitting every word each frame (Pillar 1).
    std::vector<float> BodyHeights;
    float BodyHeightsWidth = 0.0f;
    bool FetchInFlight = false;
    bool PostInFlight = false;
    /// `Comments` came from the copy saved with the ticket, not from a live fetch in this open.
    bool Seeded = false;
    /// The saved copy is the tooltip summary (latest 20 comments, dates to the day).
    bool SeedPartial = false;
    /// The last load failed, or skipped the network because the tracker is offline.
    bool FetchFailed = false;
    TrackerErrorKind FetchErrorKind = TrackerErrorKind::None;
    /// Detail of the last failed load, shown as the freshness cue's tooltip.
    std::string Error;
    bool Active = false;
    bool JustOpened = false;
    /// Generation token of the in-flight dispatch. Workers compare against the current Gen on
    /// post-back and discard on mismatch (re-open / issue-switch / a newer re-fetch). Sourced from
    /// the file-static `s_genCounter` (NOT reset with this struct) so it is genuinely monotonic
    /// across opens — see SmatchetCommentsModalGenPure.h (#1713).
    int Gen = 0;
    /// Generation of this open, fixed until the modal closes. A post is guarded by it rather than by
    /// Gen: a reload (Retry) bumps Gen, and must not orphan a post that is still in flight.
    int OpenGen = 0;

    /// This issue's queued comments (Pillar 6), rebuilt only when the published queue snapshot
    /// changes (a pointer compare per frame); their Markdown plans are built for PendingPlansFontSize.
    std::shared_ptr<const PendingActionsSnapshot> PendingSource;
    std::vector<PendingCommentView> PendingComments;
    float PendingPlansFontSize = 0.0f;

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

/// Where one comments load starts: the copies saved with the ticket (parsed on the worker) and
/// whether to ask the tracker even while the last probe says it is unreachable (an explicit Retry).
struct CommentsLoadRequest {
    std::string IssueId;
    int Gen = 0;
    std::string SeedThread;
    std::string SeedBlob;
    bool ForceNetwork = false;
};

/// One post-back from the load worker. `Seed` shows the saved copy while the load continues;
/// `Loaded` and `Failed` end the load and release FetchInFlight.
struct CommentsLoadUpdate {
    enum class Kind : unsigned char { Seed, Loaded, Failed };
    Kind What = Kind::Failed;
    std::vector<TrackerIssueComment> Comments;
    bool SeedPartial = false;
    /// Failed: a request actually went out (false when it was skipped offline).
    bool Attempted = false;
    TrackerErrorKind ErrorKind = TrackerErrorKind::None;
    std::string Error;
};

void ResetCommentBodyLayout() {
    s_CommentsState.BodyPlans.clear();
    s_CommentsState.BodyHeights.clear();
}

void ApplyCommentsLoadUpdate(AppController& app, const std::string& issueId, CommentsLoadUpdate& update) {
    CommentsModalState& st = s_CommentsState;
    switch (update.What) {
    case CommentsLoadUpdate::Kind::Seed:
        // A saved copy never replaces comments a live fetch already delivered.
        if (st.Seeded || st.Comments.empty()) {
            st.Comments = std::move(update.Comments);
            st.Seeded = true;
            st.SeedPartial = update.SeedPartial;
            ResetCommentBodyLayout();
        }
        return;
    case CommentsLoadUpdate::Kind::Loaded:
        st.FetchInFlight = false;
        st.Comments = std::move(update.Comments);
        st.Seeded = false;
        st.SeedPartial = false;
        st.FetchFailed = false;
        st.FetchErrorKind = TrackerErrorKind::None;
        st.Error.clear();
        ResetCommentBodyLayout();
        // issue-comments fix (#1291, extended) — runs on every fetch: modal-open AND the
        // post-success re-fetch. Pushes the observed thread into the cached ticket (count, tooltip
        // blob and the structured thread shown offline) so the grid Comments cell and its tooltip
        // reflect a just-posted comment without a full re-sync. UI thread (post-back).
        app.UpdateCachedCommentsFromThread(issueId, st.Comments);
        return;
    case CommentsLoadUpdate::Kind::Failed:
        st.FetchInFlight = false;
        st.FetchFailed = true;
        st.FetchErrorKind = update.ErrorKind;
        st.Error = update.Error;
        // A live request failing at the transport level means connectivity just changed: probe now
        // rather than waiting out the interval (the probe schedule is UI-thread state).
        if (update.Attempted && update.ErrorKind == TrackerErrorKind::Transport) {
            app.RequestTrackerProbeNow();
        }
        return;
    }
}

void PostCommentsLoadUpdate(AppController* appPtr, int gen, const std::string& issueId, CommentsLoadUpdate update) {
    appPtr->PostToMainThread([appPtr, gen, issueId, update = std::move(update)]() mutable {
        if (SmatchetCommentsModalGen::CallbackIsStale(s_CommentsState.Active, s_CommentsState.Gen, gen,
                                                      s_CommentsState.IssueId, issueId)) {
            return;
        }
        ApplyCommentsLoadUpdate(*appPtr, issueId, update);
    });
}

/// Worker body. Pillar 2: the saved copy is parsed here, never on the UI thread. Pillar 6: the saved
/// copy is posted first; offline the network is skipped (a request would only spend the retry
/// window), otherwise the live list replaces it. Every exit path, a throw included, posts the final
/// update so the modal can never stay on "Loading".
void RunCommentsLoad(AppController* appPtr, const CommentsLoadRequest& req) {
    bool finalPosted = false;
    smatchet::ScopeExit endLoad([appPtr, &req, &finalPosted]() {
        if (finalPosted) {
            return;
        }
        try {
            CommentsLoadUpdate failed;
            failed.ErrorKind = TrackerErrorKind::Unknown;
            PostCommentsLoadUpdate(appPtr, req.Gen, req.IssueId, std::move(failed));
        } catch (const std::exception& ex) {
            LOG_ERROR("CommentsModal: could not end the comments load for %s: %s", req.IssueId.c_str(), ex.what());
        }
    });
    CommentsLoadUpdate seed;
    if (SmatchetCommentsModalSeed::PickCommentsSeed(req.SeedThread, req.SeedBlob, seed.Comments, seed.SeedPartial)) {
        seed.What = CommentsLoadUpdate::Kind::Seed;
        PostCommentsLoadUpdate(appPtr, req.Gen, req.IssueId, std::move(seed));
    }
    CommentsLoadUpdate result;
    if (!req.ForceNetwork && appPtr->IsTrackerOffline()) {
        result.ErrorKind = TrackerErrorKind::Transport;
    } else {
        Result<std::vector<TrackerIssueComment>, TrackerError> fetched = appPtr->FetchIssueCommentsTyped(req.IssueId);
        if (fetched.has_value()) {
            result.What = CommentsLoadUpdate::Kind::Loaded;
            result.Comments = std::move(fetched.value());
        } else {
            result.Attempted = true;
            result.ErrorKind = fetched.error().Kind;
            result.Error = fetched.error().Detail;
        }
    }
    PostCommentsLoadUpdate(appPtr, req.Gen, req.IssueId, std::move(result));
    finalPosted = true;
}

/// Start a load for the current open (req.Gen). The in-flight latch is published only after the
/// launch returned: a launch that throws (no thread available) leaves a failed, retryable state.
void KickCommentsLoad(AppController& app, const CommentsLoadRequest& req) {
    AppController* appPtr = &app;
    try {
        app.LaunchBackgroundTask([appPtr, req]() { RunCommentsLoad(appPtr, req); });
        s_CommentsState.FetchInFlight = true;
    } catch (const std::exception& ex) {
        LOG_WARN("CommentsModal: could not start loading comments for %s: %s", req.IssueId.c_str(), ex.what());
        s_CommentsState.FetchInFlight = false;
        s_CommentsState.FetchFailed = true;
        s_CommentsState.FetchErrorKind = TrackerErrorKind::Unknown;
        s_CommentsState.Error = ex.what();
    }
}

/// Reload the open issue's thread under a fresh generation, so a slower earlier load that lands
/// afterwards is discarded (#1713). The comments on screen stay until the new list arrives.
void ReloadComments(AppController& app, bool forceNetwork) {
    CommentsModalState& st = s_CommentsState;
    st.Gen = SmatchetCommentsModalGen::AllocGen(s_genCounter);
    st.FetchFailed = false;
    st.FetchErrorKind = TrackerErrorKind::None;
    st.Error.clear();
    CommentsLoadRequest req;
    req.IssueId = st.IssueId;
    req.Gen = st.Gen;
    req.ForceNetwork = forceNetwork;
    KickCommentsLoad(app, req);
}

void EnsureCommentBodyPlans(float fontSize, float width) {
    CommentsModalState& st = s_CommentsState;
    if (st.BodyPlans.size() != st.Comments.size() || st.BodyPlansFontSize != fontSize) {
        st.BodyPlans.clear();
        st.BodyPlans.reserve(st.Comments.size());
        for (const TrackerIssueComment& c : st.Comments) {
            MarkdownPreviewRender::PreviewPlanPtr plan = MarkdownPreviewRender::MakePlan();
            MarkdownPreviewRender::BuildPlan(smatchet::tracker::CommentBodyDisplayMarkdown(c.Body), *plan);
            st.BodyPlans.push_back(std::move(plan));
        }
        st.BodyPlansFontSize = fontSize;
        st.BodyHeightsWidth = -1.0f;
    }
    if (st.BodyHeightsWidth != width || st.BodyHeights.size() != st.Comments.size()) {
        st.BodyHeights.assign(st.Comments.size(), 0.0f);
        st.BodyHeightsWidth = width;
    }
}

void DrawCommentHeader(const TrackerIssueComment& c, std::int64_t nowMs) {
    // Author (emphasised) • relative time, absolute on hover.
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
}

/// How every comment body renders, whether posted or still queued.
MarkdownPreviewRender::Options CommentBodyRenderOptions() {
    MarkdownPreviewRender::Options bodyOpts;
    bodyOpts.mode = MarkdownPreviewRender::Mode::Full;
    // Comment bodies are other people's text: MarkdownPreviewRender opens a clicked href with no
    // scheme check, so links here are shown, not followed.
    bodyOpts.clickableLinks = false;
    return bodyOpts;
}

/// Rebuild this issue's queued comments when the published queue changed. A queued comment that left
/// the queue without moving to the failed list was sent by replay (or discarded), so the thread
/// reloads to show the tracker's copy.
void SyncPendingComments(AppController& app) {
    CommentsModalState& st = s_CommentsState;
    const std::shared_ptr<const PendingActionsSnapshot> snap = app.GetPendingActionsSnapshot();
    if (snap == st.PendingSource) {
        return;
    }
    const bool firstSync = !st.PendingSource;
    const std::string commentKind = PendingActionKindWire(PendingActionKind::CommentAdd);
    std::vector<PendingCommentView> views;
    const auto addIfThisIssue = [&](const PendingActionRecord& row, bool failed) {
        PendingCommentView view;
        std::int64_t queuedAt = 0;
        if (row.Kind == commentKind && row.IssueKey == st.IssueId &&
            smatchet::pendingaction::ParseCommentActionPayload(row.PayloadJson, view.Body, queuedAt)) {
            view.QueueId = row.Id;
            view.Failed = failed;
            views.push_back(std::move(view));
        }
    };
    for (const PendingActionRecord& row : snap->Pending) {
        addIfThisIssue(row, false);
    }
    for (const DeadPendingAction& dead : snap->Dead) {
        addIfThisIssue(dead.Row, true);
    }
    bool sentByReplay = false;
    for (const PendingCommentView& before : st.PendingComments) {
        const bool stillListed = std::any_of(
            views.begin(), views.end(), [&before](const PendingCommentView& v) { return v.QueueId == before.QueueId; });
        sentByReplay = sentByReplay || (!before.Failed && !stillListed);
    }
    st.PendingComments = std::move(views);
    st.PendingSource = snap;
    st.PendingPlansFontSize = 0.0f;
    if (!firstSync && sentByReplay) {
        ReloadComments(app, /*forceNetwork=*/false);
    }
}

/// This issue's queued comments, after the thread: body plus a "waiting to sync" or "failed" badge.
void DrawPendingComments() {
    CommentsModalState& st = s_CommentsState;
    if (st.PendingComments.empty()) {
        return;
    }
    const float fontSize = ImGui::GetFontSize();
    if (st.PendingPlansFontSize != fontSize) {
        for (PendingCommentView& view : st.PendingComments) {
            view.Plan = MarkdownPreviewRender::MakePlan();
            MarkdownPreviewRender::BuildPlan(smatchet::tracker::CommentBodyDisplayMarkdown(view.Body), *view.Plan);
        }
        st.PendingPlansFontSize = fontSize;
    }
    const MarkdownPreviewRender::Options bodyOpts = CommentBodyRenderOptions();
    for (const PendingCommentView& view : st.PendingComments) {
        ImGui::PushID(static_cast<int>(view.QueueId));
        if (view.Failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
            ImGui::TextUnformatted(
                SmatchetLocalization::T("comments.failed_badge", "(failed \xE2\x80\x94 see Offline Queue)"));
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.pending_badge", "(waiting to sync)"));
        }
        MarkdownPreviewRender::RenderPlan(*view.Plan, bodyOpts);
        ImGui::Separator();
        ImGui::PopID();
    }
}

/// Draws the scrollable read-only comment thread. Each comment: author • formatted time • Markdown
/// body (same renderer as the description preview, Full mode). Time formatting reuses
/// smatchet::ai::FormatRelativeTime / FormatAbsoluteTime (both take unix-epoch milliseconds;
/// TrackerIssueComment times are seconds → ×1000).
void DrawCommentsThread() {
    // Only reached with data to show (ShouldRenderContent): an empty list is then known to be empty —
    // a fetch returned none or the saved comment count is 0.
    if (s_CommentsState.Comments.empty()) {
        if (s_CommentsState.PendingComments.empty()) {
            ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.none", "No comments yet."));
        }
        DrawPendingComments();
        return;
    }
    const float width = ImGui::GetContentRegionAvail().x;
    EnsureCommentBodyPlans(ImGui::GetFontSize(), width);
    const MarkdownPreviewRender::Options bodyOpts = CommentBodyRenderOptions();
    const float itemSpacingY = ImGui::GetStyle().ItemSpacing.y;
    const std::int64_t nowMs = smatchet::ai::NowUnixMs();
    for (size_t i = 0; i < s_CommentsState.Comments.size(); ++i) {
        float& height = s_CommentsState.BodyHeights[i];
        if (height > itemSpacingY && !ImGui::IsRectVisible(ImVec2(width, height))) {
            ImGui::Dummy(ImVec2(width, height - itemSpacingY)); // Dummy re-adds the trailing spacing
            continue;
        }
        const float startY = ImGui::GetCursorPosY();
        ImGui::PushID(static_cast<int>(i));
        DrawCommentHeader(s_CommentsState.Comments[i], nowMs);
        MarkdownPreviewRender::RenderPlan(*s_CommentsState.BodyPlans[i], bodyOpts);
        ImGui::Separator();
        ImGui::PopID();
        height = ImGui::GetCursorPosY() - startY;
    }
    DrawPendingComments();
}

/// Pillar 6 freshness of what the thread pane shows.
smatchet::offline::DataFreshness CurrentCommentsFreshness(const AppController& app) {
    const CommentsModalState& st = s_CommentsState;
    smatchet::offline::FreshnessInputs in;
    // An empty list is data only when it is known: the saved count is 0 (Seeded) or a load
    // finished without failing.
    in.HasCache = !st.Comments.empty() || st.Seeded || (!st.FetchInFlight && !st.FetchFailed);
    in.Live = !st.Seeded && !st.FetchFailed;
    in.InFlight = st.FetchInFlight;
    in.LastAttemptFailed = st.FetchFailed;
    in.Connectivity = app.GetLastTrackerConnectivityState();
    return smatchet::offline::ClassifyFreshness(in);
}

/// Above the thread: the freshness cue (last error on hover), Retry after a failed load, and the
/// summary hint when the saved copy is the tooltip blob.
void DrawCommentsStatusLine(AppController& app, smatchet::offline::DataFreshness freshness) {
    const CommentsModalState& st = s_CommentsState;
    if (!smatchet::offline::ShouldRenderContent(freshness)) {
        return; // the thread pane explains the no-data states itself
    }
    const bool hasCue = freshness != smatchet::offline::DataFreshness::Fresh;
    DataFreshnessCue::Draw(freshness, st.Error.empty() ? nullptr : st.Error.c_str());
    if (st.FetchFailed && !st.FetchInFlight) {
        if (hasCue) {
            ImGui::SameLine();
        }
        if (ImGui::SmallButton("Retry")) {
            app.RequestTrackerProbeNow();
            ReloadComments(app, /*forceNetwork=*/true);
        }
    }
    if (st.SeedPartial) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("comments.cached_partial", "Showing a saved summary (latest 20 comments)"));
    }
}

/// Thread pane body: the comments (saved or live), else why there are none yet.
void DrawCommentsThreadPane(AppController& app, smatchet::offline::DataFreshness freshness) {
    CommentsModalState& st = s_CommentsState;
    if (smatchet::offline::ShouldRenderContent(freshness)) {
        DrawCommentsThread();
        return;
    }
    if (freshness == smatchet::offline::DataFreshness::LoadingNoCache) {
        // Nothing saved for this issue: loading-only is the honest state (Pillar 6 LoadingNoCache).
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.loading", "Loading comments..."));
    } else if (st.FetchErrorKind == TrackerErrorKind::Transport) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("comments.unavailable_offline", "Comments are not available offline yet."));
    } else {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.fetch_failed", "Failed to load comments."));
        if (!st.Error.empty()) {
            ImGui::TextWrapped("%s", st.Error.c_str());
        }
    }
    if (!st.FetchInFlight && ImGui::SmallButton("Retry")) { // never while LoadingNoCache: a load is running
        app.RequestTrackerProbeNow();
        ReloadComments(app, /*forceNetwork=*/true);
    }
    DrawPendingComments(); // the user's own queued comments show whatever the tracker's thread state
}

/// UI-thread end of a post: toast what actually happened (posted, saved offline, or failed). A posted
/// or queued comment clears the box — a queued one shows in the thread from the queue snapshot — and a
/// posted one reloads the thread. Only the same open of this issue's modal is touched (OpenGen).
void ApplyCommentPostResult(AppController& app, const std::string& issueId, int openGen,
                            const PendingActionSubmitResult& result) {
    switch (result.K) {
    case PendingActionSubmitResult::Kind::Sent:
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("toast.comment_posted", "Comment Posted"),
                                              SmatchetLocalization::T("comments.posted_body", "Comment added."),
                                              ToastType::Success);
        break;
    case PendingActionSubmitResult::Kind::Queued:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("comments.queued_title", "Comment Queued"),
            SmatchetLocalization::T("comments.queued_body", "Saved offline; it will be posted when the tracker is "
                                                            "reachable."),
            ToastType::Info);
        if (result.QueuedAfterNetworkFailure) {
            app.RequestTrackerProbeNow(); // the send just failed on the network: re-check connectivity now
        }
        break;
    case PendingActionSubmitResult::Kind::Failed:
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.comment_failed", "Comment Failed"),
            result.Error.empty()
                ? std::string(SmatchetLocalization::T("comments.post_failed", "Failed to post comment."))
                : result.Error,
            ToastType::Error);
        break;
    }
    if (SmatchetCommentsModalGen::CallbackIsStale(s_CommentsState.Active, s_CommentsState.OpenGen, openGen,
                                                  s_CommentsState.IssueId, issueId)) {
        return;
    }
    s_CommentsState.PostInFlight = false;
    if (result.K != PendingActionSubmitResult::Kind::Failed) {
        s_CommentsState.PostBuf.assign(CommentsModalState::kPostBufferSize, '\0');
    }
    if (result.K == PendingActionSubmitResult::Kind::Sent) {
        ReloadComments(app, /*forceNetwork=*/false);
    }
}

/// Post on a worker (Pillar 2): sent now, or saved to the pending-action queue when the tracker is
/// unreachable (Pillar 6). As with the load, PostInFlight is published only after the launch returned
/// and the worker always posts a result, a throw included, so Post can never stay disabled.
void KickCommentPost(AppController& app, const std::string& issueId, const std::string& body, int openGen) {
    AppController* appPtr = &app;
    try {
        app.LaunchBackgroundTask([appPtr, issueId, body, openGen]() {
            bool posted = false;
            smatchet::ScopeExit reportThrow([appPtr, &issueId, openGen, &posted]() {
                if (posted) {
                    return;
                }
                try {
                    appPtr->PostToMainThread([appPtr, issueId, openGen]() {
                        ApplyCommentPostResult(*appPtr, issueId, openGen, PendingActionSubmitResult());
                    });
                } catch (const std::exception& ex) {
                    LOG_ERROR("CommentsModal: could not report the comment post for %s: %s", issueId.c_str(),
                              ex.what());
                }
            });
            const PendingActionSubmitResult result = appPtr->SubmitOrQueueComment(issueId, body);
            appPtr->PostToMainThread(
                [appPtr, issueId, openGen, result]() { ApplyCommentPostResult(*appPtr, issueId, openGen, result); });
            posted = true;
        });
        s_CommentsState.PostInFlight = true;
    } catch (const std::exception& ex) {
        LOG_WARN("CommentsModal: could not start posting a comment on %s: %s", issueId.c_str(), ex.what());
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("toast.comment_failed", "Comment Failed"),
            SmatchetLocalization::T("comments.post_failed", "Failed to post comment."), ToastType::Error);
        return;
    }
    // Truthful state (Pillar 6): online the request is being sent now; offline the result toast says
    // "Comment Queued" as soon as the comment is saved, with no request.
    if (!app.IsTrackerOffline()) {
        SmatchetToastManager::Instance().Push(SmatchetLocalization::T("comments.posting_title", "Posting comment"),
                                              issueId.c_str(), ToastType::Info);
    }
}

/// Draws the post box + Post button, disabled while read-only or a post is in flight. Offline a post
/// is saved to the pending-action queue and sent on reconnect; the hint says so.
void DrawCommentsPostBox(AppController& app, bool readOnlyMode) {
    const bool disabled = readOnlyMode || s_CommentsState.PostInFlight;

    ImGui::Separator();
    if (readOnlyMode) {
        ImGui::TextDisabled(
            "%s", SmatchetLocalization::T("comments.disabled_readonly", "(disabled while offline/read-only)"));
    } else if (s_CommentsState.PostInFlight) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.posting", "Posting comment..."));
    } else if (app.IsTrackerOffline()) {
        ImGui::TextDisabled("%s", SmatchetLocalization::T("comments.post_offline_hint",
                                                          "Offline \xE2\x80\x94 your comment will be saved and "
                                                          "posted when the tracker is reachable."));
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
        KickCommentPost(app, s_CommentsState.IssueId, std::string(s_CommentsState.PostBuf.data()),
                        s_CommentsState.OpenGen);
    }
}

} // namespace

void OpenCommentsModal(AppController& app, const std::string& issueId, const std::string& prefillBody) {
    s_CommentsState = CommentsModalState{};
    s_CommentsState.IssueId = issueId;
    s_CommentsState.Active = true;
    s_CommentsState.JustOpened = true;
    // Take the next monotonic token from the file-static counter (survives the reset above) so this
    // open is distinguishable from a prior open's still-in-flight fetch (#1713).
    s_CommentsState.Gen = SmatchetCommentsModalGen::AllocGen(s_genCounter);
    s_CommentsState.OpenGen = s_CommentsState.Gen;
    s_CommentsState.PostBuf.assign(CommentsModalState::kPostBufferSize, '\0');
    if (!prefillBody.empty()) {
        const std::size_t copyLen = (std::min)(prefillBody.size(), CommentsModalState::kPostBufferSize - 1);
        std::memcpy(s_CommentsState.PostBuf.data(), prefillBody.data(), copyLen);
    }
    CommentsLoadRequest req;
    req.IssueId = issueId;
    req.Gen = s_CommentsState.Gen;
    // Pillar 6: start from the copies saved with the ticket (in-memory snapshot, no SQLite); the
    // worker parses them.
    const std::shared_ptr<const std::vector<CachedTicket>> tickets = app.GetActiveTicketsSnapshot();
    if (tickets) {
        const auto it = std::find_if(tickets->begin(), tickets->end(),
                                     [&issueId](const CachedTicket& t) { return t.id == issueId; });
        if (it != tickets->end()) {
            req.SeedThread = it->GetFieldRichValue(kCommentThreadRichKey);
            req.SeedBlob = it->GetFieldValue("comment");
            // A saved count of 0 already proves the thread is empty, online or not.
            s_CommentsState.Seeded =
                req.SeedThread.empty() && req.SeedBlob.empty() && it->GetFieldValueRef("comments") == "0";
        }
    }
    KickCommentsLoad(app, req);
    SyncPendingComments(app); // this issue's queued comments show from the first frame
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

        SyncPendingComments(app);
        const smatchet::offline::DataFreshness freshness = CurrentCommentsFreshness(app);
        DrawCommentsStatusLine(app, freshness);

        // Reserve room for the footer (post box + button + status line).
        const float footerH = 80.0f + ImGui::GetFrameHeightWithSpacing() + ImGui::GetTextLineHeightWithSpacing() +
                              ImGui::GetStyle().ItemSpacing.y * 3.0f;
        const float threadH = ImGui::GetContentRegionAvail().y - footerH;
        ImGui::BeginChild("##CommentsThread", ImVec2(-FLT_MIN, threadH > 0.0f ? threadH : 0.0f), true);
        DrawCommentsThreadPane(app, freshness);
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

CommentsModalSnapshot GetCommentsModalSnapshotForTests() {
    CommentsModalSnapshot snap;
    snap.Active = s_CommentsState.Active;
    snap.FetchInFlight = s_CommentsState.FetchInFlight;
    snap.Seeded = s_CommentsState.Seeded;
    snap.SeedPartial = s_CommentsState.SeedPartial;
    snap.FetchFailed = s_CommentsState.FetchFailed;
    snap.CommentCount = s_CommentsState.Comments.size();
    snap.PendingCommentCount = s_CommentsState.PendingComments.size();
    if (!s_CommentsState.Comments.empty()) {
        snap.FirstAuthor = s_CommentsState.Comments.front().Author;
    }
    return snap;
}
