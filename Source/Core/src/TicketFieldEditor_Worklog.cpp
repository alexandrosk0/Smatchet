#include "TicketFieldEditor_Worklog.h"
#include "Ui/SmatchetInlineError.h"

// The time-tracking (worklog) dialog of the ticket grid, split out of TicketFieldEditor.cpp: seeding,
// the logged/remaining bar, the duration and date inputs, and Save on a worker. Reaches the controller
// only through the IAppThreading + IAppTicketMutations facets.

#include "TicketFieldEditor_detail.h"
#include "CachedTicketTypes.h"
#include "Commands/IAppThreading.h"
#include "CompactDateFormat.h"
#include "DraftStore.h"
#include "GridPane.h"
#include "Interfaces/IAppTicketMutations.h"
#include "Logger.h"
#include "PendingActionTypes.h"
#include "SmatchetLocalization.h"
#include "StringUtil.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldValueParser.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerGridFieldDisplay.h"
#include "Ui/PendingActionSubmitAsync.h"
#include "Ui/DialogKeysPure.h"
#include "Ui/SmatchetCommentsModalGenPure.h"
#include "Ui/SmatchetDialogScope.h"
#include "Ui/SmatchetSubmitKeys.h"
#include "Ui/SmatchetToast.h"
#include "Ui/SmatchetUiSession.h" // g_ui.activePaneForDraw: the pane that draws the dialog
#include "Ui/SmatchetWorklogSubmitPure.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <string>
#include <vector>

