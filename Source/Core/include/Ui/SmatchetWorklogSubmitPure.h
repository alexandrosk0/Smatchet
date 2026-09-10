#pragma once

#include "CompactDateFormat.h"
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

/// Validate the two required worklog inputs. Returns an empty string when the submit may
/// proceed, otherwise the user-facing error to show in the dialog. Wording is the dialog's
/// historical text, preserved verbatim through the async flip.
inline std::string ValidateWorklogSubmission(const std::string& timeSpent, const std::string& dateStarted) {
    if (ParseWorkDurationToSeconds(timeSpent) <= 0) {
        return "Invalid Time spent format. Please use e.g. 2h 30m.";
    }
    ParsedJiraDateTime parsed;
    if (!TryParseJiraDateTime(dateStarted, parsed)) {
        return "Invalid Date started format.";
    }
    return std::string();
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

} // namespace worklog
} // namespace smatchet
