#pragma once

// TrackerRoster - the lightweight "which tickets are in this view, and at what version" answer a
// backend can give without downloading ticket bodies (docs/plans/active/delta-sync.md § Phase 3).
// A rank-0 leaf: the reader interface (ITrackerIssueReader.h) and the pure delta-sync rules
// (Sync/DeltaSyncPure.h) share it without either including the other. Backend-agnostic by design -
// a key and an opaque version stamp, no Jira types. Header-only, C++14, no platform headers.

#include <string>
#include <vector>

struct TrackerRosterEntry {
    /// Ticket key/id as the tracker names it.
    std::string Key;
    /// The tracker's last-updated stamp for the ticket, verbatim and opaque: it is only ever compared
    /// for equality with the stamp stored beside the cached row. Empty = the tracker did not say.
    std::string Updated;
};

struct TrackerRoster {
    std::vector<TrackerRosterEntry> Entries;
    /// True only when every page of the view was read (no page/size/count cap hit, no soft warning):
    /// the entries are then the WHOLE view, which is what lets the caller treat cached rows missing
    /// from it as gone. An incomplete roster still describes the rows it saw.
    bool Complete = false;
    /// Soft caveat from the walk (e.g. a pagination cap); non-empty implies !Complete.
    std::string Warning;
};
