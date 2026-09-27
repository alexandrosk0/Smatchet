#include "TicketFieldEditor_Worklog.h"

// The time-tracking (worklog) dialog of the ticket grid, split out of TicketFieldEditor.cpp: seeding,
// the logged/remaining bar, the duration and date inputs, and Save on a worker. Reaches the controller
// only through the IAppThreading + IAppTicketMutations facets.

// SMATCHET_DEVIATION(rule=duplication; reason=include overlap with sibling UI TU; owner=ui; revisit=dup-scoping)
#include "TicketFieldEditor_detail.h"
#include "CachedTicketTypes.h"
#include "Commands/IAppThreading.h"
#include "CompactDateFormat.h"
#include "Interfaces/IAppTicketMutations.h"
#include "SmatchetLocalization.h"
#include "TrackerDateTimeFieldEditor.h"
#include "TrackerFieldValueParser.h"
#include "TrackerFieldValueUtils.h"
#include "TrackerGridFieldDisplay.h"
#include "Ui/SmatchetCommentsModalGenPure.h"
#include "Ui/SmatchetToast.h"
#include "Ui/SmatchetWorklogSubmitPure.h"

#include "imgui.h"
#include "SmatchetLocalizedImGui.h"
// Routes all ImGui::* calls in this TU through the localization/wrapper namespace.
#define ImGui SmatchetLocalizedImGui

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
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
};

static ActiveWorklogDialogState s_ActiveWorklogState;

/// Monotonic submit-generation counter, deliberately OUTSIDE the dialog state so it survives
/// the per-open reset (the #1713 lesson: a counter reset with the state makes the guard inert).
static int s_WorklogGenCounter = 0;

/// Issue ids whose worklog POST is still outstanding; the set is empty when none is. Also outside
/// the dialog state, and for the same reason: `SubmitInFlight` is reset on every open, so tracking
/// the POST there let Save → Cancel → re-open the SAME ticket enable Save again while the first
/// request was still running — two worklogs for one intent. The POST cannot be cancelled (`AddWorklog`
/// takes no cancel token), so Cancel closes the dialog while the request keeps running; these ids
/// are what keep that fact visible. A SET, not one slot (#2167): submits on different tickets
/// overlap, and a single slot let Save on B overwrite A's latch so re-opening A re-enabled Save
/// mid-POST — the same duplicate class, reached by a two-ticket interleaving. Each post-back
/// erases its OWN id UNCONDITIONALLY, before the stale-guard, so a cancelled dialog cannot strand
/// a latch and lock its ticket out of Save forever, and a late post-back cannot clear another
/// ticket's still-outstanding latch.
static smatchet::worklog::WorklogSubmitInFlightSet s_WorklogSubmitInFlightIssueIds;

// Computes the progress-bar logged/remaining seconds for the worklog dialog. Pure arithmetic on
// the parsed durations — extracted so the modal draw body stays layout-focused. Mirrors the
// auto-deduction (spent reduces remaining) unless the user manually edited the remaining field.
void ComputeWorklogProgressSeconds(long long& outDisplaySpentSec, long long& outDisplayRemSec) {
    const long long spentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeSpent);
    const long long remSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TotalTimeRemaining);
    const long long newSpentSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);

    outDisplaySpentSec = spentSec + newSpentSec;
    outDisplayRemSec = remSec;
    if (newSpentSec > 0 && !s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = (std::max)(0LL, remSec - newSpentSec);
    } else if (s_ActiveWorklogState.TimeRemainingManuallyEdited) {
        outDisplayRemSec = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeRemaining);
    }
}

