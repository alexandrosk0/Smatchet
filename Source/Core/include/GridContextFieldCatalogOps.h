#pragma once

// GridContextFieldCatalogOps — the two writes that reset a pane's field catalog state: the explicit
// clear a tracker or site switch starts with, and the banner a failed catalog refresh leaves. Kept out
// of AppController so a test can drive them on a bare GridContextFieldCatalog.

#include "CatalogOfflinePolicyPure.h"

#include <string>

struct GridContextFieldCatalog;

namespace smatchet {
namespace catalogops {

/// Supersedes every catalog fetch in flight for this pane (CatalogEpoch) and empties the in-memory
/// catalog, its project pin and its loaded/restored state. The offline snapshot is never touched.
/// `clearUsers` also drops the user roster. Takes the catalog mutex itself.
void ClearCatalogInMemoryInto(GridContextFieldCatalog& cat, bool clearUsers);

/// Writes the failure banner `banner` names into `cat`. Caller holds cat.availableFieldsMutex_. False
/// only when the banner already said this (the catalog revision then stays put).
bool WriteCatalogFailureBannerLocked(GridContextFieldCatalog& cat, catalogoffline::CatalogFailureBanner banner,
                                     const std::string& backendLabel, const std::string& error, bool errorTransient);

} // namespace catalogops
} // namespace smatchet