namespace {

using TrackerFieldValueUtils::LoadCommentTemplates;

std::string GetCurrentJiraDateTimeString() {
    auto now = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tmLocal{};
    std::tm tmUtc{};
#if defined(_WIN32)
    localtime_s(&tmLocal, &tt);
    gmtime_s(&tmUtc, &tt);
#else
    localtime_r(&tt, &tmLocal);
    gmtime_r(&tt, &tmUtc);
#endif

    int localMin = tmLocal.tm_hour * 60 + tmLocal.tm_min;
    int utcMin = tmUtc.tm_hour * 60 + tmUtc.tm_min;

    // CPP_CODE_AUDIT.md #33 (year-boundary UTC-offset inverted): compare full civil dates
    // (year + day-of-year), not bare tm_yday. tm_yday resets to 0 every January 1st, so
    // comparing it alone conflates "local is a calendar day ahead of UTC" with "local and
    // UTC are in different years" — e.g. Dec 31 (tm_yday=364) vs the following Jan 1
    // (tm_yday=0) used to compare as local-ahead (364 > 0) even for a NEGATIVE-offset user
    // whose local time is actually BEHIND UTC across that boundary, producing a nonsense
    // ~48h-off offset (e.g. "+43:00") in the worklog DateStarted sent to Jira. Multiplying
    // tm_year by 366 (>= any possible tm_yday) keeps the combined ordinal strictly
    // increasing across a year boundary while local/UTC can differ by at most one day.
    const long localOrdinal = static_cast<long>(tmLocal.tm_year) * 366L + tmLocal.tm_yday;
    const long utcOrdinal = static_cast<long>(tmUtc.tm_year) * 366L + tmUtc.tm_yday;
    if (localOrdinal > utcOrdinal) {
        localMin += 24 * 60;
    } else if (localOrdinal < utcOrdinal) {
        utcMin += 24 * 60;
    }
    const int offsetSec = (localMin - utcMin) * 60;

    ParsedJiraDateTime p;
    p.Year = tmLocal.tm_year + 1900;
    p.Month = tmLocal.tm_mon + 1;
    p.Day = tmLocal.tm_mday;
    p.Hour = tmLocal.tm_hour;
    p.Minute = tmLocal.tm_min;
    p.Second = tmLocal.tm_sec;
    p.HasWallTime = true;
    p.OffsetSec = offsetSec;
    p.HasTimeZoneSuffix = true;
    p.TimeZoneWasZ = (offsetSec == 0);

    return FormatJiraDateOrDateTimeForApi(false, p);
}

struct ActiveWorklogDialogState {
    std::string IssueId;
    /// Field id of the cell that opened the dialog ("timespent" or "worklog"). The modal's ImGui
    /// id lives under that cell's CellIdScope, and BOTH columns can be visible on the same row —
    /// so only the owning cell may drive the popup. Without this, the non-owning cell's
    /// BeginPopupModal (a different id, never opened) would fail and tear the state down.
    std::string OwnerFieldId;
    char TimeSpent[64] = "";
    char TimeRemaining[64] = "";
    std::string DateStarted;
    /// DateStarted as the dialog opened with it, to tell a date the user changed from the default.
    std::string InitialDateStarted;
    char WorkDescription[1024] = "";
    std::string OriginalEstimate;
    std::string TotalTimeSpent;
    std::string TotalTimeRemaining;
    bool Initialized = false;
    std::string ErrorMsg;
    bool JustOpened = false;
    bool TimeRemainingManuallyEdited = false;
    /// Pillar 2 (#2043): true from the moment Save dispatches the worklog POST onto a worker
    /// until its post-back lands. Drives the disabled Save button + the "Saving..." cue.
    bool SubmitInFlight = false;
    /// Set by the worker's post-back on success; consumed inside the modal draw, which is the
    /// only place `ImGui::CloseCurrentPopup()` may legally be called (the dispatcher drains
    /// before any window is submitted).
    bool CloseRequested = false;
    /// Generation token of the in-flight submit — see SmatchetCommentsModalGenPure.h. A
    /// post-back that lands after the dialog was closed or re-opened on another ticket is
    /// discarded instead of writing into the fresh state.
    int Gen = 0;
    /// Frames left to put the caret in Time spent (0 = nothing pending): set when the dialog opens
    /// and when Enter there was turned down. Bounded, so a focus that cannot land never fights the user.
    int TimeSpentFocusFramesLeft = 0;
    /// The dialog scope's state (Ui/SmatchetDialogScope.h): Esc and Ctrl+Enter are read through it.
    smatchet::ui::DialogScopeState Dialog;
    /// The text boxes as last handed to the draft store (DraftStore.h), to notice a change without
    /// allocating each frame. Time remaining is kept only while the user edited it (WorklogRemainingDraftText).
    std::string DraftSyncedSpent;
    std::string DraftSyncedDescription;
    std::string DraftSyncedRemaining;
    /// The boxes were filled from an entry kept by an earlier open.
    bool DraftRestored = false;
    /// The grid pane whose cell opened the dialog (g_ui.activePaneForDraw at the open). Empty when it was
    /// opened outside the pane windows. Only that pane's draw hosts the popup, and the target is that pane.
    std::string OwnerPaneId;
    /// The pane this open acts for, latched once on the first draw (LatchWorklogTarget). Save sends to it and
    /// the draft scope names its tracker site, so the kept entry and the worklog always belong to the same site.
    PendingActionTarget Target;
    bool TargetLatched = false;
    /// Draft-store scope of this open (WorklogDraftScope: the target's tracker site and the issue). Empty
    /// until the target is latched, and empty for a target with no tracker: nothing is kept or put back then.
    std::string DraftScope;
    /// Key of this open in the in-flight set (WorklogDraftScope of the target, even one without a backend
    /// key). Empty until the target is latched and for a target with no tracker, which never sends.
    std::string SubmitKey;
    /// ImGui frame on which the owning pane last hosted the popup. An open whose pane stopped drawing it
    /// (the pane closed or hid, or the row left the grid) is no longer seen, so it is ended (OpenIsSeen).
    int HostedFrame = 0;
};

static ActiveWorklogDialogState s_ActiveWorklogState;

/// Submit keys (the tracker site and the issue, ActiveWorklogDialogState::SubmitKey) whose worklog POST is
/// still outstanding; the set is empty when none is. Outside the dialog state because `SubmitInFlight` is
/// reset on every open, so tracking the POST there let Save → Cancel → re-open the SAME ticket enable Save
/// again while the first request was still running — two worklogs for one intent. The POST cannot be
/// cancelled (`AddWorklog` takes no cancel token), so Cancel closes the dialog while the request keeps
/// running; these keys are what keep that fact visible. A SET, not one slot (#2167): submits on different
/// tickets overlap, and a single slot let Save on B overwrite A's latch so re-opening A re-enabled Save
/// mid-POST. Keyed by site as well as issue: two sites can share an issue key, and a submit on one must
/// neither block Save nor hold back the kept entry on the other. Each post-back erases its OWN key
/// UNCONDITIONALLY, before the stale-guard, so a cancelled dialog cannot strand a latch and lock its ticket
/// out of Save forever, and a late post-back cannot clear another submit's still-outstanding latch.
static smatchet::worklog::WorklogSubmitInFlightSet s_WorklogSubmitInFlightKeys;

/// One focus request plus one retry; each needs the frame after it to land.
constexpr int kTimeSpentFocusFrames = 4;
constexpr const char* kTimeSpentFieldLabel = "##WorklogTimeSpent";

/// Draft store surfaces of the text boxes; the scope is the tracker site and the issue (DraftStore.h).
constexpr const char* kWorklogSpentDraftSurface = "worklog.time";
constexpr const char* kWorklogDescriptionDraftSurface = "worklog.description";
constexpr const char* kWorklogRemainingDraftSurface = "worklog.remaining";

/// Keeps what is typed in the two boxes as a draft, so Cancel, Esc, a restart or a crash does not lose it.
/// Called once per frame while the dialog is up; it compares the boxes with what was last handed over.
void SyncWorklogDraft() {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    if (st.SubmitInFlight || st.DraftScope.empty()) {
        return; // being sent (the draft is erased when it lands), or no tracker site to keep it under
    }
    if (st.DraftSyncedSpent != st.TimeSpent) {
        st.DraftSyncedSpent = st.TimeSpent;
        smatchet::drafts::Put(kWorklogSpentDraftSurface, st.DraftScope, st.DraftSyncedSpent);
    }
    if (st.DraftSyncedDescription != st.WorkDescription) {
        st.DraftSyncedDescription = st.WorkDescription;
        smatchet::drafts::Put(kWorklogDescriptionDraftSurface, st.DraftScope, st.DraftSyncedDescription);
    }
    const bool remainingChanged =
        st.TimeRemainingManuallyEdited ? st.DraftSyncedRemaining != st.TimeRemaining : !st.DraftSyncedRemaining.empty();
    if (remainingChanged) {
        st.DraftSyncedRemaining =
            smatchet::worklog::WorklogRemainingDraftText(st.TimeRemainingManuallyEdited, st.TimeRemaining);
        smatchet::drafts::Put(kWorklogRemainingDraftSurface, st.DraftScope, st.DraftSyncedRemaining);
    }
}

void EraseWorklogDraft(const std::string& scope) {
    if (scope.empty()) {
        return; // an open with no tracker site kept nothing
    }
    smatchet::drafts::Erase(kWorklogSpentDraftSurface, scope);
    smatchet::drafts::Erase(kWorklogDescriptionDraftSurface, scope);
    smatchet::drafts::Erase(kWorklogRemainingDraftSurface, scope);
}

/// Once the target is latched: puts back the entry kept for its tracker site and this issue. Not while this
/// issue's last submit is still running (that text is what is being sent), and not without a tracker site.
void RestoreWorklogDraft() {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    st.DraftSyncedSpent.clear();
    st.DraftSyncedDescription.clear();
    st.DraftSyncedRemaining.clear();
    st.DraftRestored = false;
    if (st.SubmitInFlight || st.DraftScope.empty()) {
        return;
    }
    smatchet::drafts::DraftEntry spent;
    smatchet::drafts::DraftEntry description;
    if (smatchet::drafts::Get(kWorklogSpentDraftSurface, st.DraftScope, spent)) {
        std::strncpy(st.TimeSpent, spent.Text.c_str(), sizeof(st.TimeSpent) - 1);
        st.TimeSpent[sizeof(st.TimeSpent) - 1] = '\0';
        st.DraftRestored = true;
    }
    if (smatchet::drafts::Get(kWorklogDescriptionDraftSurface, st.DraftScope, description)) {
        std::strncpy(st.WorkDescription, description.Text.c_str(), sizeof(st.WorkDescription) - 1);
        st.WorkDescription[sizeof(st.WorkDescription) - 1] = '\0';
        st.DraftRestored = true;
    }
    smatchet::drafts::DraftEntry remaining;
    if (smatchet::drafts::Get(kWorklogRemainingDraftSurface, st.DraftScope, remaining)) {
        std::strncpy(st.TimeRemaining, remaining.Text.c_str(), sizeof(st.TimeRemaining) - 1);
        st.TimeRemaining[sizeof(st.TimeRemaining) - 1] = '\0';
        st.TimeRemainingManuallyEdited = true; // it was typed: the spent time no longer derives it
        st.DraftRestored = true;
    }
    st.DraftSyncedSpent = st.TimeSpent;
    st.DraftSyncedDescription = st.WorkDescription;
    st.DraftSyncedRemaining =
        smatchet::worklog::WorklogRemainingDraftText(st.TimeRemainingManuallyEdited, st.TimeRemaining);
}

/// The pane drawing right now (g_ui.activePaneForDraw), empty outside the pane windows.
std::string DrawingPaneId() {
    const GridPane* drawingPane = g_ui.activePaneForDraw;
    return drawingPane != nullptr ? drawingPane->id : std::string();
}

/// Whether `target` names a tracker a worklog can go to. A pane with no live context latches a target with
/// neither a backend nor a backend key: Save then says so in the dialog instead of sending, and nothing is
/// kept, so no entry can land on another pane's site.
bool WorklogTargetHasTracker(const PendingActionTarget& target) {
    return target.Backend != nullptr || !target.BackendKey.empty();
}

/// Latches the pane this open acts for, then its draft scope, then puts back the kept entry. Called on every
/// draw that hosts the popup and acts once, on the first, before any input reads the boxes. The pane is the
/// one whose cell opened the dialog (OwnerPaneId), whichever pane holds focus: a click in a pane that is not
/// focused opens the dialog before the pane focus follows it. Opened outside the pane windows, it is the
/// focused pane. Save sends to this same target, so the kept entry and the worklog cannot name different sites.
void LatchWorklogTarget(IAppTicketMutations& mutations) {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    if (st.TargetLatched) {
        return;
    }
    st.Target = st.OwnerPaneId.empty() ? mutations.LatchPendingActionTarget()
                                       : mutations.LatchPendingActionTargetForPane(st.OwnerPaneId);
    st.TargetLatched = true;
    st.DraftScope.clear();
    st.SubmitKey.clear();
    if (!WorklogTargetHasTracker(st.Target)) {
        LOG_WARN("Worklog: pane %s has no tracker, so the dialog for %s keeps no entry and cannot save",
                 st.OwnerPaneId.c_str(), st.IssueId.c_str());
    } else {
        st.SubmitKey = smatchet::worklog::WorklogDraftScope(st.Target.BackendKey, st.IssueId);
        if (!st.Target.BackendKey.empty()) {
            st.DraftScope = st.SubmitKey;
        }
    }
    // Seeded from this site's outstanding submit only (#2085): a re-open while it runs keeps Save disabled
    // and the "Saving worklog..." cue up, while a submit for the same issue key on another site holds back
    // neither Save nor this site's kept entry. Before the restore, which skips the text being sent.
    st.SubmitInFlight = smatchet::worklog::WorklogSubmitOutstandingFor(s_WorklogSubmitInFlightKeys, st.SubmitKey);
    RestoreWorklogDraft();
}

/// The dialog's open ends: nothing of it may outlive the close, least of all the target's strong handle
/// on the tracker backend.
void EndWorklogOpen() {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    st.Initialized = false;
    st.Target = PendingActionTarget();
    st.TargetLatched = false;
}

/// Whether the owning pane hosted the popup on this frame or the one before. When it stopped (the pane was
/// closed or hidden, or the row left the grid) the open is no longer seen: a save result must then go to a
/// toast, not into a dialog nobody can read. Without an ImGui frame there is nothing to measure against.
bool OpenIsSeen() {
    if (ImGui::GetCurrentContext() == nullptr) {
        return true;
    }
    return s_ActiveWorklogState.HostedFrame >= ImGui::GetFrameCount() - 1;
}

/// The "restored" line with its Clear button, above the time inputs. Clearing empties both boxes and puts
/// back the issue's own Time remaining; the next SyncWorklogDraft erases the kept entry.
void DrawWorklogDraftNotice() {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    if (!st.DraftRestored) {
        return;
    }
    ImGui::TextDisabled("%s", SmatchetLocalization::T("worklog.draft_restored", "Restored your unsaved entry."));
    ImGui::SameLine();
    if (ImGui::SmallButton(SmatchetLocalization::T("worklog.draft_clear", "Clear"))) {
        st.TimeSpent[0] = '\0';
        st.WorkDescription[0] = '\0';
        std::strncpy(st.TimeRemaining, st.TotalTimeRemaining.c_str(), sizeof(st.TimeRemaining) - 1);
        st.TimeRemaining[sizeof(st.TimeRemaining) - 1] = '\0';
        st.TimeRemainingManuallyEdited = false;
        st.DraftRestored = false;
        st.TimeSpentFocusFramesLeft = kTimeSpentFocusFrames;
    }
}

#if defined(SMATCHET_BUILD_UI_TESTS)
/// Latched by the draw for GetWorklogDialogSnapshotForTests.
static bool s_WorklogTimeSpentActiveForTests = false;
#endif

/// Monotonic submit-generation counter, deliberately OUTSIDE the dialog state so it survives
/// the per-open reset (the #1713 lesson: a counter reset with the state makes the guard inert).
static int s_WorklogGenCounter = 0;

// Computes the progress-bar logged/remaining seconds for the worklog dialog. Pure arithmetic on
// the parsed durations — extracted so the modal draw body stays layout-focused. Mirrors the
// auto-deduction (spent reduces remaining) unless the user manually edited the remaining field.
void ComputeWorklogProgressSeconds(long long& outDisplaySpentSec, long long& outDisplayRemSec) {
    const long long spentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeSpent);
    const long long remSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
    const long long newSpentSec = smatchet::worklog::TypedDurationSeconds(s_ActiveWorklogState.TimeSpent);

