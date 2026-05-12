#pragma once

#include <string>

class ITrackerClient;

namespace smatchet {

/** Resolve the project key for a new draft / bulk-import row.
 *
 *  Resolution order (Jira):
 *    1. client->ExtractProjectFromQuery(activeViewQuery) — JQL `project = …` scope
 *    2. lastVisibleTicketId key prefix — "TICKET-123" → "TICKET"
 *    3. legacyFallback — `cfg.ProjectKey` today; "" after PR 6.
 *
 *  For Plane the key-prefix step is skipped (Plane IDs are UUIDs with no project prefix);
 *  resolution is (1) → (3).
 *
 *  Pure C++14; no I/O. `client` may be null — then steps 1 (and the Plane-detect for step 2)
 *  degrade to "skip" and we fall straight to the prefix / legacy fallback.
 *
 *  Added in PR 2 of docs/design/remove-global-project-key.md.
 */
std::string ResolveProjectForDraft(const ITrackerClient* client,
                                   const std::string& activeViewQuery,
                                   const std::string& lastVisibleTicketId,
                                   const std::string& legacyFallback);

} // namespace smatchet
