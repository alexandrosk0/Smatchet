#ifndef SMATCHET_UI_BULK_IMPORT_ABANDON_H
#define SMATCHET_UI_BULK_IMPORT_ABANDON_H

// Non-blocking abandon of the bulk-import per-row create futures
// (docs/plans/shipped/ui-freeze-pillar2-blocking.md, WS-A bucket-E / #734).
// Hoisted out of SmatchetBulkTicketsUi.cpp so the Pillar-2 regression guard
// (tests/Core/BulkImportAbandonNonBlocking.test.cpp) drives THIS code, not a copy
// of it (DR29). Templated on the session because naming the production session
// (`UiDrawSession`) drags ImGui + AppController into the including TU.

#include "CancelToken.h"

namespace smatchet {
namespace ui {

/**
 * Signal-then-abandon the in-flight bulk-import create futures. Destroying a
 * create future inline would join its worker and block the UI frame for the whole
 * network create + post-create refresh, so instead: flip the shared cancel flag
 * (the worker short-circuits at its next check), move the still-valid futures into
 * the session graveyard (drained at app teardown, off the hot path), and hand the
 * next run a fresh token. The caller never waits on a worker.
 *
 * `SessionT` must expose `bulkImportCancel` (a `CancelToken`) plus
 * `bulkImportFutures` / `bulkImportFutureGraveyard` (`std::vector<std::future<T>>`).
 */
template <typename SessionT> void BulkImportAbandonFutures(SessionT& d) {
    d.bulkImportCancel.Cancel();
    AbandonFuturesInto(d.bulkImportFutures, d.bulkImportFutureGraveyard);
    d.bulkImportCancel.Reset();
}

} // namespace ui
} // namespace smatchet

#endif // SMATCHET_UI_BULK_IMPORT_ABANDON_H