    outDisplaySpentSec = spentSec + newSpentSec;
    outDisplayRemSec = remSec;
    if (newSpentSec > 0 && !s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = (std::max)(0LL, remSec - newSpentSec);
    } else if (s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = smatchet::worklog::TypedDurationSeconds(s_ActiveWorklogState.TimeRemaining);
    }
}

// UI-thread end of a Save. Releases this submit's latch first, then reports the outcome: into the dialog
// while it is still this submit's, otherwise as a toast (the worklog was sent, saved or rejected, and
// closing the dialog cannot undo that). A queued worklog always gets a toast — the dialog closes but
// nothing has reached the tracker yet (Quality Pillar 6: say what actually happened).
void ApplyWorklogSaveResult(int gen, const std::string& issueId, const std::string& submitKey,
                            const std::string& draftScope, const PendingActionSubmitResult& result) {
    // Before any early-out: a latch left set would block Save on this ticket for the rest of the session.
    // Erasing only THIS key leaves any other submit latched.
    smatchet::worklog::ClearWorklogSubmitInFlight(s_WorklogSubmitInFlightKeys, submitKey);
    if (s_ActiveWorklogState.Initialized && !OpenIsSeen()) {
        // No pane draws the dialog any more: end the open, so the outcome below takes the toast path and the
        // target's backend handle goes with it.
        EndWorklogOpen();
    }
    const bool ok = result.K != PendingActionSubmitResult::Kind::Failed;
    if (ok) {
        EraseWorklogDraft(draftScope); // sent or saved to the queue: the kept entry is done
    }
    const std::string err = result.Error.empty() ? std::string("the worklog could not be saved.") : result.Error;
    if (result.K == PendingActionSubmitResult::Kind::Queued && result.NeedsReview) {
        // The send may have landed: replay never resends it, so say what the user has to do.
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("worklog.toast.review_title", "Worklog may already be logged"),
            issueId + ": " +
                SmatchetLocalization::T("worklog.toast.review_body",
                                        "The tracker did not answer, so the worklog may already be logged. Check "
                                        "the issue, then send or discard it in the Offline Queue panel."),
            ToastType::Warning);
    } else if (result.K == PendingActionSubmitResult::Kind::Queued) {
        SmatchetToastManager::Instance().Push(
            SmatchetLocalization::T("worklog.toast.queued_title", "Worklog queued offline"),
            issueId + ": " +
                SmatchetLocalization::T("worklog.toast.queued_body",
                                        "Saved offline; it will be logged when the tracker is reachable."),
            ToastType::Info);
    }
    // The comments-modal stale-guard (#1713): the dialog closed, moved to another ticket, or a newer
    // submit superseded this one, so nothing may be written into the dialog state.
    if (SmatchetCommentsModalGen::CallbackIsStale(s_ActiveWorklogState.Initialized, s_ActiveWorklogState.Gen, gen,
                                                  s_ActiveWorklogState.IssueId, issueId)) {
        // Stale because the SAME ticket on the same site was re-opened mid-submit (#2168): that open seeded
        // SubmitInFlight from the latch released above, so the re-opened dialog would otherwise keep Save disabled.
        if (smatchet::worklog::StalePostBackReleasesDialogSubmit(s_ActiveWorklogState.Initialized,
                                                                 s_ActiveWorklogState.SubmitKey, submitKey)) {
            s_ActiveWorklogState.SubmitInFlight = false;
        }
        if (result.K == PendingActionSubmitResult::Kind::Sent) {
            SmatchetToastManager::Instance().Push(SmatchetLocalization::T("worklog.toast.saved_title", "Worklog saved"),
                                                  issueId, ToastType::Success);
        } else if (!ok) {
            SmatchetToastManager::Instance().Push(
                SmatchetLocalization::T("worklog.toast.failed_title", "Worklog failed"), issueId + ": " + err,
                ToastType::Error);
        }
        return;
    }
    s_ActiveWorklogState.SubmitInFlight = false;
    if (ok) {
        // CloseCurrentPopup() is only legal inside the popup's own draw — request the close and let
        // RenderTimeTrackingModal honour it on this frame's draw.
        s_ActiveWorklogState.CloseRequested = true;
    } else {
        // The in-dialog message is the cue while the dialog is open; no duplicate toast.
        s_ActiveWorklogState.ErrorMsg = "Failed: " + err;
    }
}

