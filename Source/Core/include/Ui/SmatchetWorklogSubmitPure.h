#pragma once

#include "CompactDateFormat.h"
#include "TrackerFieldValueParser.h"

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

} // namespace worklog
} // namespace smatchet
