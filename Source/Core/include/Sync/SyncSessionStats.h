#pragma once

// SyncSessionStats — what one streaming-sync session cost (docs/plans/active/delta-sync.md
// § Phase 1). Filled by TicketSyncService at session end, logged once, and exposed read-only
// through the `sync.stats` command so a reload's request count, bytes and fallback reason can be
// checked without reading logs. Plain data: header-only, no OS or SQLite headers (C++14, builds in
// the standalone and DX12 targets).

#include <cstddef>
#include <cstdint>
#include <string>

namespace smatchet {
namespace sync {

/// Tracker HTTP traffic counters at one instant (monotonic since process start). A session's cost
/// is the difference of two samples; other panes' requests overlapping the session are included,
/// so the figure is an upper bound when several panes sync at once.
struct TrackerTrafficSample {
    std::uint64_t Requests = 0;
    std::uint64_t UploadBytes = 0;
    std::uint64_t DownloadBytes = 0;
};

struct SyncSessionStats {
    /// True once a session has finished (or failed) at least once; false for a default record.
    bool Valid = false;
    std::uint64_t RequestId = 0;
    /// "full" or "delta" - the strategy the session ran to completion (a delta that fell back
    /// reports "full" with `FallbackReason` set).
    std::string Mode;
    /// Keys the roster listed (delta only; 0 for a full fetch).
    std::size_t RosterSize = 0;
    /// Ticket bodies downloaded this session (everything the tracker sent as full rows).
    std::size_t BodiesFetched = 0;
    /// Rows served from the cache because they had not changed (delta only).
    std::size_t Unchanged = 0;
    std::uint64_t Requests = 0;
    std::uint64_t UploadBytes = 0;
    std::uint64_t DownloadBytes = 0;
    long long DurationMs = 0;
    /// Why the delta path was abandoned for a full fetch; empty when it was not.
    std::string FallbackReason;
    /// The session ended authoritative for its view (`FullSyncCompleted`).
    bool Completed = false;
    /// Non-empty when the session ended with a fetch error.
    std::string Error;
};

} // namespace sync
} // namespace smatchet