// Validates the worklog inputs and, on success, submits on a background worker (Pillar 2, #2043: the
// request used to freeze the render thread for the whole tracker round trip). Offline the worklog is
// saved to the pending-action queue and logged on reconnect (Quality Pillar 6). Sets ErrorMsg on a
// validation failure; ApplyWorklogSaveResult reports the outcome.
//
// Lifetime: both facets are the app-lifetime controller, which joins every background task before
// teardown, and PostToMainThread no-ops after shutdown began — so the captured pointer cannot dangle.
void HandleWorklogSave(IAppThreading& threading, IAppTicketMutations& mutations) {
    // Two gates, not one: the dialog's own flag AND the cross-instance in-flight id set. The second
    // is what stops Save → Cancel → re-open → Save creating two worklogs for one intent.
    if (!smatchet::worklog::CanSubmitWorklog(s_ActiveWorklogState.SubmitInFlight) ||
        smatchet::worklog::WorklogSubmitOutstandingFor(s_WorklogSubmitInFlightKeys, s_ActiveWorklogState.SubmitKey)) {
        return; // a submit is already in flight — never queue a duplicate worklog
    }
    s_ActiveWorklogState.ErrorMsg.clear();
    // The target is latched on the dialog's first draw, before any button exists. One without a tracker
    // (the pane has no live context) is never sent: the send-or-queue route could neither send nor queue it.
    if (!WorklogTargetHasTracker(s_ActiveWorklogState.Target)) {
        s_ActiveWorklogState.ErrorMsg = "Failed: this pane has no tracker connected, so the worklog was not sent.";
        return;
    }
    const std::string validationError =
        smatchet::worklog::ValidateWorklogSubmission(s_ActiveWorklogState.TimeSpent, s_ActiveWorklogState.DateStarted);
    if (!validationError.empty()) {
        s_ActiveWorklogState.ErrorMsg = validationError;
        return;
    }
    // A Time remaining the user typed must read too: Jira refuses a worklog whose remaining estimate it cannot parse.
    const TrackerParseExplain::ParseMessage remainingProblem = smatchet::worklog::TimeRemainingProblem(
        s_ActiveWorklogState.TimeRemainingManuallyEdited ? std::string(s_ActiveWorklogState.TimeRemaining)
                                                         : std::string());
    if (remainingProblem.IsError()) {
        s_ActiveWorklogState.ErrorMsg = TrackerParseExplain::DescribeParseMessage(remainingProblem);
        return;
    }

    IAppTicketMutations* mutationsPtr = &mutations;
    // Latched for this open (LatchWorklogTarget): the worklog goes to the pane the dialog acts for,
    // wherever focus is now, and to the site whose kept entry the boxes show. Whether the tracker is reachable
    // is read now, not at the open: the dialog can stay up across a drop, and the send-or-queue route follows it.
    PendingActionTarget target = s_ActiveWorklogState.Target;
    target.Connectivity = mutations.GetLastTrackerConnectivityState();
    const std::string issueId = s_ActiveWorklogState.IssueId;
    // The entry that is done when this lands is the one this dialog was keeping.
    const std::string draftScope = s_ActiveWorklogState.DraftScope;
    const std::string submitKey = s_ActiveWorklogState.SubmitKey;
    // What goes out is the canonical form ("1h 30m"), whatever natural spelling was typed.
    const std::string timeSpent = smatchet::worklog::CanonicalDurationText(s_ActiveWorklogState.TimeSpent);
    const std::string timeRemaining = smatchet::worklog::CanonicalDurationText(s_ActiveWorklogState.TimeRemaining);
    const std::string adjEst = s_ActiveWorklogState.TimeRemainingManuallyEdited ? "new" : "auto";
    const std::string description = s_ActiveWorklogState.WorkDescription;
    const std::string startedDate = s_ActiveWorklogState.DateStarted;

    // Latched before the launch: the post-back drains on this thread later, never inside this call.
    const int gen = SmatchetCommentsModalGen::AllocGen(s_WorklogGenCounter);
    s_ActiveWorklogState.Gen = gen;
    s_ActiveWorklogState.SubmitInFlight = true;
    smatchet::worklog::MarkWorklogSubmitInFlight(s_WorklogSubmitInFlightKeys, submitKey);
    try {
        smatchet::ui::SubmitPendingActionAsync(
            threading,
            [mutationsPtr, target, issueId, timeSpent, timeRemaining, adjEst, description, startedDate]() {
                return mutationsPtr->SubmitOrQueueWorklog(target, issueId, timeSpent, timeRemaining, adjEst,
                                                          description, startedDate);
            },
            [gen, issueId, submitKey, draftScope](const PendingActionSubmitResult& result) {
                ApplyWorklogSaveResult(gen, issueId, submitKey, draftScope, result);
            });
    } catch (const std::exception& ex) {
        // The launch failed (no thread): nothing was sent, so release both latches here.
        LOG_WARN("Worklog: could not start saving the worklog for %s: %s", issueId.c_str(), ex.what());
        smatchet::worklog::ClearWorklogSubmitInFlight(s_WorklogSubmitInFlightKeys, submitKey);
        s_ActiveWorklogState.SubmitInFlight = false;
        s_ActiveWorklogState.ErrorMsg = "Failed: the worklog could not be saved. Try again.";
    }
}

