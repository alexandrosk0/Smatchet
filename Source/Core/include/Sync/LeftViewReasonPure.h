#pragma once

// LeftViewReasonPure — why an issue left a view, worked out from what the tracker reports. Pure: no I/O, no
// Logger.h, no tracker client. See docs/plans/left-view-history-grid.md.
//
// The evidence is either the issue's history (dated and attributed) or a comparison of its last-known row with
// a fresh one. Changes to a field the view's query names explain the departure. A change to any other field is
// only possibly related, and the reason says so. No change found is reported as that, never guessed at.

#include "LeftViewRecord.h"
#include "TrackerFieldChange.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace smatchet {

struct LeftViewReason {
    LeftViewReasonKind kind = LeftViewReasonKind::NoChangeFound;
    std::string text;                        ///< One line for the grid.
    std::string author;                      ///< Who made the newest explaining change, empty when unknown.
    std::vector<TrackerFieldChange> changes; ///< The explaining changes, newest first.
};

/// True when `query` names the field as a whole word, by id or label, case-insensitively. `cf[10020]` matches
/// `customfield_10020`, and `statusCategory` matches the status field.
bool QueryNamesField(const std::string& query, const std::string& fieldId, const std::string& fieldLabel);

/// The fields that differ between a last-known row and a fresh one. Only fields both rows carry are compared,
/// because a row holds only the fields its fetch asked for. Bookkeeping fields (`updated`, comments, worklogs,
/// watch and vote counts, ...) never explain a departure and are skipped. Sorted by field id.
std::vector<TrackerFieldChange> DiffRowFields(const std::string& issueKey,
                                              const std::unordered_map<std::string, std::string>& lastKnown,
                                              const std::unordered_map<std::string, std::string>& fresh);

/// History entries newer than `sinceMs` (epoch ms; 0 keeps all), newest first. Compared as instants: a tracker
/// writes stamps in the user's offset. An entry at exactly the bound is what the last-known row already shows.
std::vector<TrackerFieldChange> ChangesSince(const std::vector<TrackerFieldChange>& history, std::int64_t sinceMs);

/// The reason for a departure the tracker confirmed still exists, from the changes found (history entries or a
/// row diff). `fieldLabels` maps field ids to display names and may be empty.
LeftViewReason BuildLeftViewReason(const std::vector<TrackerFieldChange>& changes, const std::string& query,
                                   const std::unordered_map<std::string, std::string>& fieldLabels);

LeftViewReason DeletedReason();
/// The request failed, so nothing is known about the issue.
LeftViewReason UnavailableReason();
/// The tracker answered without the issue, and did not say it is gone: it cannot be told which.
LeftViewReason NotVisibleReason();
LeftViewReason NotCheckedReason();

/// JSON for the row's last-known values: empty values skipped, each value and the whole payload capped so a
/// long description cannot bloat the table. Invalid UTF-8 is replaced, never thrown on.
std::string LastKnownFieldsToJson(const std::unordered_map<std::string, std::string>& fieldValues,
                                  std::size_t maxValueBytes = 2048, std::size_t maxTotalBytes = 65536);

/// Field id -> value from LastKnownFieldsToJson. Malformed input yields an empty map.
std::unordered_map<std::string, std::string> LastKnownFieldsFromJson(const std::string& json);

std::string ReasonDetailToJson(const std::vector<TrackerFieldChange>& changes);

} // namespace smatchet