// Validates the worklog inputs and, on success, DISPATCHES the submit onto a background worker.
// Sets s_ActiveWorklogState.ErrorMsg on validation failure (synchronously) or on submit failure
// (from the worker's post-back).
//
// Pillar 2 (#2043): `AppController::SubmitWorklog` does a `ConfigManager::Load()` plus a blocking
// Jira `AddWorklog` POST. Running that inline froze the render thread for the whole tracker
// round-trip with no cue — on a hung tracker the app looked dead. It now runs on
// `LaunchBackgroundTask` and applies its `VoidResult` via `PostToMainThread`, exactly like the
// comments-modal post path (SmatchetCommentsModalUi.cpp).
//
// Lifetime: `AppController` is app-lifetime and `~AppController` joins every
// LaunchBackgroundTask worker (JoinBackgroundTasks) before member teardown, while
// PostToMainThread no-ops after BeginShutdown() — so the captured facet pointers (both are the controller) can never
// dangle and a post-back dropped at exit is harmless. Every other capture is a by-value copy; the lambda touches only
// the file-static dialog state, which outlives the controller.
void HandleWorklogSave(IAppThreading& threading, IAppTicketMutations& mutations) {
    // Two gates, not one: the dialog's own flag AND the cross-instance in-flight id set. The second
    // is what stops Save → Cancel → re-open → Save creating two worklogs for one intent.
    if (!smatchet::worklog::CanSubmitWorklog(s_ActiveWorklogState.SubmitInFlight) ||
        smatchet::worklog::WorklogSubmitOutstandingFor(s_WorklogSubmitInFlightIssueIds, s_ActiveWorklogState.IssueId)) {
        return; // a submit is already in flight — never queue a duplicate worklog
    }
    s_ActiveWorklogState.ErrorMsg.clear();
    const std::string validationError =
        smatchet::worklog::ValidateWorklogSubmission(s_ActiveWorklogState.TimeSpent, s_ActiveWorklogState.DateStarted);
    if (!validationError.empty()) {
        s_ActiveWorklogState.ErrorMsg = validationError;
        return;
    }

    IAppThreading* threadingPtr = &threading;
    IAppTicketMutations* mutationsPtr = &mutations;
    const std::string issueId = s_ActiveWorklogState.IssueId;
    const std::string timeSpent = s_ActiveWorklogState.TimeSpent;
    const std::string timeRemaining = s_ActiveWorklogState.TimeRemaining;
    const std::string adjEst = s_ActiveWorklogState.TimeRemainingManuallyEdited ? "new" : "auto";
    const std::string description = s_ActiveWorklogState.WorkDescription;
    const std::string startedDate = s_ActiveWorklogState.DateStarted;

    const int gen = SmatchetCommentsModalGen::AllocGen(s_WorklogGenCounter);
    s_ActiveWorklogState.Gen = gen;
    s_ActiveWorklogState.SubmitInFlight = true;
    smatchet::worklog::MarkWorklogSubmitInFlight(s_WorklogSubmitInFlightIssueIds, issueId);

    threading.LaunchBackgroundTask([threadingPtr, mutationsPtr, gen, issueId, timeSpent, timeRemaining, adjEst,
                                    description, startedDate]() {
        const VoidResult worklogResult =
            mutationsPtr->SubmitWorklog(issueId, timeSpent, timeRemaining, adjEst, description, startedDate);
        const bool ok = worklogResult.has_value();
        const std::string err = ok ? std::string() : worklogResult.error();
        threadingPtr->PostToMainThread([gen, issueId, ok, err]() {
            // Release this submit's own latch FIRST, before any early-out. If the stale-guard
            // below returned with it still set, the ticket could never be Saved again for the
            // rest of the session. Erasing only THIS id leaves any other ticket's outstanding
            // submit latched.
            smatchet::worklog::ClearWorklogSubmitInFlight(s_WorklogSubmitInFlightIssueIds, issueId);
            // Reuse of the comments-modal stale-guard (#1713): the dialog closed, moved to another
            // ticket, or a newer submit superseded this one — so nothing may be written into the
            // dialog state. The OUTCOME still has to reach the user: the request was sent and it
            // either created a worklog or failed, and Cancel does not (cannot) undo it. Reporting
            // it as a toast is the difference between "cancelled, nothing happened" and a silent
            // success/failure the user never learns about.
            if (SmatchetCommentsModalGen::CallbackIsStale(s_ActiveWorklogState.Initialized, s_ActiveWorklogState.Gen,
                                                          gen, s_ActiveWorklogState.IssueId, issueId)) {
                // Stale because the SAME ticket was re-opened mid-POST (#2168): that open seeded
                // SubmitInFlight from the latch released above, so the re-opened dialog would
                // otherwise keep Save disabled forever. The seed stood for exactly this POST.
                if (smatchet::worklog::StalePostBackReleasesDialogSubmit(s_ActiveWorklogState.Initialized,
                                                                         s_ActiveWorklogState.IssueId, issueId)) {
                    s_ActiveWorklogState.SubmitInFlight = false;
                }
                if (ok) {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("worklog.toast.saved_title", "Worklog saved"), issueId,
                        ToastType::Success);
                } else {
                    SmatchetToastManager::Instance().Push(
                        SmatchetLocalization::T("worklog.toast.failed_title", "Worklog failed"), issueId + ": " + err,
                        ToastType::Error);
                }
                return;
            }
            s_ActiveWorklogState.SubmitInFlight = false;
            if (ok) {
                // CloseCurrentPopup() is only legal inside the popup's own draw — request the
                // close and let RenderTimeTrackingModal honour it on this frame's draw.
                s_ActiveWorklogState.CloseRequested = true;
            } else {
                // In-dialog message is the primary cue (the dialog is open and showing it); the
                // toast is deliberately NOT duplicated here.
                s_ActiveWorklogState.ErrorMsg = "Failed: " + err;
            }
        });
    });
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