// Draws the time-tracking modal popup (logged/remaining bar, duration inputs, date picker, work
// description with templates, Save/Cancel). Owns its OpenPopup→BeginPopupModal lifecycle keyed on
// s_ActiveWorklogState. Extracted from the tail of RenderFieldCell; behaviour byte-identical.
// Logged/remaining labels + Jira-blue progress bar + original-estimate note.
void DrawWorklogProgressBar() {
    long long displaySpentSec = 0;
    long long displayRemSec = 0;
    ComputeWorklogProgressSeconds(displaySpentSec, displayRemSec);

    long long totalSec = displaySpentSec + displayRemSec;
    float fraction = 0.0f;
    if (totalSec > 0) {
        fraction = (float)displaySpentSec / (float)totalSec;
    }

    std::string loggedLabel = FormatWorkDurationFromSeconds(displaySpentSec);
    if (loggedLabel.empty())
        loggedLabel = "0m";
    loggedLabel += " logged";

    std::string remainingLabel = FormatWorkDurationFromSeconds(displayRemSec);
    if (remainingLabel.empty())
        remainingLabel = "0m";
    remainingLabel += " remaining";

    ImGui::TextUnformatted(loggedLabel.c_str());
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(remainingLabel.c_str()).x);
    ImGui::TextUnformatted(remainingLabel.c_str());

    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.12f, 0.45f, 0.88f, 1.00f)); // Jira blue
    ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 14.0f), "");
    ImGui::PopStyleColor();

    if (!s_ActiveWorklogState.OriginalEstimate.empty()) {
        ImGui::TextDisabled("The original estimate for this work item was %s.",
                            s_ActiveWorklogState.OriginalEstimate.c_str());
    }
}

// Puts the caret in Time spent while a focus request is pending. Call right before the Time spent
// field, with nothing drawn in between (the request targets the next item).
//
// The duration field opens its suggestions list the first time it gains the caret while empty, and
// that list takes the keyboard away again. A caret this dialog placed must stay put, so the field is
// told it "already had" the caret. That lives in the per-field slots DrawDurationFieldWithSuggestions
// keeps in the window's state storage under PushID(label): the last-active id is written on every
// pending frame, and on the first one the three popup flags a previous open may have left set are
// cleared. The slot names must match that function. If the last-active slot drifts, the UI test
// WorklogDialog_OpensWithCaretInTimeSpent (group DurationInlineEdit) fails, but nothing guards the
// three popup-flag names. A click that puts the caret in the empty field, the down arrow and the
// list button open the suggestions as before.
void PrimeTimeSpentFocus(ImGuiID spentInputId) {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    if (st.TimeSpentFocusFramesLeft <= 0) {
        return;
    }
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool otherDialogItemActive = g.ActiveId != 0 && g.ActiveId != spentInputId && g.ActiveIdWindow != nullptr &&
                                       g.ActiveIdWindow->RootWindow == g.CurrentWindow->RootWindow;
    if (otherDialogItemActive) {
        st.TimeSpentFocusFramesLeft = 0; // the user is already in another field: leave them there
        return;
    }
    ImGuiStorage* storage = ImGui::GetStateStorage();
    ImGui::PushID(kTimeSpentFieldLabel);
    if (st.TimeSpentFocusFramesLeft == kTimeSpentFocusFrames) {
        storage->SetInt(ImGui::GetID("##popupWasOpen"), 0);
        storage->SetInt(ImGui::GetID("##valueSelectedFromPopup"), 0);
        storage->SetInt(ImGui::GetID("##needRepositionAndFocus"), 0);
    }
    storage->SetInt(ImGui::GetID("##lastActiveId"), static_cast<int>(spentInputId));
    ImGui::PopID();
    if (st.TimeSpentFocusFramesLeft % 2 == 0) {
        ImGui::SetKeyboardFocusHere(); // lands on the next frame; asked again once if it did not
    }
}

// Rewrites a duration buffer to its canonical spelling when what is typed reads: "90 m" -> "1h 30m".
// Text that does not read is left as typed, so the user can fix it.
void RewriteDurationBufferToCanonical(char* buf, std::size_t bufSize) {
    const std::string typed = buf;
    const std::string canonical = smatchet::worklog::CanonicalDurationText(typed);
    if (canonical != typed && canonical.size() < bufSize) {
        std::memcpy(buf, canonical.c_str(), canonical.size() + 1);
    }
}

