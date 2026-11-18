#pragma once

// PaneCatalogScopePure — the project a grid pane's OWN field catalog is fetched for.
//
// The focused catalog scopes its fetch to the single project the active view's query names
// (ResolveViewProjectKeyForCatalog in AppController_CatalogAndFieldEdit.cpp). A grid pane's own catalog
// (AppController::populatePaneCatalogAfterSync_) is scoped the same way, from the view the pane's sync kick
// resolved for it (GridLiveContext::resolvedOwnView). Without a project, Jira skips createmeta enrichment and
// the pane's component and version dropdowns stay empty.
//
// The query is read with the pane's own backend parser (ITrackerConnectivity::ExtractProjectFromQuery), so a
// Jira pane parses JQL and a Plane pane its structured query. The parser is pure: no request is sent, which
// keeps this safe on the UI thread. Header-only.
//
// Plane note: the focused grid's own fetch resolves a Plane project further (ResolvePlaneOperationProject: the
// config query, then the workspace's sole project via ListProjects). Both are left out here: the config query
// carries the FOCUSED view's scope, and ListProjects is a network call. A Plane pane whose view names no
// project_id therefore fetches its own catalog unscoped.

#include "ITrackerConnectivity.h"

#include <string>

namespace smatchet {
namespace catalog {

/// The project key a pane's own catalog fetch, and its saved-snapshot fallback, are scoped to.
///
/// `connectivity` is the pane's own backend (null before its first sync created one). `ownViewQuery` is the
/// query of the view the pane's kick resolved for it, or "" when the kick resolved none: the caller's
/// configuration then carries the FOCUSED view's query, which is another pane's scope and is never used here.
///
/// Returns "" (the documented unscoped fallback) when there is no backend, no resolved view, or the query
/// names zero or several projects.
inline std::string PaneCatalogProjectKey(const ITrackerConnectivity* connectivity, const std::string& ownViewQuery) {
    if (connectivity == nullptr || ownViewQuery.empty()) {
        return std::string();
    }
    return connectivity->ExtractProjectFromQuery(ownViewQuery);
}

} // namespace catalog
} // namespace smatchet
