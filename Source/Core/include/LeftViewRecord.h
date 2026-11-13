#pragma once

// LeftViewRecord — one row of the history of issues that left a view: what the `left_view_history` table stores
// and the window shows. Plain data, no dependencies, so Persistence, Sync and Ui can all include it.

#include <cstdint>
#include <string>

namespace smatchet {

enum class LeftViewKind { LeftView = 0, Deleted = 1 };

/// Stored as an integer: append only.
enum class LeftViewReasonKind {
    Deleted = 0,           ///< Gone from the tracker.
    QueryFieldChanged = 1, ///< A field the view's query names changed.
    OtherFieldChanged = 2, ///< Only fields the query does not name changed: possibly related.
    NoChangeFound = 3,     ///< The issue was read and nothing differs.
    Unavailable = 4,       ///< The issue could not be read.
    NotChecked = 5,        ///< Over the per-batch lookup limit, so no request was spent on it.
    Pending = 6,           ///< Recorded; the lookup has not finished.
    NotVisible = 7,        ///< The tracker answered without it: deleted, or no longer readable by this account.
};

struct LeftViewRecord {
    std::int64_t id = 0; ///< Row id; 0 until stored.
    std::string backendKey;
    std::string paneId;
    std::string viewId;
    std::string viewName;
    std::string query; ///< The query the view ran when the issue left it.
    std::string issueKey;
    std::string summary;
    LeftViewKind kind = LeftViewKind::LeftView;
    LeftViewReasonKind reasonKind = LeftViewReasonKind::Pending;
    std::string reason;
    std::string reasonDetailJson; ///< ReasonDetailToJson of the explaining changes.
    std::string author;
    std::string browseUrl;
    std::string lastKnownJson; ///< LastKnownFieldsToJson of the row as the pane last held it.
    std::int64_t leftAtUnixSec = 0;
    /// When the view last listed the issue: it left between this and `leftAtUnixSec`. A gap is normal for a
    /// departure first noticed by the reload after a launch. 0 when unknown.
    std::int64_t listedAtUnixSec = 0;
};

} // namespace smatchet