// Under a duration field: the red line that says what is wrong while the text does not read, or the dim
// "= 1h 30m" once it reads in a spelling other than the canonical one. Blank shows the format hint instead.
// `required`: Time spent must be above zero; Time remaining may be zero or blank.
void DrawTypedDurationFeedback(const char* typedText, bool required) {
    const std::string typed = typedText;
    const TrackerParseExplain::DurationParse parsed = smatchet::worklog::ParseTypedDuration(typed);
    if (parsed.Empty) {
        if (required) {
            ImGui::TextDisabled(
                "%s", SmatchetLocalization::T("worklog.duration_hint", "Examples: 1h 30m, 1.5h, 90m, 2d (d=8h, w=5d)"));
        }
        return;
    }
    if (!parsed.Ok) {
        smatchet::ui::DrawInlineParseError(parsed.Message);
        return;
    }
    if (required && parsed.Seconds <= 0) {
        TrackerParseExplain::ParseMessage zero;
        zero.Id = TrackerParseExplain::ParseMessageId::DurationRequiredPositive;
        smatchet::ui::DrawInlineParseError(zero);
        return;
    }
    // Only a spelling that is not already canonical earns a preview line: "1h 30m" shows nothing.
    if (!EqualsIgnoreCaseAscii(TrimCopyAsciiWhitespace(typed), parsed.Canonical)) {
        smatchet::ui::DrawInlineParseHint("= " + parsed.Canonical);
    }
}

// Time-spent + time-remaining duration inputs, including the auto-decrement of remaining. Returns
// true when Enter was pressed in Time spent (the dialog's quick submit).
bool DrawWorklogTimeInputs() {
    ImGui::Text("Time spent *");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(kTimeSpentFieldLabel);
    const ImGuiID spentInputId = ImGui::GetID("##duration_input");
    ImGui::PopID();
    PrimeTimeSpentFocus(spentInputId);
    bool spentEnter = false;
    const bool spentCommitted = DrawDurationFieldWithSuggestions(kTimeSpentFieldLabel, s_ActiveWorklogState.TimeSpent,
                                                                 sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr,
                                                                 nullptr, nullptr, false, &spentEnter);
    const bool spentActive = ImGui::GetActiveID() == spentInputId;
    if (s_ActiveWorklogState.TimeSpentFocusFramesLeft > 0) {
        s_ActiveWorklogState.TimeSpentFocusFramesLeft =
            spentActive ? 0 : s_ActiveWorklogState.TimeSpentFocusFramesLeft - 1;
    }
#if defined(SMATCHET_BUILD_UI_TESTS)
    s_WorklogTimeSpentActiveForTests = spentActive;
#endif
    if (spentCommitted) {
        // The field takes the canonical spelling once it is committed: "90 m" becomes "1h 30m".
        RewriteDurationBufferToCanonical(s_ActiveWorklogState.TimeSpent, sizeof(s_ActiveWorklogState.TimeSpent));
        if (!s_ActiveWorklogState.TimeRemainingManuallyEdited) {
            long long spentVal = smatchet::worklog::TypedDurationSeconds(s_ActiveWorklogState.TimeSpent);
            long long remVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
            if (spentVal > 0) {
                long long newRem = (std::max)(0LL, remVal - spentVal);
                std::string formattedRem = FormatWorkDurationFromSeconds(newRem);
                std::strncpy(s_ActiveWorklogState.TimeRemaining, formattedRem.c_str(),
                             sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
            } else {
                std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(),
                             sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
                s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';
            }
        }
    }
    DrawTypedDurationFeedback(s_ActiveWorklogState.TimeSpent, /*required=*/true);

    ImGui::Spacing();
    ImGui::Text("Time remaining");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (DrawDurationFieldWithSuggestions("##WorklogTimeRemaining", s_ActiveWorklogState.TimeRemaining,
                                         sizeof(s_ActiveWorklogState.TimeRemaining), 0, nullptr, nullptr,
                                         &s_ActiveWorklogState.TimeRemainingManuallyEdited, false)) {
        RewriteDurationBufferToCanonical(s_ActiveWorklogState.TimeRemaining,
                                         sizeof(s_ActiveWorklogState.TimeRemaining));
    }
    DrawTypedDurationFeedback(s_ActiveWorklogState.TimeRemaining, /*required=*/false);
    return spentEnter;
}

struct WorklogKeys {
    bool Save = false;   ///< Ctrl+Enter
    bool Cancel = false; ///< Esc
};

// True when the dialog field holding the caret no longer shows the text it had when the caret
// entered: something typed that has not reached the dialog's own state yet (a half-typed date).
bool ActiveFieldTextChanged(const ImGuiContext& g) {
    const ImGuiInputTextState& input = g.InputTextState;
    if (g.ActiveId == 0 || input.ID != g.ActiveId || input.TextA.Data == nullptr ||
        input.TextToRevertTo.Data == nullptr) {
        return false;
    }
    return std::strcmp(input.TextA.Data, input.TextToRevertTo.Data) != 0;
}

// Reads the dialog's own keys. Call inside the modal before any of its items are drawn: a key the
// dialog takes must not also reach the field or button that has the keyboard.
// - Only while the dialog itself has the keyboard. A popup above it (suggestions, date picker,
//   templates) keeps its keys, and so does any other window.
// - Ctrl+Enter saves from anywhere, once the dialog has had the keyboard for a frame. The field being
//   typed in is left first, which commits its text the way clicking Save would. Nav activation is dropped
//   for the frame, because ImGui activates the item under the keyboard cursor on Enter whatever the
//   modifiers: without that, Ctrl+Enter with the cursor on Cancel would save and close, and on the date
//   picker's reset would save a reset date.
// - Esc: see smatchet::dialogkeys::ResolveEscape. Leaving the field here ends the edit the way clicking away does:
//   Time spent, Time remaining and the description keep what was typed (ImGui's own Esc would put
//   the field back to its text from before the caret entered); a start date or time that does not
//   parse yet goes back to the last valid one, as it does on a click away.
// Both are the dialog scope's (smatchet::ui::DialogReadKeys); this only says what "nothing typed" means here.
WorklogKeys ReadWorklogKeys() {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    const ImGuiContext& g = *ImGui::GetCurrentContext();
    const bool fieldActive =
        g.ActiveId != 0 && g.ActiveIdWindow != nullptr && g.ActiveIdWindow->RootWindow == g.CurrentWindow->RootWindow;
    // Nothing typed: Time spent and the description are empty, Time remaining and the start date
    // still show what the dialog opened with (Time remaining only moves by itself when Time spent
    // has a value), and the field holding the caret has not been typed in.
    const bool nothingTyped = st.TimeSpent[0] == '\0' && st.WorkDescription[0] == '\0' &&
                              st.TotalTimeRemaining == st.TimeRemaining && st.DateStarted == st.InitialDateStarted &&
                              !(fieldActive && ActiveFieldTextChanged(g));
    const smatchet::ui::DialogKeys dialogKeys = smatchet::ui::DialogReadKeys(st.Dialog, nothingTyped);
    WorklogKeys keys;
    keys.Save = dialogKeys.Submit;
    keys.Cancel = dialogKeys.Esc == smatchet::dialogkeys::EscapeAction::Cancel;
    return keys;
}

// Save / Cancel row with the keyboard hint. Pillar 2 (#2043): while the worklog POST is in flight the
// Save button is disabled and a visible "Saving worklog..." cue is drawn beside it — the dialog stays
// interactive (the whole app used to freeze here) and the user can see work is happening.
// `wasInFlight` is latched BEFORE the Save button so BeginDisabled/EndDisabled stay paired across the
// click that flips the flag; the cue itself re-reads the LIVE flag afterwards so it appears on the
// very frame Save dispatched (cue-before-stall ordering — a snapshot here would delay it by one
// frame). The labelled id is the bucket-E visible-cue anchor.
// One HandleWorklogSave call per frame whatever asked for it (button, Ctrl+Enter, Enter in Time
// spent), and it has the same validation and in-flight gates for all three.
void DrawWorklogButtons(IAppThreading& threading, IAppTicketMutations& mutations, const WorklogKeys& keys,
                        bool spentEnter) {
    ActiveWorklogDialogState& st = s_ActiveWorklogState;
    const bool wasInFlight = st.SubmitInFlight;
    if (wasInFlight) {
        ImGui::BeginDisabled();
    }
    const bool saveClicked = smatchet::ui::KeyHintButton("Save", smatchet::ui::CtrlEnterKeyName(), ImVec2(80, 0));
    if (wasInFlight) {
        ImGui::EndDisabled();
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", SmatchetLocalization::T("worklog.save_shortcut_tooltip",
                                                        "Ctrl+Enter saves from any field. Enter saves from Time "
                                                        "spent."));
    }
    if (saveClicked || keys.Save || spentEnter) {
        HandleWorklogSave(threading, mutations);
        if (smatchet::worklog::ShouldRefocusTimeSpentAfterSaveKey(spentEnter || keys.Save, st.SubmitInFlight,
                                                                  st.TimeSpent)) {
            st.TimeSpentFocusFramesLeft = kTimeSpentFocusFrames;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0)) || keys.Cancel) {
        ImGui::CloseCurrentPopup();
        EndWorklogOpen();
    }
    if (wasInFlight && ImGui::IsItemHovered()) {
        // Be honest about what Cancel does mid-submit: AddWorklog takes no cancel token, so the
        // request is already gone. Closing the dialog does not un-send it; the outcome arrives
        // as a toast instead.
        ImGui::SetTooltip("%s", SmatchetLocalization::T("worklog.cancel_inflight_tooltip",
                                                        "The worklog request was already sent — closing this dialog "
                                                        "won't cancel it. The result will appear as a notification."));
    }
    const char* cue = st.SubmitInFlight
                          ? SmatchetLocalization::T("worklog.saving", "Saving worklog...")
                          : SmatchetLocalization::T("worklog.keys_hint", "Ctrl+Enter to save \xC2\xB7 Esc to cancel");
    ImGui::SameLine();
    if (ImGui::GetContentRegionAvail().x < ImGui::CalcTextSize(cue).x) {
        ImGui::NewLine(); // a large font: the hint goes under the buttons instead of being cut off
    }
    ImGui::TextDisabled("%s", cue);
}