// Time-spent + time-remaining duration inputs, including the auto-decrement of remaining.
void DrawWorklogTimeInputs() {
    ImGui::Text("Time spent *");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (DrawDurationFieldWithSuggestions("##WorklogTimeSpent", s_ActiveWorklogState.TimeSpent,
                                         sizeof(s_ActiveWorklogState.TimeSpent), 0, nullptr, nullptr, nullptr, false)) {
        if (!s_ActiveWorklogState.TimeRemainingManuallyEdited) {
            long long spentVal = ParseWorkDurationToSeconds(s_ActiveWorklogState.TimeSpent);
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
    ImGui::TextDisabled("Use the format: 2w 4d 6h 45m (w=weeks, d=days, h=hours, m=minutes)");

    ImGui::Spacing();
    ImGui::Text("Time remaining");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (DrawDurationFieldWithSuggestions("##WorklogTimeRemaining", s_ActiveWorklogState.TimeRemaining,
                                         sizeof(s_ActiveWorklogState.TimeRemaining), 0, nullptr, nullptr,
                                         &s_ActiveWorklogState.TimeRemainingManuallyEdited, false)) {
        // value changed!
    }
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
    ImGui::InputTextMultiline("##WorklogDesc", s_ActiveWorklogState.WorkDescription,
                              sizeof(s_ActiveWorklogState.WorkDescription),
                              ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 4));
}

} // namespace

