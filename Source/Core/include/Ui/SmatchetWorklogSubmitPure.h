#pragma once

#include "CompactDateFormat.h"
#include "TrackerFieldValueParseExplain.h"
#include "TrackerFieldValueParser.h"

#include <set>
#include <string>

/// Pure, unit-testable half of the worklog dialog's Save (#2043).
/// `HandleWorklogSave` used to validate the inputs and then run `AppController::SubmitWorklog`
/// — a `ConfigManager::Load()` plus a blocking Jira `AddWorklog` POST — inline on the ImGui
/// render path, so the whole UI froze for the tracker round-trip with no progress cue. The
/// submit half now runs on a `LaunchBackgroundTask` worker and reports back through
/// `PostToMainThread`; the validation half below stays synchronous (it is pure string parsing,
/// no I/O) and is extracted here so the accept/reject decision is bucket-A testable without
/// ImGui or an AppController.
namespace smatchet {
namespace worklog {

/// A duration the user typed into the dialog, read the tolerant way (INPUT_UX_AUDIT.md #38): "1.5h", "90 m",
/// "1h30", "1:30". A number with no unit is minutes, Jira's default.
inline TrackerParseExplain::DurationParse ParseTypedDuration(const std::string& text) {
    return TrackerParseExplain::ParseDurationInput(text, TrackerParseExplain::BareNumberUnit::Minutes);
}

/// Seconds in a typed duration; 0 when it is empty or does not parse (the progress bar shows nothing for it).
inline long long TypedDurationSeconds(const std::string& text) {
    const TrackerParseExplain::DurationParse parsed = ParseTypedDuration(text);
    return parsed.Ok ? parsed.Seconds : 0;
}

/// Why `timeSpent` cannot be logged, or a message with no error when it can. Time spent must be above zero:
/// a blank or "0m" gets "enter a time above zero", anything unreadable gets the parser's own message.
inline TrackerParseExplain::ParseMessage TimeSpentProblem(const std::string& timeSpent) {
    const TrackerParseExplain::DurationParse parsed = ParseTypedDuration(timeSpent);
    if (parsed.Ok && parsed.Seconds > 0) {
        return TrackerParseExplain::ParseMessage();
    }
    if (parsed.Ok || parsed.Empty) {
        TrackerParseExplain::ParseMessage m;
        m.Id = TrackerParseExplain::ParseMessageId::DurationRequiredPositive;
        return m;
    }
    return parsed.Message;
}

/// Why `timeRemaining` cannot be sent, or no error. Blank is fine (the tracker keeps its own figure) and so is
/// zero: "nothing left".
inline TrackerParseExplain::ParseMessage TimeRemainingProblem(const std::string& timeRemaining) {
    const TrackerParseExplain::DurationParse parsed = ParseTypedDuration(timeRemaining);
    return (parsed.Ok || parsed.Empty) ? TrackerParseExplain::ParseMessage() : parsed.Message;
}

/// The text to send for a typed duration: its canonical form ("1h 30m") when it reads, the text untouched
/// otherwise (the validation above has already refused what must not go out).
inline std::string CanonicalDurationText(const std::string& typed) {
    const TrackerParseExplain::DurationParse parsed = ParseTypedDuration(typed);
    return parsed.Ok ? parsed.Canonical : typed;
}

/// Validate the worklog inputs. Returns an empty string when the submit may proceed, otherwise the
/// user-facing error to show in the dialog: the same text the field shows under itself while typing.
inline std::string ValidateWorklogSubmission(const std::string& timeSpent, const std::string& dateStarted) {
    const TrackerParseExplain::ParseMessage problem = TimeSpentProblem(timeSpent);
    if (problem.IsError()) {
        return TrackerParseExplain::DescribeParseMessage(problem);
    }
    ParsedJiraDateTime parsed;
    if (!TryParseJiraDateTime(dateStarted, parsed)) {
        return "Invalid Date started format.";
    }
    return std::string();
}

/// After a save asked for from the keyboard (Enter in Time spent, or Ctrl+Enter anywhere): true when
/// Time spent was turned down (nothing went out and it does not parse), so the caret goes into that
/// field to fix it instead of leaving the user to click.
inline bool ShouldRefocusTimeSpentAfterSaveKey(bool saveKeyPressed, bool submitInFlight, const std::string& timeSpent) {
    return saveKeyPressed && !submitInFlight && TimeSpentProblem(timeSpent).IsError();
}

/// Whether the dialog's Save button may fire. False while a submit is already in flight, so a
/// second click cannot queue a duplicate worklog against the same round-trip (the button is
/// also drawn disabled with a "Saving..." cue — this is the belt-and-braces guard).
inline bool CanSubmitWorklog(bool submitInFlight) { return !submitInFlight; }

/// The set of issue ids whose worklog POST is still outstanding. A SET, not a single id: the
/// submits are per-ticket and can overlap, so one slot let ticket B's Save overwrite ticket A's
/// outstanding latch and re-enable Save on A mid-POST — a second worklog for one intent, the
/// #2085 duplicate class reopened by a two-ticket interleaving (#2167).
typedef std::set<std::string> WorklogSubmitInFlightSet;

/// Whether a worklog POST for `issueId` is still outstanding. This is deliberately NOT the
/// dialog's own `SubmitInFlight` flag: that flag is per-dialog-instance and is reset on every
/// open, so Save → Cancel → re-open the SAME ticket used to re-enable Save while the first POST
/// was still running and let the user create a SECOND worklog for one intent. The POST itself
/// cannot be cancelled (`AddWorklog` takes no cancel token), so the honest behaviour is to keep
/// the submit visible across dialog instances rather than pretend Cancel undid it.
inline bool WorklogSubmitOutstandingFor(const WorklogSubmitInFlightSet& inFlightIssueIds, const std::string& issueId) {
    // An empty id never matches: a dialog opened before its id is populated must not come up
    // spuriously disabled (and an empty id is never inserted, so it cannot be outstanding).
    return !issueId.empty() && inFlightIssueIds.find(issueId) != inFlightIssueIds.end();
}

/// Record that `issueId`'s worklog POST has been dispatched. Additive — an outstanding submit on
/// another ticket is left alone, which is the whole point of the set.
inline void MarkWorklogSubmitInFlight(WorklogSubmitInFlightSet& inFlightIssueIds, const std::string& issueId) {
    if (!issueId.empty()) {
        inFlightIssueIds.insert(issueId);
    }
}

/// Release `issueId`'s latch when its post-back lands. Each submit erases its OWN id, so a late
/// post-back can neither strand another ticket's latch nor clear it.
inline void ClearWorklogSubmitInFlight(WorklogSubmitInFlightSet& inFlightIssueIds, const std::string& issueId) {
    inFlightIssueIds.erase(issueId);
}

/// Whether a post-back the stale-guard rejected must still release the OPEN dialog's own
/// `SubmitInFlight` flag (#2168). The stale-guard fires on a generation mismatch, and one way
/// to get that mismatch is re-opening the SAME ticket while its POST is running: the open
/// seeds `SubmitInFlight = true` from the cross-instance latch and burns a fresh generation, so
/// when the POST lands its generation no longer matches and the normal release never runs —
/// Save stays disabled behind a perpetual "Saving worklog..." cue, which after a FAILED POST
/// blocks the user's retry until the dialog is closed and re-opened. The seed represented
/// exactly the POST that just completed, so releasing it is correct; a dialog on a different
/// ticket (or no dialog at all) is left alone.
inline bool StalePostBackReleasesDialogSubmit(bool dialogInitialized, const std::string& dialogIssueId,
                                              const std::string& postBackIssueId) {
    return dialogInitialized && !postBackIssueId.empty() && dialogIssueId == postBackIssueId;
}

/// The Time remaining text the Log work draft keeps: what the user typed, and only while they edited the
/// field themselves. The value derived from the spent time is recomputed at every open, so keeping it would
/// pin an estimate that is stale by then. Empty means keep nothing (the draft store erases on empty text).
inline std::string WorklogRemainingDraftText(bool manuallyEdited, const std::string& timeRemaining) {
    return manuallyEdited ? timeRemaining : std::string();
}

} // namespace worklog
} // namespace smatchet