// Work-description label + templates popup + the multiline description input.
void DrawWorklogDescription() {
    ImGui::Text("Work description");
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
    if (ImGui::Button("Templates ▼", ImVec2(90.0f, 0.0f))) {
        ImGui::OpenPopup("comment_templates_popup");
    }
    if (ImGui::BeginPopup("comment_templates_popup")) {
        std::vector<std::string> templates = LoadCommentTemplates();
        if (templates.empty()) {
            ImGui::TextDisabled("No templates configured in Preferences.");
        } else {
            for (const auto& item : templates) {
                if (ImGui::Selectable(item.c_str())) {
                    std::strncpy(s_ActiveWorklogState.WorkDescription, item.c_str(),
                                 sizeof(s_ActiveWorklogState.WorkDescription) - 1);
                    s_ActiveWorklogState.WorkDescription[sizeof(s_ActiveWorklogState.WorkDescription) - 1] = '\0';
                }
            }
        }
        ImGui::EndPopup();
    }
    // Ctrl+Enter here is read by ReadWorklogKeys for the whole dialog; the helper adds the placeholder
    // and keeps Tab moving on.
    smatchet::ui::SubmitMultilineInput(
        "##WorklogDesc", s_ActiveWorklogState.WorkDescription, sizeof(s_ActiveWorklogState.WorkDescription),
        ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4),
        SmatchetLocalization::T("worklog.desc_placeholder", "What did you work on? (optional)"));
}

} // namespace

