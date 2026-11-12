#pragma once

struct UiDrawSession;

namespace smatchet {
class LeftViewHistoryService;
}

// Issues that left a view: one dockable, newest-first grid across every pane and view, with the reason each one
// left. Reads the service's snapshot only; no SQLite or tracker call runs on this path. See cpp.
void SmatchetDrawLeftViewHistoryWindow(UiDrawSession& d, smatchet::LeftViewHistoryService& history);
