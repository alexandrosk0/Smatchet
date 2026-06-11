#ifndef SMATCHET_P4_CL_PREVIEW_H
#define SMATCHET_P4_CL_PREVIEW_H

#include "ConfigManager.h"
#include "P4Annotate.h"

#include <string>

/**
 * Async changelist-preview tooltip, extracted from AnnotateAnalysisUi_Modals.cpp
 * so non-Annotate windows (User Info window) can reuse it. The hover slot +
 * describe cache live here (keyed by CL), no longer on the Annotate window's
 * pimpl singleton. UI-thread only, except Cache() which is thread-safe.
 */
namespace P4ClPreview {

/**
 * Draw the CL tooltip for the hovered cell. First call for a new `cl` kicks an
 * async `p4 describe` fetch (never blocks the UI thread); subsequent frames show
 * "Loading CL info..." until ready. Call between the hovered item and EndTooltip
 * scope rules — this opens/closes its own BeginTooltip/EndTooltip.
 */
void DrawClTooltipAsync(const std::string& cl, const AnnotateAnalysisConfig& cfg, const AnnotateUiThemeColors& theme);

/**
 * Detach an in-flight hover fetch (call when the owning window closes so the
 * future is not abandoned mid-flight). Detached futures are reaped by
 * ReapDetached().
 */
void DetachInFlight();

/** Reap completed detached hover futures. Call once per frame from a poll site. */
void ReapDetached();

/** Process-wide `p4 describe -s` cache shared by the tooltip and detail loaders. Thread-safe. */
P4ChangelistDescribeCache& Cache();

} // namespace P4ClPreview

#endif