namespace TicketFieldEditorWorklog {

// Primes the time-tracking dialog for `ticket`, owned by the cell whose column is `ownerFieldId`
// (the only cell allowed to draw the modal — see ActiveWorklogDialogState::OwnerFieldId). Shared
// by the `timespent` and `worklog` cells so both entry points open an identically-seeded dialog.
void OpenWorklogDialog(const CachedTicket& ticket, const std::string& ownerFieldId) {
    s_ActiveWorklogState.IssueId = ticket.id;
    s_ActiveWorklogState.OwnerFieldId = ownerFieldId;
    // The grid cells open it from inside their pane's draw, so this is the pane whose cell was clicked.
    s_ActiveWorklogState.OwnerPaneId = DrawingPaneId();
    s_ActiveWorklogState.TimeSpent[0] = '\0';
    s_ActiveWorklogState.OriginalEstimate = ticket.GetFieldValue("timeoriginalestimate");
    s_ActiveWorklogState.TotalTimeSpent = ticket.GetFieldValue("timespent");
    s_ActiveWorklogState.TotalTimeRemaining = ticket.GetFieldValue("timeestimate");
    std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(),
                 sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
    s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';

    s_ActiveWorklogState.DateStarted = GetCurrentJiraDateTimeString();
    s_ActiveWorklogState.InitialDateStarted = s_ActiveWorklogState.DateStarted;

    s_ActiveWorklogState.WorkDescription[0] = '\0';
    s_ActiveWorklogState.ErrorMsg.clear();
    s_ActiveWorklogState.Initialized = true;
    s_ActiveWorklogState.JustOpened = true;
    s_ActiveWorklogState.TimeRemainingManuallyEdited = false;
    // Seeded from this site's outstanding submit when the target is latched (LatchWorklogTarget), on the
    // first draw and before any button exists. Both cell entry points share that draw, so both get the
    // re-open guard (#2085).
    s_ActiveWorklogState.SubmitInFlight = false;
    s_ActiveWorklogState.SubmitKey.clear();
    s_ActiveWorklogState.HostedFrame = ImGui::GetCurrentContext() != nullptr ? ImGui::GetFrameCount() : 0;
    s_ActiveWorklogState.CloseRequested = false;
    s_ActiveWorklogState.TimeSpentFocusFramesLeft = kTimeSpentFocusFrames;
    s_ActiveWorklogState.Dialog = smatchet::ui::DialogScopeState();
    // The target, and with it the draft scope and the kept entry, is latched from the draw, which knows the
    // pane; nothing left from the previous open may survive until then.
    s_ActiveWorklogState.Target = PendingActionTarget();
    s_ActiveWorklogState.TargetLatched = false;
    s_ActiveWorklogState.DraftScope.clear();
    s_ActiveWorklogState.DraftRestored = false;
    // Burn a fresh generation on every open (#1713 contract). Without this, a post-back from a
    // submit whose dialog was cancelled mid-flight would still match `Gen` after the user
    // re-opened the SAME ticket and would close the freshly-opened dialog under them.
    s_ActiveWorklogState.Gen = SmatchetCommentsModalGen::AllocGen(s_WorklogGenCounter);
}

// Draws the "Log work" / time-spent button for the SpecialTimeSpent column and, on click, primes
// the worklog dialog state. Shares its flat-button affordance and its dialog seeding with the
// `worklog` cell, so the two entry points cannot drift apart.
void RenderTimeSpentButton(const CachedTicket& ticket, const std::string& fieldId, const std::string& currentValue,
                           float availWidth, bool tooltipsEnabled) {
    const std::string buttonText = currentValue.empty() ? "Log work" : currentValue;
    if (TrackerGridFieldDisplay::DrawCellActionButton(buttonText, availWidth)) {
        OpenWorklogDialog(ticket, fieldId);
    }

    if (tooltipsEnabled && ImGui::IsItemHovered()) {
        ImGui::BeginTooltip();
        if (currentValue.empty()) {
            ImGui::TextUnformatted("No work logged yet. Click to log work.");
        } else {
            ImGui::Text("Total Time Spent: %s\nClick to log work / edit estimates.", currentValue.c_str());
        }
        ImGui::EndTooltip();
    }
}

// `columnFieldId` is the cell currently being drawn; the modal is submitted only by the cell that
// opened it, so a row showing BOTH `timespent` and `worklog` draws it exactly once per frame.
void RenderTimeTrackingModal(IAppThreading& threading, IAppTicketMutations& mutations, const CachedTicket& ticket,
                             const std::string& columnFieldId) {
    if (!(s_ActiveWorklogState.Initialized && s_ActiveWorklogState.IssueId == ticket.id &&
          s_ActiveWorklogState.OwnerFieldId == columnFieldId)) {
        return;
    }
    // Another visible pane showing the same issue in the same column: its popup id is not the open one, so
    // its BeginDialog would fail. It leaves the open alone while the owning pane still hosts the popup, and
    // ends it once that pane has stopped (closed, hidden, or the row left its grid), so no open outlives
    // every pane that could show it.
    if (!s_ActiveWorklogState.OwnerPaneId.empty() && s_ActiveWorklogState.OwnerPaneId != DrawingPaneId()) {
        if (!OpenIsSeen()) {
            EndWorklogOpen();
        }
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_Always);
    if (s_ActiveWorklogState.JustOpened) {
        ImGui::OpenPopup("TimeTrackingPopup");
        s_ActiveWorklogState.JustOpened = false;
    }
    if (!smatchet::ui::BeginDialog(s_ActiveWorklogState.Dialog, "TimeTrackingPopup", nullptr,
                                   ImGuiWindowFlags_AlwaysAutoResize)) {
        EndWorklogOpen();
        return;
    }
    s_ActiveWorklogState.HostedFrame = ImGui::GetFrameCount();
    // The target is latched here (OpenWorklogDialog has no controller), on the first draw that hosts the
    // popup, so the kept entry is back before the inputs read it in the same frame.
    LatchWorklogTarget(mutations);

    // Pillar 2 (#2043): the async submit's success post-back can only ask for the close; the
    // actual CloseCurrentPopup must happen inside the popup's own draw scope.
    if (s_ActiveWorklogState.CloseRequested) {
        s_ActiveWorklogState.CloseRequested = false;
        EndWorklogOpen();
        ImGui::CloseCurrentPopup();
        smatchet::ui::EndDialog(s_ActiveWorklogState.Dialog);
        return;
    }

    const WorklogKeys keys = ReadWorklogKeys();

    ImGui::Text("Time tracking: %s", ticket.id.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    // Logged & Remaining progress bar
    DrawWorklogProgressBar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Inputs
    DrawWorklogDraftNotice();
    const bool spentEnter = DrawWorklogTimeInputs();

    ImGui::Spacing();
    ImGui::Text("Date started *");
    ImGui::SetNextItemWidth(-FLT_MIN);
    TrackerDateTimeFieldEditor::RenderGenericDatePicker("##WorklogDateStarted", s_ActiveWorklogState.DateStarted, true);

    ImGui::Spacing();
    DrawWorklogDescription();

    if (!s_ActiveWorklogState.ErrorMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "%s", s_ActiveWorklogState.ErrorMsg.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    DrawWorklogButtons(threading, mutations, keys, spentEnter);
    SyncWorklogDraft();

    smatchet::ui::EndDialog(s_ActiveWorklogState.Dialog);
}

#if defined(SMATCHET_BUILD_UI_TESTS)
WorklogDialogTestSnapshot GetWorklogDialogSnapshotForTests() {
    WorklogDialogTestSnapshot snap;
    snap.Active = s_ActiveWorklogState.Initialized;
    snap.SubmitInFlight = s_ActiveWorklogState.SubmitInFlight;
    snap.TimeSpentActive = snap.Active && s_WorklogTimeSpentActiveForTests;
    snap.TimeSpent = s_ActiveWorklogState.TimeSpent;
    snap.Description = s_ActiveWorklogState.WorkDescription;
    snap.TimeRemaining = s_ActiveWorklogState.TimeRemaining;
    snap.TimeRemainingManuallyEdited = s_ActiveWorklogState.TimeRemainingManuallyEdited;
    snap.ErrorMsg = s_ActiveWorklogState.ErrorMsg;
    snap.DraftRestored = s_ActiveWorklogState.DraftRestored;
    return snap;
}

void RequestCloseWorklogDialogForTests() {
    if (s_ActiveWorklogState.Initialized) {
        s_ActiveWorklogState.CloseRequested = true;
    }
}
#endif // SMATCHET_BUILD_UI_TESTS

} // namespace TicketFieldEditorWorklog