namespace TicketFieldEditorWorklog {

// Primes the time-tracking dialog for `ticket`, owned by the cell whose column is `ownerFieldId`
// (the only cell allowed to draw the modal — see ActiveWorklogDialogState::OwnerFieldId). Shared
// by the `timespent` and `worklog` cells so both entry points open an identically-seeded dialog.
void OpenWorklogDialog(const CachedTicket& ticket, const std::string& ownerFieldId) {
    s_ActiveWorklogState.IssueId = ticket.id;
    s_ActiveWorklogState.OwnerFieldId = ownerFieldId;
    s_ActiveWorklogState.TimeSpent[0] = '\0';
    s_ActiveWorklogState.OriginalEstimate = ticket.GetFieldValue("timeoriginalestimate");
    s_ActiveWorklogState.TotalTimeSpent = ticket.GetFieldValue("timespent");
    s_ActiveWorklogState.TotalTimeRemaining = ticket.GetFieldValue("timeestimate");
    std::strncpy(s_ActiveWorklogState.TimeRemaining, s_ActiveWorklogState.TotalTimeRemaining.c_str(),
                 sizeof(s_ActiveWorklogState.TimeRemaining) - 1);
    s_ActiveWorklogState.TimeRemaining[sizeof(s_ActiveWorklogState.TimeRemaining) - 1] = '\0';

    s_ActiveWorklogState.DateStarted = GetCurrentJiraDateTimeString();

    s_ActiveWorklogState.WorkDescription[0] = '\0';
    s_ActiveWorklogState.ErrorMsg.clear();
    s_ActiveWorklogState.Initialized = true;
    s_ActiveWorklogState.JustOpened = true;
    s_ActiveWorklogState.TimeRemainingManuallyEdited = false;
    // NOT an unconditional false: if this ticket's previous submit is still running (the user hit
    // Save then Cancel, then re-opened), the dialog must re-open in the in-flight state so Save
    // stays disabled and the "Saving worklog..." cue shows. Otherwise the second Save creates a
    // duplicate worklog for one intent (#2085 review). Seeding it HERE rather than at the button
    // means the `worklog` cell entry point added by #2088 gets the same guard for free.
    s_ActiveWorklogState.SubmitInFlight =
        smatchet::worklog::WorklogSubmitOutstandingFor(s_WorklogSubmitInFlightIssueIds, ticket.id);
    s_ActiveWorklogState.CloseRequested = false;
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
    ImGui::SetNextWindowSize(ImVec2(450.0f, 0.0f), ImGuiCond_Always);
    if (s_ActiveWorklogState.JustOpened) {
        ImGui::OpenPopup("TimeTrackingPopup");
        s_ActiveWorklogState.JustOpened = false;
    }

    if (!ImGui::BeginPopupModal("TimeTrackingPopup", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        s_ActiveWorklogState.Initialized = false;
        return;
    }

    // Pillar 2 (#2043): the async submit's success post-back can only ask for the close; the
    // actual CloseCurrentPopup must happen inside the popup's own draw scope.
    if (s_ActiveWorklogState.CloseRequested) {
        s_ActiveWorklogState.CloseRequested = false;
        s_ActiveWorklogState.Initialized = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Time tracking: %s", ticket.id.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    // Logged & Remaining progress bar
    DrawWorklogProgressBar();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Inputs
    DrawWorklogTimeInputs();

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

    // Buttons. Pillar 2 (#2043): while the worklog POST is in flight the Save button is
    // disabled and a visible "Saving worklog..." cue is drawn beside it — the dialog stays
    // interactive (the whole app used to freeze here) and the user can see work is happening.
    // `wasInFlight` is latched BEFORE the Save button so BeginDisabled/EndDisabled stay paired
    // across the click that flips the flag; the cue itself re-reads the LIVE flag afterwards so
    // it appears on the very frame Save dispatched (cue-before-stall ordering — a snapshot here
    // would delay it by one frame). The labelled id is the bucket-E visible-cue anchor.
    const bool wasInFlight = s_ActiveWorklogState.SubmitInFlight;
    if (wasInFlight) {
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        HandleWorklogSave(threading, mutations);
    }
    if (wasInFlight) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        ImGui::CloseCurrentPopup();
        s_ActiveWorklogState.Initialized = false;
    }
    if (wasInFlight && ImGui::IsItemHovered()) {
        // Be honest about what Cancel does mid-submit: AddWorklog takes no cancel token, so the
        // request is already gone. Closing the dialog does not un-send it; the outcome arrives
        // as a toast instead.
        ImGui::SetTooltip("%s", SmatchetLocalization::T("worklog.cancel_inflight_tooltip",
                                                        "The worklog request was already sent — closing this dialog "
                                                        "won't cancel it. The result will appear as a notification."));
    }
    if (s_ActiveWorklogState.SubmitInFlight) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", SmatchetLocalization::T("worklog.saving", "Saving worklog..."));
    }

    ImGui::EndPopup();
}

} // namespace TicketFieldEditorWorklog
