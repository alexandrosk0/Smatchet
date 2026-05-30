#pragma once

#include <cstdint>
#include <string>

namespace smatchet {
namespace ai {

/**
 * Pure string-formatting helpers for the AI chat hover action row.
 *
 * Phase 1 of the Claude-Desktop-parity plan (docs/plans/shipped/ai-chat-claude-desktop-parity.md).
 * No ImGui dependency — TU is exercised by the doctest rig.
 *
 * All times are unix-epoch milliseconds (matches `std::chrono::system_clock`'s
 * `time_since_epoch().count()` when cast to milliseconds).
 */

/**
 * Wall-clock "now" in unix-epoch milliseconds.
 *
 * Wraps `std::chrono::system_clock::now()` so the three chat message-creation
 * sites (UI dispatchSend, controller's normal finalisation, controller's
 * cancel-with-partial path) all share one source of truth for what "created
 * at" means.
 */
std::int64_t NowUnixMs();

/**
 * Coarse relative-time bucket for a past instant.
 *
 * Buckets (boundary inclusive on the lower edge):
 *   delta <       60_000 ms  → "just now"
 *   delta <    3 600_000 ms  → "<N>m ago"    N = delta / 60_000
 *   delta <   86 400_000 ms  → "<N>h ago"    N = delta / 3_600_000
 *   delta ≥   86 400_000 ms  → "<N>d ago"    N = delta / 86_400_000
 *
 * If `thenUnixMs` is in the future (then > now) the function clamps to
 * "just now" — clock skew between a server-issued timestamp and the local
 * wall clock is the common cause; never emit "-3m ago".
 */
std::string FormatRelativeTime(std::int64_t nowUnixMs, std::int64_t thenUnixMs);

/**
 * Absolute local-time string in the fixed shape "HH:MM:SS DD-Mon-YYYY".
 *
 * Uses `std::localtime` — the result is in the host's local TZ.
 * `Mon` is the English three-letter abbreviation (Jan, Feb, ..., Dec).
 *
 * On parse / overflow failure returns an empty string (caller can fall back
 * to the relative form).
 */
std::string FormatAbsoluteTime(std::int64_t unixMs);

} // namespace ai
} // namespace smatchet
