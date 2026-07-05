#ifndef SMATCHET_TICKET_GRID_DURATION_SORT_PURE_H
#define SMATCHET_TICKET_GRID_DURATION_SORT_PURE_H

#include <string>

/**
 * Pure duration-sort helpers lifted from TicketGridModel.cpp's anonymous namespace so the
 * time-tracking sort path can be unit-tested without linking the grid model's heavy
 * transitive dependencies (TrackerGridFieldDisplay, TrackerDateTimeFieldEditor, ...).
 *
 * Implementations are byte-identical to the originals; only namespace + linkage changed.
 * These run inside a UI-thread sort comparator over tracker-supplied (untrusted) cell
 * values, so they must terminate on ANY input and never overflow: the unit-by-unit parse
 * always advances its cursor (CPP_CODE_AUDIT.md #3) and accumulation saturates at
 * kMaxDurationSeconds instead of overflowing long long (CPP_CODE_AUDIT.md #19).
 *
 * Owner: TicketGridModel (grid sort) — pure helpers live here.
 * Test surface: tests/Core/TicketGridDurationSortPure.test.cpp.
 */
namespace TicketGridDurationSortPure {

/// Saturation ceiling for parsed durations — a hostile magnitude sorts as "very large"
/// rather than overflowing. Exposed for tests.
long long MaxDurationSeconds();

/**
 * Parse a Jira-style time-tracking cell value into seconds for sort comparison.
 * Whole-integer strings ("3600") are plain seconds; otherwise the unit-by-unit parse
 * handles "3h 30m", "1w 2d", "2.5h", ... with w/d/h/m suffixes (Jira week = 5d, day = 8h).
 * Unrecognized characters are skipped without stalling; magnitudes saturate. Empty or
 * unparsable input returns 0.
 */
long long ParseDurationToSecondsForSort(const std::string& input);

/// Time-tracking duration compare (e.g. "2d 4h" vs "3600"): -1 / 0 / 1 on parsed seconds.
int CompareTimeTrackingValues(const std::string& aVal, const std::string& bVal);

} // namespace TicketGridDurationSortPure

#endif
