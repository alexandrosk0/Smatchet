#pragma once

// JqlChangedSincePure — pure query-window helpers for the ticket-change monitor
// (ticket-change-monitor plan, S1a). The monitor narrows each change-probe to issues
// touched since the last poll so an idle poll is near-empty server-side:
//   * WrapJqlChangedWithin — wraps a view's base JQL in a relative `updated >= -Nm`
//     window (the Jira path). A trailing `ORDER BY` clause is repositioned after the
//     AND so the result stays valid JQL.
//   * IsoSinceFromWindow — formats an absolute ISO-8601 UTC `since` timestamp (the
//     GitHub `since=` / Plane `updated_at` path) for `nowUnix - windowSeconds`.
// Extracted as free functions (PaneSyncKickPolicy.h precedent) so the window math is
// unit-testable without a backend. Backend-agnostic: no JQL grammar leaks past the
// Jira-specific WrapJqlChangedWithin (kept here, not on a role interface, like JqlEscape).

#include <chrono>
#include <cstdint>
#include <string>

namespace smatchet {

/// Whole-minute lookback for a `std::chrono::seconds` change-monitor window, for the Jira relative
/// `updated >= -Nm` clause. Rounds UP (ceil) so a sub-minute remainder never drops a change, and
/// clamps to >= 1 (a zero/negative window still probes the last minute). e.g. 180s → 3, 121s → 3,
/// 60s → 1, 30s → 1, 0s → 1.
int ChangedSinceWindowMinutes(std::chrono::seconds window);

/// Wrap `baseJql` so it additionally matches only issues updated within the last
/// `minutes` minutes: `(<base>) AND updated >= -Nm`. `minutes` is clamped to >= 1.
/// An empty/blank `baseJql` yields the bare `updated >= -Nm` clause. A trailing
/// `ORDER BY ...` in `baseJql` is moved to the end of the result (Jira rejects an
/// ORDER BY inside parentheses), so `proj = X ORDER BY created` becomes
/// `(proj = X) AND updated >= -Nm ORDER BY created`.
std::string WrapJqlChangedWithin(const std::string& baseJql, int minutes);

/// ISO-8601 UTC ("YYYY-MM-DDTHH:MM:SSZ") timestamp for `nowUnix - windowSeconds`,
/// for backends whose change query takes an absolute `since` (GitHub issues `since=`,
/// Plane `updated_at`). `windowSeconds` is treated as 0 if negative; the result is
/// clamped at the Unix epoch (never negative). Pure integer civil-date math — no
/// locale, no timezone database, thread-safe (no gmtime/localtime).
std::string IsoSinceFromWindow(std::int64_t nowUnix, std::int64_t windowSeconds);

} // namespace smatchet
